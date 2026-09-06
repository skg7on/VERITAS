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

#include "veritas/wpa/CppRuleEvaluator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/facts/RelationSchema.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

// Rank of an epistemic state by how weak its warrant is. This is the same
// order as logic/common/epistemic.dl and must stay in step with it: MUST is
// observed and definite, MAY observed as possible, INFERRED derived from
// analysed evidence, ASSUMED resting on an unverified external model, and
// UNKNOWN carrying no information.
//
// MUST_NOT has no rank. It is a negative statement and never participates in
// a positive derivation, so a join meeting it yields no tuple rather than
// coercing it into a positive state.
std::optional<int> WeaknessRank(sem::EpistemicState state) {
  switch (state) {
  case sem::EpistemicState::kMust:
    return 0;
  case sem::EpistemicState::kMay:
    return 1;
  case sem::EpistemicState::kInferred:
    return 2;
  case sem::EpistemicState::kAssumed:
    return 3;
  case sem::EpistemicState::kUnknown:
    return 4;
  case sem::EpistemicState::kMustNot:
    return std::nullopt;
  }
  return std::nullopt;
}

// A conclusion is never better warranted than the weakest step used to reach
// it.
std::optional<sem::EpistemicState> Weaken(sem::EpistemicState left,
                                          sem::EpistemicState right) {
  const auto left_rank = WeaknessRank(left);
  const auto right_rank = WeaknessRank(right);
  if (!left_rank.has_value() || !right_rank.has_value())
    return std::nullopt;
  return *left_rank >= *right_rank ? left : right;
}

// One derived tuple: a pair of subjects plus the warrant it was derived at.
// Both memory domains have the shape (function, memory); reachability is
// (function, function). One representation serves all of them.
struct DerivedTuple {
  core::StableId subject;
  core::StableId object;
  sem::EpistemicState epistemic;

  auto operator<=>(const DerivedTuple&) const = default;
};

struct CallEdgeTuple {
  core::StableId caller;
  core::StableId callee;
  sem::EpistemicState epistemic;
  facts::SemanticRow row;  // the DirectCall row, cited as a witness input
};

// One closure domain a component evaluates: its base EDB relation, its
// successor-SCC support relation, the IDB relation it derives, and the witness
// rule-id prefix.
struct Domain {
  facts::RelationId base_relation;
  facts::RelationId support_relation;
  facts::RelationId derived_relation;
  std::string rule_prefix;
};

// The domains a component evaluates. Reachability has one; the memory-effects
// component evaluates the write and read closures together over the same call
// edges.
std::vector<Domain> DomainsFor(WpaComponentKind component) {
  if (component == WpaComponentKind::kReachability) {
    return {{facts::RelationId::kDirectCall,
             facts::RelationId::kSupportReachableCall,
             facts::RelationId::kReachableCall, "wpa.reachability."}};
  }
  return {
      {facts::RelationId::kDirectWrite, facts::RelationId::kSupportMayWrite,
       facts::RelationId::kMayWrite, "wpa.memory.may_write."},
      {facts::RelationId::kDirectRead, facts::RelationId::kSupportMayRead,
       facts::RelationId::kMayRead, "wpa.memory.may_read."},
  };
}

// The cells of an execution row resolved back to semantic form. `endpoint_ids`
// collects FunctionId/MemoryId cells in column order; `epistemics` collects
// epistemic cells. Both are what the closure rules join on.
struct ResolvedRow {
  facts::SemanticRow semantic;
  std::vector<core::StableId> endpoint_ids;
  std::vector<sem::EpistemicState> epistemics;
};

