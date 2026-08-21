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

#include "veritas/wpa/FixpointEngine.h"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/ComponentHash.h"
#include "veritas/summary/FunctionSummary.h"
#include "veritas/wpa/FixpointDomain.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

constexpr std::string_view kReachableDirectRule =
    "m8.reachable.direct.v1";
constexpr std::string_view kReachableTransitiveRule =
    "m8.reachable.transitive.v1";
constexpr std::string_view kMayWriteDirectRule =
    "m8.may_write.direct.v1";
constexpr std::string_view kMayWriteTransitiveRule =
    "m8.may_write.transitive.v1";

bool IsSupported(v1::ComponentKind component_kind) {
  return component_kind == v1::COMPONENT_KIND_CALLS ||
         component_kind == v1::COMPONENT_KIND_MEMORY_EFFECTS;
}

bool IsPositive(v1::EpistemicState epistemic) {
  return epistemic == v1::EPISTEMIC_STATE_MUST ||
         epistemic == v1::EPISTEMIC_STATE_MAY;
}

void AppendField(std::string* output, std::string_view value) {
  output->append(std::to_string(value.size()));
  output->push_back(':');
  output->append(value);
}

std::string HashFields(std::string_view version,
                       std::vector<std::string> fields) {
  std::ranges::sort(fields);
  std::string canonical;
  AppendField(&canonical, version);
  for (const auto& field : fields) AppendField(&canonical, field);
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(canonical.data(), canonical.size()))));
}

std::string FactField(const facts::FactTuple& fact, bool include_proof) {
  std::string field;
  auto relation_name = facts::FactRelationName(fact.relation);
  if (relation_name.ok()) AppendField(&field, *relation_name);
  for (const auto& column : fact.columns) AppendField(&field, column);
  AppendField(&field, std::to_string(static_cast<int>(fact.epistemic)));
  if (include_proof) {
    AppendField(&field, core::ToString(fact.tuple_id));
    AppendField(&field, fact.rule_id);
    for (const auto& input : fact.input_tuple_ids) {
      AppendField(&field, core::ToString(input));
    }
  }
  return field;
}

std::vector<facts::FactTuple> FactsForSource(
    const FactDomain& domain, facts::FactRelation relation,
    std::string_view source) {
  std::vector<facts::FactTuple> matches;
  for (const auto& [key, domain_fact] : domain) {
    static_cast<void>(key);
    const auto& fact = domain_fact.tuple;
    if (fact.relation == relation && !fact.columns.empty() &&
        fact.columns[0] == source) {
      matches.push_back(fact);
    }
  }
  std::ranges::sort(matches, {}, &facts::FactTuple::tuple_id);
  return matches;
}

bool IsRecursiveScc(const CallGraph& call_graph,
                    std::span<const core::StableId> members) {
  if (members.size() > 1u) return true;
  if (members.empty()) return false;
  return std::ranges::any_of(call_graph.Outgoing(members[0]),
                             [&](const CallEdge& edge) {
    return edge.callee == members[0];
  });
}

StatusOr<std::vector<facts::FactTuple>> WeakenApproximatedFacts(
    const FactDomain& domain) {
  std::vector<facts::FactTuple> pending;
  std::set<core::StableId> local_ids;
  for (const auto& [key, domain_fact] : domain) {
    static_cast<void>(key);
    pending.push_back(domain_fact.tuple);
    local_ids.insert(domain_fact.tuple.tuple_id);
  }
  std::ranges::sort(pending, {}, &facts::FactTuple::tuple_id);

  std::map<core::StableId, core::StableId> replacements;
  std::vector<facts::FactTuple> weakened_facts;
  weakened_facts.reserve(pending.size());
  while (!pending.empty()) {
    bool made_progress = false;
    auto current = pending.begin();
    while (current != pending.end()) {
      std::vector<core::StableId> inputs = current->input_tuple_ids;
      bool ready = true;
      for (auto& input : inputs) {
        if (!local_ids.contains(input)) continue;
        auto replacement = replacements.find(input);
        if (replacement == replacements.end()) {
          ready = false;
          break;
        }
        input = replacement->second;
      }
      if (!ready) {
        ++current;
        continue;
      }

      auto weakened = facts::MakeDerivedFact(
          current->relation, current->columns, v1::EPISTEMIC_STATE_MAY,
          current->rule_id, std::move(inputs));
      if (!weakened.ok()) return weakened.status();
      replacements.emplace(current->tuple_id, weakened->tuple_id);
      weakened_facts.push_back(std::move(*weakened));
      current = pending.erase(current);
      made_progress = true;
    }
    if (!made_progress) {
      return Status::FailedPrecondition(
          "approximated fact provenance contains a cyclic local proof");
    }
  }
  return weakened_facts;
}

}  // namespace

