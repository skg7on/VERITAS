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

#include "veritas/wpa/SccStateRepository.h"

#include <charconv>
#include <map>
#include <string>
#include <utility>

namespace veritas::wpa {
namespace {

StatusOr<std::size_t> ParseSize(const std::string& value) {
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return Status::Internal("stored SCC iteration count is invalid");
  }
  return parsed;
}

StatusOr<int> ParseInt(const std::string& value) {
  int parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return Status::Internal("stored SCC enum value is invalid");
  }
  return parsed;
}

Status RollbackWith(summarydb::MetadataStore& store, Status status) {
  auto rollback = store.RollbackTransaction();
  return rollback.ok() ? status : rollback;
}

}  // namespace

Status SccStateRepository::PublishGraph(const SccContext& context,
                                        const CallGraph& call_graph,
                                        const SccGraph& scc_graph) {
  auto begun = metadata_store_.BeginTransaction();
  if (!begun.ok()) return begun;
  for (std::string_view table : {"wpa_scc_edges", "wpa_scc_members",
                                 "wpa_sccs"}) {
    auto deleted = metadata_store_.Execute(
        "DELETE FROM " + std::string(table) +
            " WHERE revision_id = ? AND build_variant_id = ?",
        {context.revision_id, context.build_variant_id});
    if (!deleted.ok()) return RollbackWith(metadata_store_, deleted);
  }

  for (const auto& scc_id : scc_graph.ReverseTopologicalOrder()) {
    auto inserted = metadata_store_.Execute(
        "INSERT INTO wpa_sccs(scc_id, revision_id, build_variant_id) "
        "VALUES(?, ?, ?)",
        {core::ToString(scc_id), context.revision_id,
         context.build_variant_id});
    if (!inserted.ok()) return RollbackWith(metadata_store_, inserted);
    auto members = scc_graph.Members(scc_id);
    if (!members.ok()) return RollbackWith(metadata_store_, members.status());
    for (const auto& member : *members) {
      inserted = metadata_store_.Execute(
          "INSERT INTO wpa_scc_members(scc_id, revision_id, build_variant_id, "
          "function_variant_id) VALUES(?, ?, ?, ?)",
          {core::ToString(scc_id), context.revision_id,
           context.build_variant_id, core::ToString(member)});
      if (!inserted.ok()) return RollbackWith(metadata_store_, inserted);
    }
  }

  std::map<std::pair<core::StableId, core::StableId>,
           summary::v1::EpistemicState>
      edges;
  for (const auto& caller : call_graph.Functions()) {
    auto caller_scc = scc_graph.SccForFunction(caller);
    if (!caller_scc.ok()) {
      return RollbackWith(metadata_store_, caller_scc.status());
    }
    for (const auto& edge : call_graph.Outgoing(caller)) {
      auto callee_scc = scc_graph.SccForFunction(edge.callee);
      if (!callee_scc.ok()) {
        return RollbackWith(metadata_store_, callee_scc.status());
      }
      if (*caller_scc == *callee_scc) continue;
      auto [it, inserted] = edges.emplace(
          std::pair{*caller_scc, *callee_scc}, edge.epistemic);
      if (!inserted && edge.epistemic == summary::v1::EPISTEMIC_STATE_MAY) {
        it->second = summary::v1::EPISTEMIC_STATE_MAY;
      }
    }
  }
  for (const auto& [edge, epistemic] : edges) {
    auto inserted = metadata_store_.Execute(
        "INSERT INTO wpa_scc_edges(caller_scc_id, callee_scc_id, revision_id, "
        "build_variant_id, epistemic) VALUES(?, ?, ?, ?, ?)",
        {core::ToString(edge.first), core::ToString(edge.second),
         context.revision_id, context.build_variant_id,
         std::to_string(static_cast<int>(epistemic))});
    if (!inserted.ok()) return RollbackWith(metadata_store_, inserted);
  }
  return metadata_store_.CommitTransaction();
}

StatusOr<std::optional<StoredSccState>> SccStateRepository::LoadState(
    const SccContext& context, core::StableId scc_id,
    summary::v1::ComponentKind component_kind) const {
  auto rows = metadata_store_.Query(
      "SELECT input_hash, fixpoint_hash, externally_visible_hash, "
      "iteration_count, status FROM wpa_component_states WHERE scc_id = ? "
      "AND revision_id = ? AND build_variant_id = ? AND component_kind = ?",
      {core::ToString(scc_id), context.revision_id, context.build_variant_id,
       std::to_string(static_cast<int>(component_kind))});
  if (!rows.ok()) return rows.status();
  if (rows->empty()) return std::optional<StoredSccState>{};
  if (rows->size() != 1u || (*rows)[0].size() != 5u) {
    return Status::Internal("stored SCC state has an invalid row shape");
  }
  auto iterations = ParseSize((*rows)[0][3]);
  auto status_value = ParseInt((*rows)[0][4]);
  if (!iterations.ok()) return iterations.status();
  if (!status_value.ok()) return status_value.status();
  if (*status_value < static_cast<int>(SccStatus::kConverged) ||
      *status_value > static_cast<int>(SccStatus::kUnsupported)) {
    return Status::Internal("stored SCC status is out of range");
  }
  return std::optional<StoredSccState>{StoredSccState{
      .scc_id = std::move(scc_id),
      .component_kind = component_kind,
      .input_hash = (*rows)[0][0],
      .fixpoint_hash = (*rows)[0][1],
      .externally_visible_hash = (*rows)[0][2],
      .iteration_count = *iterations,
      .status = static_cast<SccStatus>(*status_value)}};
}

StatusOr<ExternalChange> SccStateRepository::StoreState(
    const SccContext& context, const SccResult& result) {
  auto begun = metadata_store_.BeginTransaction();
  if (!begun.ok()) return begun;
  auto previous = LoadState(context, result.scc_id, result.component_kind);
  if (!previous.ok()) {
    return RollbackWith(metadata_store_, previous.status());
  }
  auto stored = metadata_store_.Execute(
      "INSERT INTO wpa_component_states(scc_id, revision_id, build_variant_id, "
      "component_kind, input_hash, fixpoint_hash, externally_visible_hash, "
      "iteration_count, status) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(scc_id, revision_id, build_variant_id, component_kind) DO "
      "UPDATE SET input_hash=excluded.input_hash, "
      "fixpoint_hash=excluded.fixpoint_hash, "
      "externally_visible_hash=excluded.externally_visible_hash, "
      "iteration_count=excluded.iteration_count, status=excluded.status, "
      "updated_at=strftime('%s', 'now')",
      {core::ToString(result.scc_id), context.revision_id,
       context.build_variant_id,
       std::to_string(static_cast<int>(result.component_kind)),
       result.input_hash, result.fixpoint_hash, result.externally_visible_hash,
       std::to_string(result.iteration_count),
       std::to_string(static_cast<int>(result.status))});
  if (!stored.ok()) return RollbackWith(metadata_store_, stored);
  auto committed = metadata_store_.CommitTransaction();
  if (!committed.ok()) return committed;
  return !previous->has_value() ||
                 (*previous)->externally_visible_hash !=
                     result.externally_visible_hash
             ? ExternalChange::kChanged
             : ExternalChange::kUnchanged;
}

}  // namespace veritas::wpa