// Resolves dense cells through the component's mappings. Dense ids are
// run-local, so every cell is mapped back before it can appear in a result or
// a witness key.
StatusOr<ResolvedRow> ResolveRow(const facts::ExecutionRow& row,
                                 const StableIdMappings& mappings) {
  const auto& schema = facts::RelationsV2().Get(row.relation);
  if (row.cells.size() != schema.columns.size()) {
    return Status::InvalidArgument("execution row does not match its schema");
  }

  ResolvedRow resolved;
  resolved.semantic.relation = row.relation;
  for (const auto& cell : row.cells) {
    if (const auto* function = std::get_if<facts::FunctionId>(&cell)) {
      auto stable = mappings.functions.ToStable(*function);
      if (!stable.ok())
        return stable.status();
      resolved.endpoint_ids.push_back(*stable);
      resolved.semantic.cells.push_back(*stable);
    } else if (const auto* memory_id = std::get_if<facts::MemoryId>(&cell)) {
      auto stable = mappings.memories.ToStable(*memory_id);
      if (!stable.ok())
        return stable.status();
      resolved.endpoint_ids.push_back(*stable);
      resolved.semantic.cells.push_back(*stable);
    } else if (const auto* site = std::get_if<facts::CallSiteId>(&cell)) {
      auto stable = mappings.call_sites.ToStable(*site);
      if (!stable.ok())
        return stable.status();
      resolved.semantic.cells.push_back(*stable);
    } else if (const auto* state = std::get_if<sem::EpistemicState>(&cell)) {
      resolved.epistemics.push_back(*state);
      resolved.semantic.cells.push_back(*state);
    } else {
      std::visit(
          [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, facts::FunctionId> ||
                          std::is_same_v<T, facts::MemoryId> ||
                          std::is_same_v<T, facts::CallSiteId> ||
                          std::is_same_v<T, facts::ValueId> ||
                          std::is_same_v<T, facts::FactId>) {
              // Handled above; ValueId/FactId do not appear in these
              // relations.
            } else {
              resolved.semantic.cells.push_back(value);
            }
          },
          cell);
    }
  }
  if (resolved.epistemics.empty()) {
    return Status::InvalidArgument("row carries no epistemic state");
  }
  return resolved;
}

}  // namespace