FixpointEngine::FixpointEngine(
    const CallGraph& call_graph, const SccGraph& scc_graph,
    std::span<const v1::FunctionSummary> summaries)
    : call_graph_(call_graph), scc_graph_(scc_graph) {
  for (const auto& summary : summaries) {
    auto function_id =
        core::ParseStableId(summary.identity().function_variant_id());
    if (!function_id.ok()) {
      initialization_status_ = function_id.status();
      return;
    }
    if (function_id->kind != core::IdKind::kFunctionVariant) {
      initialization_status_ = Status::InvalidArgument(
          "fixpoint summary identity is not a function-variant ID");
      return;
    }
    if (!summaries_.emplace(*function_id, summary).second) {
      initialization_status_ = Status::InvalidArgument(
          "fixpoint summaries contain a duplicate function variant");
      return;
    }
  }
  for (const auto& function : call_graph_.Functions()) {
    if (!summaries_.contains(function)) {
      initialization_status_ = Status::FailedPrecondition(
          "call graph function has no current summary");
      return;
    }
  }
}

StatusOr<std::vector<SccResult>> FixpointEngine::ComputeAll(
    v1::ComponentKind component_kind, FixpointBudget budget) {
  if (!initialization_status_.ok()) return initialization_status_;
  std::vector<SccResult> results;
  results.reserve(scc_graph_.ReverseTopologicalOrder().size());
  for (const auto& scc_id : scc_graph_.ReverseTopologicalOrder()) {
    auto result = Compute(scc_id, component_kind, budget);
    if (!result.ok()) return result.status();
    results.push_back(std::move(*result));
  }
  return results;
}

StatusOr<SccResult> FixpointEngine::Compute(
    core::StableId scc_id, v1::ComponentKind component_kind,
    FixpointBudget budget) {
  if (!initialization_status_.ok()) return initialization_status_;
  auto members = scc_graph_.Members(scc_id);
  if (!members.ok()) return members.status();
  if (!IsSupported(component_kind)) {
    return SccResult{.scc_id = std::move(scc_id),
                     .component_kind = component_kind,
                     .input_hash = {},
                     .fixpoint_hash = {},
                     .externally_visible_hash = {},
                     .iteration_count = 0,
                     .status = SccStatus::kUnsupported,
                     .facts = {}};
  }
  if (budget.max_iterations == 0u) {
    return Status::InvalidArgument(
        "fixpoint budget must allow at least one iteration");
  }

  const auto key = std::pair{scc_id, component_kind};
  auto cached = cache_.find(key);
  if (cached != cache_.end() &&
      (cached->second.result.status == SccStatus::kConverged ||
       cached->second.max_iterations >= budget.max_iterations)) {
    return cached->second.result;
  }

  auto successors = scc_graph_.Successors(scc_id);
  if (!successors.ok()) return successors.status();
  for (const auto& successor : *successors) {
    auto successor_result = Compute(successor, component_kind, budget);
    if (!successor_result.ok()) return successor_result.status();
  }

  auto result = Evaluate(scc_id, component_kind, budget);
  if (!result.ok()) return result.status();
  cache_[key] = CacheEntry{*result, budget.max_iterations};
  return result;
}

