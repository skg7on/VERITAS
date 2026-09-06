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
// collects FunctionId/MemoryId cells in column order; `value_ids` collects
// ValueId cells (the flow closure's endpoints); `epistemics` collects
// epistemic cells.
struct ResolvedRow {
  facts::SemanticRow semantic;
  std::vector<core::StableId> endpoint_ids;
  std::vector<core::StableId> value_ids;
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
    } else if (const auto* value_id = std::get_if<facts::ValueId>(&cell)) {
      auto stable = mappings.values.ToStable(*value_id);
      if (!stable.ok())
        return stable.status();
      resolved.value_ids.push_back(*stable);
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

// Evaluates the GlobalFlow closure: the transitive closure over value
// identity, seeded by LocalFlow/ParameterFlow/ReturnFlow and closed over the
// transitive and successor-support steps. Unlike the call-mediated closures,
// the transitive step joins two derived flows directly on a shared boundary
// value.
StatusOr<facts::RawWpaEvaluation> EvaluateFlow(
    const WpaLogicalComponentInput& input) {
  std::vector<std::pair<DerivedTuple, facts::SemanticRow>> base_rows;
  std::vector<std::pair<DerivedTuple, facts::SemanticRow>> support_rows;
  std::map<DerivedTuple, std::string> base_rule;

  for (const auto& row : input.edb) {
    const bool base = row.relation == facts::RelationId::kLocalFlow ||
                      row.relation == facts::RelationId::kParameterFlow ||
                      row.relation == facts::RelationId::kReturnFlow;
    if (!base && row.relation != facts::RelationId::kSupportGlobalFlow)
      continue;
    auto resolved = ResolveRow(row, input.mappings);
    if (!resolved.ok())
      return resolved.status();
    if (resolved->value_ids.size() < 2) {
      return Status::InvalidArgument("flow row is missing value endpoints");
    }
    const DerivedTuple tuple{resolved->value_ids[0], resolved->value_ids[1],
                             resolved->epistemics.front()};
    if (row.relation == facts::RelationId::kSupportGlobalFlow) {
      support_rows.emplace_back(tuple, resolved->semantic);
      continue;
    }
    base_rows.emplace_back(tuple, resolved->semantic);
    const char* rule = row.relation == facts::RelationId::kLocalFlow
                           ? "wpa.flow.global.local.v2"
                       : row.relation == facts::RelationId::kParameterFlow
                           ? "wpa.flow.global.parameter.v2"
                           : "wpa.flow.global.return.v2";
    base_rule.emplace(tuple, rule);
  }

  facts::RawWpaEvaluation raw;
  std::set<DerivedTuple> derived;
  auto ResultRow = [&](const DerivedTuple& tuple) {
    return facts::SemanticRow{facts::RelationId::kGlobalFlow,
                              {tuple.subject, tuple.object, tuple.epistemic}};
  };

  for (const auto& [tuple, row] : base_rows) {
    if (!WeaknessRank(tuple.epistemic).has_value())
      continue;
    if (derived.insert(tuple).second) {
      raw.witnesses.push_back(facts::WitnessEdge{
          .result = facts::SemanticKey{ResultRow(tuple)},
          .rule_id = base_rule.at(tuple),
          .derivation_key = facts::EncodeSemanticKey(row),
          .input = facts::SemanticKey{row},
          .input_ordinal = 0});
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    const std::vector<DerivedTuple> snapshot(derived.begin(), derived.end());

    // Transitive: compose two derived flows sharing a boundary value.
    for (const auto& a : snapshot) {
      for (const auto& b : snapshot) {
        if (a.object != b.subject)
          continue;
        const auto combined = Weaken(a.epistemic, b.epistemic);
        if (!combined.has_value())
          continue;
        const DerivedTuple next{a.subject, b.object, *combined};
        if (derived.insert(next).second)
          changed = true;
        const std::string derivation_key =
            facts::EncodeSemanticKey(ResultRow(a)) +
            facts::EncodeSemanticKey(ResultRow(b));
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{ResultRow(next)},
            .rule_id = "wpa.flow.global.transitive.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{ResultRow(a)},
            .input_ordinal = 0});
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{ResultRow(next)},
            .rule_id = "wpa.flow.global.transitive.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{ResultRow(b)},
            .input_ordinal = 1});
      }
    }

    // Support: a derived flow composed with a successor SCC's flow summary.
    for (const auto& a : snapshot) {
      for (const auto& [tuple, row] : support_rows) {
        if (a.object != tuple.subject)
          continue;
        const auto combined = Weaken(a.epistemic, tuple.epistemic);
        if (!combined.has_value())
          continue;
        const DerivedTuple next{a.subject, tuple.object, *combined};
        if (derived.insert(next).second)
          changed = true;
        const std::string derivation_key =
            facts::EncodeSemanticKey(ResultRow(a)) +
            facts::EncodeSemanticKey(row);
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{ResultRow(next)},
            .rule_id = "wpa.flow.global.support.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{ResultRow(a)},
            .input_ordinal = 0});
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{ResultRow(next)},
            .rule_id = "wpa.flow.global.support.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{row},
            .input_ordinal = 1});
      }
    }
  }

  for (const auto& tuple : derived) {
    raw.results.push_back(ResultRow(tuple));
  }
  std::ranges::sort(raw.witnesses);
  raw.witnesses.erase(std::unique(raw.witnesses.begin(), raw.witnesses.end()),
                      raw.witnesses.end());
  return raw;
}