StatusOr<facts::RawWpaEvaluation> CppRuleEvaluator::Evaluate(
    const WpaLogicalComponentInput& input) const {
  const auto domains = DomainsFor(input.component);

  // 1. Parse the DirectCall edges once; they seed the transitive closure for
  // every domain this component evaluates.
  std::vector<CallEdgeTuple> calls;
  for (const auto& row : input.edb) {
    if (row.relation != facts::RelationId::kDirectCall)
      continue;
    auto resolved = ResolveRow(row, input.mappings);
    if (!resolved.ok())
      return resolved.status();
    if (resolved->endpoint_ids.size() < 2) {
      return Status::InvalidArgument("DirectCall row is missing endpoints");
    }
    calls.push_back(CallEdgeTuple{.caller = resolved->endpoint_ids[0],
                                  .callee = resolved->endpoint_ids[1],
                                  .epistemic = resolved->epistemics.front(),
                                  .row = resolved->semantic});
  }

  // 2. Evaluate each domain over the shared call edges. Derived tuples are kept
  // in a set so the fixpoint terminates and the output order is independent of
  // discovery order.
  facts::RawWpaEvaluation raw;
  for (const auto& domain : domains) {
    std::vector<std::pair<DerivedTuple, facts::SemanticRow>> base_rows;
    std::vector<std::pair<DerivedTuple, facts::SemanticRow>> support_rows;

    for (const auto& row : input.edb) {
      if (row.relation != domain.base_relation &&
          row.relation != domain.support_relation) {
        continue;
      }
      auto resolved = ResolveRow(row, input.mappings);
      if (!resolved.ok())
        return resolved.status();
      if (resolved->endpoint_ids.size() < 2) {
        return Status::InvalidArgument("base row is missing subjects");
      }
      const DerivedTuple tuple{resolved->endpoint_ids[0],
                               resolved->endpoint_ids[1],
                               resolved->epistemics.front()};
      if (row.relation == domain.base_relation) {
        base_rows.emplace_back(tuple, resolved->semantic);
      } else {
        support_rows.emplace_back(tuple, resolved->semantic);
      }
    }

    std::set<DerivedTuple> derived;

    auto ResultRow = [&](const DerivedTuple& tuple) {
      return facts::SemanticRow{domain.derived_relation,
                                {tuple.subject, tuple.object, tuple.epistemic}};
    };

    for (const auto& [tuple, row] : base_rows) {
      // The derived tuple keeps the base row's own warrant.
      if (!WeaknessRank(tuple.epistemic).has_value())
        continue;
      if (derived.insert(tuple).second) {
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{ResultRow(tuple)},
            .rule_id = domain.rule_prefix + "direct.v2",
            .derivation_key = facts::EncodeSemanticKey(row),
            .input = facts::SemanticKey{row},
            .input_ordinal = 0});
      }
    }

    // Support tuples are inputs, not results: they are never inserted into
    // `derived` on their own, only combined with a call edge below.
    bool changed = true;
    while (changed) {
      changed = false;

      // Transitive: a call edge composed with an already-derived tuple.
      const std::vector<DerivedTuple> snapshot(derived.begin(), derived.end());
      for (const auto& call : calls) {
        for (const auto& tuple : snapshot) {
          if (tuple.subject != call.callee)
            continue;
          const auto combined = Weaken(call.epistemic, tuple.epistemic);
          if (!combined.has_value())
            continue;
          const DerivedTuple next{call.caller, tuple.object, *combined};
          if (!derived.insert(next).second)
            continue;
          changed = true;
          const std::string derivation_key =
              facts::EncodeSemanticKey(call.row) +
              facts::EncodeSemanticKey(ResultRow(tuple));
          raw.witnesses.push_back(facts::WitnessEdge{
              .result = facts::SemanticKey{ResultRow(next)},
              .rule_id = domain.rule_prefix + "transitive.v2",
              .derivation_key = derivation_key,
              .input = facts::SemanticKey{call.row},
              .input_ordinal = 0});
          raw.witnesses.push_back(facts::WitnessEdge{
              .result = facts::SemanticKey{ResultRow(next)},
              .rule_id = domain.rule_prefix + "transitive.v2",
              .derivation_key = derivation_key,
              .input = facts::SemanticKey{ResultRow(tuple)},
              .input_ordinal = 1});
        }
      }

      // Support: a call edge composed with a successor SCC's result.
      for (const auto& call : calls) {
        for (const auto& [tuple, row] : support_rows) {
          if (tuple.subject != call.callee)
            continue;
          const auto combined = Weaken(call.epistemic, tuple.epistemic);
          if (!combined.has_value())
            continue;
          const DerivedTuple next{call.caller, tuple.object, *combined};
          if (!derived.insert(next).second)
            continue;
          changed = true;
          const std::string derivation_key =
              facts::EncodeSemanticKey(call.row) +
              facts::EncodeSemanticKey(row);
          raw.witnesses.push_back(facts::WitnessEdge{
              .result = facts::SemanticKey{ResultRow(next)},
              .rule_id = domain.rule_prefix + "support.v2",
              .derivation_key = derivation_key,
              .input = facts::SemanticKey{call.row},
              .input_ordinal = 0});
          raw.witnesses.push_back(facts::WitnessEdge{
              .result = facts::SemanticKey{ResultRow(next)},
              .rule_id = domain.rule_prefix + "support.v2",
              .derivation_key = derivation_key,
              .input = facts::SemanticKey{row},
              .input_ordinal = 1});
        }
      }
    }

    for (const auto& tuple : derived) {
      raw.results.push_back(ResultRow(tuple));
    }
  }

  std::ranges::sort(raw.witnesses);
  raw.witnesses.erase(std::unique(raw.witnesses.begin(), raw.witnesses.end()),
                      raw.witnesses.end());
  return raw;
}

}  // namespace veritas::wpa