StatusOr<SccResult> FixpointEngine::Evaluate(
    core::StableId scc_id, v1::ComponentKind component_kind,
    FixpointBudget budget) {
  auto members_result = scc_graph_.Members(scc_id);
  if (!members_result.ok()) return members_result.status();
  const auto members = *members_result;

  FactDomain local;
  FactDomain available;
  std::vector<std::string> input_fields;
  for (const auto& member : members) {
    const auto& summary = summaries_.at(member);
    const auto digest = summary::ComputeComponentDigest(component_kind, summary);
    input_fields.push_back("member:" + core::ToString(member) + ":" +
                           core::DigestToHex(digest.semantic_hash));
    for (const auto& edge : call_graph_.Outgoing(member)) {
      input_fields.push_back(
          "edge:" + core::ToString(edge.caller) + ":" +
          core::ToString(edge.callee) + ":" + edge.call_site_anchor_id + ":" +
          std::to_string(static_cast<int>(edge.epistemic)));
    }
    for (const auto& unknown : call_graph_.UnknownCalls(member)) {
      input_fields.push_back("unknown:" + core::ToString(member) + ":" +
                             unknown.call_site_anchor_id + ":" +
                             unknown.callee_symbol);
    }
  }

  auto successors = scc_graph_.Successors(scc_id);
  if (!successors.ok()) return successors.status();
  for (const auto& successor : *successors) {
    const auto cache_key = std::pair{successor, component_kind};
    auto cached = cache_.find(cache_key);
    if (cached == cache_.end()) {
      return Status::Internal("successor SCC result is missing from cache");
    }
    input_fields.push_back("successor:" + core::ToString(successor) + ":" +
                           cached->second.result.externally_visible_hash);
    for (const auto& fact : cached->second.result.facts) {
      auto joined = JoinFact(fact, &available);
      if (!joined.ok()) return joined.status();
    }
  }

  auto join_local = [&](facts::FactTuple fact) -> StatusOr<bool> {
    auto local_change = JoinFact(fact, &local);
    if (!local_change.ok()) return local_change.status();
    if (*local_change) {
      auto available_change = JoinFact(std::move(fact), &available);
      if (!available_change.ok()) return available_change.status();
    }
    return *local_change;
  };

  for (const auto& member : members) {
    const auto& summary = summaries_.at(member);
    auto summary_id = summary::ComputeFunctionSummaryId(summary);
    if (!summary_id.ok()) return summary_id.status();
    const std::string member_text = core::ToString(member);

    if (component_kind == v1::COMPONENT_KIND_CALLS) {
      for (const auto& edge : call_graph_.Outgoing(member)) {
        auto base = facts::MakeBaseFact(
            facts::FactRelation::kDirectCall,
            {member_text, core::ToString(edge.callee)}, edge.epistemic,
            facts::BaseFactOrigin{*summary_id, edge.call_site_anchor_id,
                                  edge.provenance_ref});
        if (!base.ok()) return base.status();
        auto direct = facts::MakeDerivedFact(
            facts::FactRelation::kReachableCall,
            {member_text, core::ToString(edge.callee)}, edge.epistemic,
            std::string(kReachableDirectRule), {base->tuple_id});
        if (!direct.ok()) return direct.status();
        auto joined = join_local(std::move(*direct));
        if (!joined.ok()) return joined.status();
      }
    } else {
      for (const auto& effect : summary.memory_effects()) {
        if (effect.kind() != v1::EFFECT_KIND_WRITE ||
            !IsPositive(effect.epistemic())) {
          continue;
        }
        auto base = facts::MakeBaseFact(
            facts::FactRelation::kDirectWrite,
            {member_text, effect.location()}, effect.epistemic(),
            facts::BaseFactOrigin{*summary_id, effect.location(),
                                  effect.provenance_ref()});
        if (!base.ok()) return base.status();
        auto direct = facts::MakeDerivedFact(
            facts::FactRelation::kMayWrite,
            {member_text, effect.location()}, effect.epistemic(),
            std::string(kMayWriteDirectRule), {base->tuple_id});
        if (!direct.ok()) return direct.status();
        auto joined = join_local(std::move(*direct));
        if (!joined.ok()) return joined.status();
      }
    }
  }

  const bool recursive = IsRecursiveScc(call_graph_, members);
  std::size_t iterations = 0;
  bool converged = false;
  for (; iterations < budget.max_iterations; ++iterations) {
    bool changed = false;
    for (const auto& member : members) {
      const auto& summary = summaries_.at(member);
      auto summary_id = summary::ComputeFunctionSummaryId(summary);
      if (!summary_id.ok()) return summary_id.status();
      const std::string caller_text = core::ToString(member);
      for (const auto& edge : call_graph_.Outgoing(member)) {
        auto base_call = facts::MakeBaseFact(
            facts::FactRelation::kDirectCall,
            {caller_text, core::ToString(edge.callee)}, edge.epistemic,
            facts::BaseFactOrigin{*summary_id, edge.call_site_anchor_id,
                                  edge.provenance_ref});
        if (!base_call.ok()) return base_call.status();
        const auto relation =
            component_kind == v1::COMPONENT_KIND_CALLS
                ? facts::FactRelation::kReachableCall
                : facts::FactRelation::kMayWrite;
        for (const auto& callee_fact : FactsForSource(
                 available, relation, core::ToString(edge.callee))) {
          auto epistemic = facts::WeakenPositiveEpistemic(
              edge.epistemic, callee_fact.epistemic);
          if (!epistemic.ok()) return epistemic.status();
          auto derived = facts::MakeDerivedFact(
              relation, {caller_text, callee_fact.columns[1]}, *epistemic,
              std::string(component_kind == v1::COMPONENT_KIND_CALLS
                              ? kReachableTransitiveRule
                              : kMayWriteTransitiveRule),
              {base_call->tuple_id, callee_fact.tuple_id});
          if (!derived.ok()) return derived.status();
          auto joined = join_local(std::move(*derived));
          if (!joined.ok()) return joined.status();
          changed = changed || *joined;
        }
      }
    }

    if (!recursive || !changed) {
      ++iterations;
      converged = true;
      break;
    }
  }
  const SccStatus status =
      converged ? SccStatus::kConverged : SccStatus::kApproximated;

  std::vector<facts::FactTuple> result_facts;
  if (status == SccStatus::kApproximated) {
    auto weakened = WeakenApproximatedFacts(local);
    if (!weakened.ok()) return weakened.status();
    result_facts = std::move(*weakened);
  } else {
    result_facts.reserve(local.size());
    for (const auto& [key, domain_fact] : local) {
      static_cast<void>(key);
      result_facts.push_back(domain_fact.tuple);
    }
  }
  std::ranges::sort(result_facts, {}, &facts::FactTuple::tuple_id);

  std::vector<std::string> fixpoint_fields;
  std::vector<std::string> external_fields;
  fixpoint_fields.reserve(result_facts.size());
  external_fields.reserve(result_facts.size());
  for (const auto& fact : result_facts) {
    fixpoint_fields.push_back(FactField(fact, true));
    external_fields.push_back(FactField(fact, false));
  }
  const std::string component_field =
      "component:" + std::to_string(static_cast<int>(component_kind));
  input_fields.push_back(component_field);
  fixpoint_fields.push_back(component_field);
  external_fields.push_back(component_field);

  return SccResult{
      .scc_id = std::move(scc_id),
      .component_kind = component_kind,
      .input_hash = HashFields("veritas.wpa.input.v1", std::move(input_fields)),
      .fixpoint_hash =
          HashFields("veritas.wpa.fixpoint.v1", std::move(fixpoint_fields)),
      .externally_visible_hash =
          HashFields("veritas.wpa.external.v1", std::move(external_fields)),
      .iteration_count = iterations,
      .status = status,
      .facts = std::move(result_facts)};
}

}  // namespace veritas::wpa