// One unknown-effect tuple: the function carrying the unknown plus its
// subject/reason pair and warrant.
struct UnknownTuple {
  core::StableId function;
  std::string subject;
  std::string reason;
  sem::EpistemicState epistemic;

  auto operator<=>(const UnknownTuple&) const = default;
};

// Evaluates the UnknownEffect and SoundnessCoverage domains together: the
// unknown-effect closure seeds from unresolved calls and propagates up the
// call graph, then the coverage certificate marks each function complete or
// not based on the presence of an unknown effect.
StatusOr<facts::RawWpaEvaluation> EvaluateEffects(
    const WpaLogicalComponentInput& input) {
  std::vector<CallEdgeTuple> calls;
  std::vector<std::pair<UnknownTuple, facts::SemanticRow>> base_rows;
  std::vector<std::pair<UnknownTuple, facts::SemanticRow>> support_rows;
  std::map<core::StableId, std::string> function_stable;
  std::map<std::string, core::StableId> stable_to_function;
  std::set<core::StableId> unmodeled_externals;
  std::vector<std::pair<std::string, facts::SemanticRow>> unsupported_features;

  for (const auto& row : input.edb) {
    if (row.relation == facts::RelationId::kDirectCall) {
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
    } else if (row.relation == facts::RelationId::kUnknownCall) {
      auto resolved = ResolveRow(row, input.mappings);
      if (!resolved.ok())
        return resolved.status();
      if (resolved->endpoint_ids.empty()) {
        return Status::InvalidArgument("UnknownCall row is missing its caller");
      }
      const auto* site = std::get_if<core::StableId>(&resolved->semantic.cells[0]);
      const auto* reason = std::get_if<std::string>(&resolved->semantic.cells[2]);
      if (site == nullptr || reason == nullptr) {
        return Status::InvalidArgument("malformed UnknownCall row");
      }
      base_rows.emplace_back(
          UnknownTuple{resolved->endpoint_ids[0], core::ToString(*site),
                       *reason, resolved->epistemics.front()},
          resolved->semantic);
    } else if (row.relation == facts::RelationId::kSupportUnknownEffect) {
      auto resolved = ResolveRow(row, input.mappings);
      if (!resolved.ok())
        return resolved.status();
      if (resolved->endpoint_ids.empty()) {
        return Status::InvalidArgument(
            "SupportUnknownEffect row is missing its function");
      }
      const auto* subject = std::get_if<std::string>(&resolved->semantic.cells[1]);
      const auto* reason = std::get_if<std::string>(&resolved->semantic.cells[2]);
      if (subject == nullptr || reason == nullptr) {
        return Status::InvalidArgument("malformed SupportUnknownEffect row");
      }
      support_rows.emplace_back(
          UnknownTuple{resolved->endpoint_ids[0], *subject, *reason,
                       resolved->epistemics.front()},
          resolved->semantic);
    } else if (row.relation == facts::RelationId::kFunctionMap) {
      // FunctionMap carries no epistemic state, so resolve it directly rather
      // than through ResolveRow (which requires one).
      const auto* fn = std::get_if<facts::FunctionId>(&row.cells[0]);
      const auto* stable = std::get_if<std::string>(&row.cells[1]);
      if (fn == nullptr || stable == nullptr) {
        return Status::InvalidArgument("malformed FunctionMap row");
      }
      auto id = input.mappings.functions.ToStable(*fn);
      if (!id.ok())
        return id.status();
      function_stable[*id] = *stable;
      stable_to_function[*stable] = *id;
    } else if (row.relation == facts::RelationId::kUnmodeledExternal) {
      const auto* fn = std::get_if<facts::FunctionId>(&row.cells[0]);
      if (fn == nullptr) {
        return Status::InvalidArgument("malformed UnmodeledExternal row");
      }
      auto id = input.mappings.functions.ToStable(*fn);
      if (!id.ok())
        return id.status();
      unmodeled_externals.insert(*id);
    } else if (row.relation == facts::RelationId::kUnsupportedFeature) {
      const auto* node = std::get_if<std::string>(&row.cells[0]);
      const auto* kind = std::get_if<std::string>(&row.cells[1]);
      const auto* policy = std::get_if<std::string>(&row.cells[2]);
      if (node == nullptr || kind == nullptr || policy == nullptr) {
        return Status::InvalidArgument("malformed UnsupportedFeature row");
      }
      facts::SemanticRow semantic;
      semantic.relation = row.relation;
      semantic.cells = {*node, *kind, *policy};
      unsupported_features.emplace_back(*node, semantic);
    }
  }

  facts::RawWpaEvaluation raw;
  auto UnknownResultRow = [](const UnknownTuple& tuple) {
    return facts::SemanticRow{facts::RelationId::kUnknownEffect,
                              {tuple.function, tuple.subject, tuple.reason,
                               tuple.epistemic}};
  };
  auto CoverageResultRow = [](const std::string& scope,
                              std::uint64_t complete) {
    return facts::SemanticRow{
        facts::RelationId::kSoundnessCoverage,
        {scope, std::string("dominating_check_absence"), complete,
         sem::EpistemicState::kMust}};
  };

  std::set<UnknownTuple> derived;
  for (const auto& [tuple, row] : base_rows) {
    if (!WeaknessRank(tuple.epistemic).has_value())
      continue;
    if (derived.insert(tuple).second) {
      raw.witnesses.push_back(facts::WitnessEdge{
          .result = facts::SemanticKey{UnknownResultRow(tuple)},
          .rule_id = "wpa.effect.unknown.call.v2",
          .derivation_key = facts::EncodeSemanticKey(row),
          .input = facts::SemanticKey{row},
          .input_ordinal = 0});
    }
  }

  // Unmodeled external: a DirectCall to an unmodeled external callee.
  for (const auto& call : calls) {
    if (!unmodeled_externals.contains(call.callee))
      continue;
    auto stable_it = function_stable.find(call.callee);
    if (stable_it == function_stable.end())
      continue;
    const UnknownTuple tuple{call.caller, stable_it->second,
                             "unmodeled_external", sem::EpistemicState::kMay};
    if (derived.insert(tuple).second) {
      raw.witnesses.push_back(facts::WitnessEdge{
          .result = facts::SemanticKey{UnknownResultRow(tuple)},
          .rule_id = "wpa.effect.unknown.external.v2",
          .derivation_key = facts::EncodeSemanticKey(call.row),
          .input = facts::SemanticKey{call.row},
          .input_ordinal = 0});
    }
  }

  // Unsupported feature: seed from UnsupportedFeature resolved to a function.
  for (const auto& [node, row] : unsupported_features) {
    auto fn_it = stable_to_function.find(node);
    if (fn_it == stable_to_function.end())
      continue;
    const auto* kind = std::get_if<std::string>(&row.cells[1]);
    if (kind == nullptr)
      continue;
    const UnknownTuple tuple{fn_it->second, *kind, "unsupported_feature",
                             sem::EpistemicState::kUnknown};
    if (derived.insert(tuple).second) {
      raw.witnesses.push_back(facts::WitnessEdge{
          .result = facts::SemanticKey{UnknownResultRow(tuple)},
          .rule_id = "wpa.effect.unknown.feature.v2",
          .derivation_key = facts::EncodeSemanticKey(row),
          .input = facts::SemanticKey{row},
          .input_ordinal = 0});
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    const std::vector<UnknownTuple> snapshot(derived.begin(), derived.end());

    for (const auto& call : calls) {
      for (const auto& tuple : snapshot) {
        if (tuple.function != call.callee)
          continue;
        const auto combined = Weaken(call.epistemic, tuple.epistemic);
        if (!combined.has_value())
          continue;
        const UnknownTuple next{call.caller, tuple.subject, tuple.reason,
                                *combined};
        if (derived.insert(next).second)
          changed = true;
        const std::string derivation_key =
            facts::EncodeSemanticKey(call.row) +
            facts::EncodeSemanticKey(UnknownResultRow(tuple));
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{UnknownResultRow(next)},
            .rule_id = "wpa.effect.unknown.transitive.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{call.row},
            .input_ordinal = 0});
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{UnknownResultRow(next)},
            .rule_id = "wpa.effect.unknown.transitive.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{UnknownResultRow(tuple)},
            .input_ordinal = 1});
      }
    }

    for (const auto& call : calls) {
      for (const auto& [tuple, row] : support_rows) {
        if (tuple.function != call.callee)
          continue;
        const auto combined = Weaken(call.epistemic, tuple.epistemic);
        if (!combined.has_value())
          continue;
        const UnknownTuple next{call.caller, tuple.subject, tuple.reason,
                                *combined};
        if (derived.insert(next).second)
          changed = true;
        const std::string derivation_key =
            facts::EncodeSemanticKey(call.row) +
            facts::EncodeSemanticKey(row);
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{UnknownResultRow(next)},
            .rule_id = "wpa.effect.unknown.support.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{call.row},
            .input_ordinal = 0});
        raw.witnesses.push_back(facts::WitnessEdge{
            .result = facts::SemanticKey{UnknownResultRow(next)},
            .rule_id = "wpa.effect.unknown.support.v2",
            .derivation_key = derivation_key,
            .input = facts::SemanticKey{row},
            .input_ordinal = 1});
      }
    }
  }

  for (const auto& tuple : derived) {
    raw.results.push_back(UnknownResultRow(tuple));
  }

  // SoundnessCoverage: a function carrying an unknown effect is marked
  // incomplete (gapped); the closed-world "complete" state is the absence of
  // this fact at the query layer. Each unknown effect witnesses the gap, but
  // the fact itself is emitted once per function.
  std::set<std::string> emitted_scopes;
  for (const auto& tuple : derived) {
    const auto it = function_stable.find(tuple.function);
    if (it == function_stable.end())
      continue;
    const std::string& scope = it->second;
    raw.witnesses.push_back(facts::WitnessEdge{
        .result = facts::SemanticKey{CoverageResultRow(scope, 0)},
        .rule_id = "wpa.coverage.incomplete.v2",
        .derivation_key = facts::EncodeSemanticKey(UnknownResultRow(tuple)),
        .input = facts::SemanticKey{UnknownResultRow(tuple)},
        .input_ordinal = 0});
    if (emitted_scopes.insert(scope).second) {
      raw.results.push_back(CoverageResultRow(scope, 0));
    }
  }

  std::ranges::sort(raw.witnesses);
  raw.witnesses.erase(std::unique(raw.witnesses.begin(), raw.witnesses.end()),
                      raw.witnesses.end());
  return raw;
}

}  // namespace

StatusOr<facts::RawWpaEvaluation> CppRuleEvaluator::Evaluate(
    const WpaLogicalComponentInput& input) const {
  if (input.component == WpaComponentKind::kFlow) {
    return EvaluateFlow(input);
  }
  if (input.component == WpaComponentKind::kEffects) {
    return EvaluateEffects(input);
  }
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
