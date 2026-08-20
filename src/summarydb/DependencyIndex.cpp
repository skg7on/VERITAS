// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/summarydb/DependencyIndex.h"

#include <charconv>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {

namespace {

std::string ComponentKey(core::StableId id, summary::v1::ComponentKind kind) {
  return core::ToString(id) + '\x1f' + std::to_string(static_cast<int>(kind));
}

std::string ConsumerKey(const ConsumerRef& ref) {
  return core::ToString(ref.consumer_id) + '\x1f' +
         std::to_string(static_cast<int>(ref.consumer_component));
}

// Non-throwing integer parse (VERITAS builds with -fno-exceptions). Returns
// nullopt on invalid input or trailing characters.
std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

Sensitivity SensitivityFromInt(int value) {
  switch (value) {
    case static_cast<int>(Sensitivity::kSemantic):
      return Sensitivity::kSemantic;
    case static_cast<int>(Sensitivity::kEvidenceOnly):
      return Sensitivity::kEvidenceOnly;
    case static_cast<int>(Sensitivity::kIdentity):
      return Sensitivity::kIdentity;
    default:
      return Sensitivity::kConfiguration;
  }
}

}  // namespace

DependencyIndex::DependencyIndex(MetadataStore& metadata_store)
    : metadata_store_(metadata_store) {}

Status DependencyIndex::ReplaceCurrentDependencies(
    core::StableId consumer_summary_id,
    const std::vector<DependencyEdge>& edges) {
  const std::string consumer_id = core::ToString(consumer_summary_id);

  auto begin = metadata_store_.BeginTransaction();
  if (!begin.ok()) {
    return begin;
  }

  // Remove stale current rows for this consumer, then insert the new current
  // rows and append historical rows, all in one transaction.
  auto remove = metadata_store_.Execute(
      "DELETE FROM reverse_dependency_index WHERE consumer_id = ?",
      {consumer_id});
  if (!remove.ok()) {
    metadata_store_.RollbackTransaction();
    return remove;
  }

  for (const auto& edge : edges) {
    auto insert_current = metadata_store_.Execute(
        "INSERT OR REPLACE INTO reverse_dependency_index (consumer_id, "
        "consumer_component, producer_id, producer_component, sensitivity) "
        "VALUES (?, ?, ?, ?, ?)",
        {consumer_id, std::to_string(static_cast<int>(edge.consumer_component)),
         core::ToString(edge.producer_id),
         std::to_string(static_cast<int>(edge.producer_component)),
         std::to_string(static_cast<int>(edge.sensitivity))});
    if (!insert_current.ok()) {
      metadata_store_.RollbackTransaction();
      return insert_current;
    }

    auto insert_historical = metadata_store_.Execute(
        "INSERT INTO summary_dependencies (consumer_id, consumer_component, "
        "producer_id, producer_component, dependency_kind, sensitivity) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        {consumer_id, std::to_string(static_cast<int>(edge.consumer_component)),
         core::ToString(edge.producer_id),
         std::to_string(static_cast<int>(edge.producer_component)),
         std::to_string(static_cast<int>(edge.dependency_kind)),
         std::to_string(static_cast<int>(edge.sensitivity))});
    if (!insert_historical.ok()) {
      metadata_store_.RollbackTransaction();
      return insert_historical;
    }
  }

  return metadata_store_.CommitTransaction();
}

StatusOr<std::vector<ConsumerRef>> DependencyIndex::UsersOf(
    core::StableId producer_id,
    summary::v1::ComponentKind producer_component) const {
  auto query = metadata_store_.Query(
      "SELECT consumer_id, consumer_component, sensitivity "
      "FROM reverse_dependency_index "
      "WHERE producer_id = ? AND producer_component = ? "
      "ORDER BY consumer_id, consumer_component",
      {core::ToString(producer_id),
       std::to_string(static_cast<int>(producer_component))});
  if (!query.ok()) {
    return query.status();
  }

  std::vector<ConsumerRef> consumers;
  for (const auto& row : *query) {
    auto id = core::ParseStableId(row[0]);
    if (!id.ok()) {
      return id.status();
    }
    auto component_kind = ParseInt(row[1]);
    auto sensitivity = ParseInt(row[2]);
    if (!component_kind || !sensitivity) {
      return Status::Internal(
          "Invalid component_kind or sensitivity in reverse_dependency_index");
    }
    consumers.push_back(ConsumerRef{
        *id,
        static_cast<summary::v1::ComponentKind>(*component_kind),
        SensitivityFromInt(*sensitivity),
    });
  }
  return consumers;
}

StatusOr<ImpactGraph> DependencyIndex::GetImpactSet(
    const SummaryDelta& delta, ImpactBudget budget) const {
  ImpactGraph graph;

  // Seed the frontier with each changed component's producer, carrying the
  // sensitivity scope that determines which edges it may follow.
  struct FrontierItem {
    core::StableId id;
    summary::v1::ComponentKind component;
    Sensitivity scope;
  };
  std::map<std::string, FrontierItem> frontier;
  for (const auto& component : delta.changed_components) {
    const Sensitivity scope = component.SemanticChanged()
                                  ? Sensitivity::kSemantic
                                  : Sensitivity::kEvidenceOnly;
    frontier.emplace(
        ComponentKey(delta.old_summary_id, component.component_kind),
        FrontierItem{delta.old_summary_id, component.component_kind, scope});
  }

  // Collect consumers keyed by (consumer_id, consumer_component) so duplicate
  // invalidation paths collapse, and the map yields deterministic ordering.
  std::map<std::string, ConsumerRef> collected;

  for (std::size_t depth = 0; !frontier.empty() && !graph.truncated; ++depth) {
    if (depth >= budget.max_depth) {
      graph.truncated = true;
      break;
    }

    std::map<std::string, FrontierItem> next_frontier;
    for (const auto& [key, item] : frontier) {
      if (graph.truncated) {
        break;
      }
      auto users = UsersOf(item.id, item.component);
      if (!users.ok()) {
        return users.status();
      }
      for (const auto& consumer : *users) {
        ++graph.explored_edges;
        // Only follow edges whose sensitivity matches this delta's scope: a
        // semantic delta must not schedule evidence-only consumers and vice
        // versa.
        if (consumer.sensitivity != item.scope) {
          continue;
        }
        const std::string consumer_key = ConsumerKey(consumer);
        if (collected.find(consumer_key) == collected.end()) {
          if (collected.size() >= budget.max_consumers) {
            graph.truncated = true;
            break;
          }
          collected.emplace(consumer_key, consumer);
        }
        next_frontier.emplace(
            ComponentKey(consumer.consumer_id, consumer.consumer_component),
            FrontierItem{consumer.consumer_id, consumer.consumer_component,
                         item.scope});
      }
    }
    frontier = std::move(next_frontier);
  }

  graph.consumers.reserve(collected.size());
  for (const auto& [key, ref] : collected) {
    graph.consumers.push_back(ref);
  }
  return graph;
}

}  // namespace veritas::summarydb
