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

#include "veritas/facts/ResultCanonicalizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/facts/RuleRegistry.h"

namespace veritas::facts {

namespace {

constexpr std::uint64_t kUnproven = std::numeric_limits<std::uint64_t>::max();

void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

// One candidate derivation: a single rule applied to an ordered argument list.
// At most one derivation exists per (result, rule), because a rule cannot bind
// two different inputs at the same argument position.
struct Derivation {
  std::string rule_id;
  std::uint32_t priority = 0;
  // Input key -> ordinal, kept ordered so the tie-break on "lexicographic
  // stable input identity" is well defined.
  std::map<std::string, std::uint32_t> inputs;
  std::uint64_t cost = kUnproven;
};

std::string HashOf(const std::string& bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

}  // namespace

StatusOr<CanonicalizedResult> ResultCanonicalizer::Canonicalize(
    const CanonicalizationRequest& request) {
  if (request.evaluation == nullptr) {
    return Status::InvalidArgument("missing raw evaluation");
  }
  const RawWpaEvaluation& raw = *request.evaluation;

  // 1. Roots ground every proof. A root's key is what a witness edge cites.
  std::map<std::string, const RootedInputFact*> roots;
  for (const auto& span : {request.local_roots, request.successor_roots}) {
    for (const auto& root : span) {
      auto valid = ValidateSemanticRow(root.fact.row);
      if (!valid.ok())
        return valid;
      roots.emplace(EncodeSemanticKey(root.fact.row), &root);
    }
  }

  // 2. Index the asserted results. A witness may only speak about a result the
  // engine actually published.
  std::map<std::string, const SemanticRow*> results;
  for (const auto& row : raw.results) {
    auto valid = ValidateSemanticRow(row);
    if (!valid.ok())
      return valid;
    results.emplace(EncodeSemanticKey(row), &row);
  }

  // 3. Fold witness edges into derivations, rejecting anything unverifiable.
  std::map<std::string, std::map<std::string, Derivation>> derivations;
  for (const auto& edge : raw.witnesses) {
    const std::string result_key = EncodeSemanticKey(edge.result.row);
    if (!results.contains(result_key)) {
      return Status::InvalidArgument(
          "witness names a result that was not published");
    }
    const RuleSpec* rule = RulesV2().Find(edge.rule_id);
    if (rule == nullptr) {
      return Status::InvalidArgument("witness names an unregistered rule");
    }
    if (rule->result != edge.result.row.relation) {
      return Status::InvalidArgument(
          "witness rule does not derive this relation");
    }
    auto valid = ValidateSemanticRow(edge.input.row);
    if (!valid.ok())
      return valid;

    const std::string input_key = EncodeSemanticKey(edge.input.row);
    if (!roots.contains(input_key) && !results.contains(input_key)) {
      return Status::InvalidArgument(
          "witness cites an input that is neither a root nor a result");
    }

    Derivation& derivation = derivations[result_key][edge.rule_id];
    derivation.rule_id = edge.rule_id;
    derivation.priority = rule->priority;
    const auto [it, inserted] =
        derivation.inputs.emplace(input_key, edge.input_ordinal);
    if (!inserted && it->second != edge.input_ordinal) {
      return Status::InvalidArgument(
          "witness binds one input at two ordinals");
    }
    // Two different inputs at the same ordinal is an ambiguous proof, not two
    // alternative proofs: the rule has one argument in that position.
    for (const auto& [existing_key, existing_ordinal] : derivation.inputs) {
      if (existing_ordinal == edge.input_ordinal && existing_key != input_key) {
        return Status::InvalidArgument(
            "witness binds two inputs at one ordinal");
      }
    }
  }

  // 4. Relax derivation costs to a fixpoint. A root costs nothing; a
  // derivation costs one edge per input plus the cost of proving each derived
  // input. A result reachable only through a cycle never leaves kUnproven,
  // which is what rejects unrooted cycles without needing a cycle search.
  std::map<std::string, std::uint64_t> cost;
  for (const auto& [key, row] : results) {
    cost[key] = kUnproven;
  }
  for (std::size_t round = 0; round <= results.size(); ++round) {
    bool changed = false;
    for (auto& [result_key, by_rule] : derivations) {
      for (auto& [rule_id, derivation] : by_rule) {
        std::uint64_t total = 0;
        bool provable = true;
        for (const auto& [input_key, ordinal] : derivation.inputs) {
          total += 1;
          if (roots.contains(input_key))
            continue;
          const auto input_cost = cost.find(input_key);
          if (input_cost == cost.end() || input_cost->second == kUnproven) {
            provable = false;
            break;
          }
          total += input_cost->second;
        }
        if (!provable)
          continue;
        if (total < derivation.cost) {
          derivation.cost = total;
          changed = true;
        }
        auto& best = cost[result_key];
        if (total < best) {
          best = total;
          changed = true;
        }
      }
    }
    if (!changed)
      break;
  }

  // 5. Select one canonical proof per result: fewest derived edges, then lower
  // rule priority, then lexicographic input keys.
  CanonicalizedResult canonical;
  canonical.diagnostics = raw.diagnostics;
  std::vector<std::pair<std::string, const SemanticRow*>> ordered_results(
      results.begin(), results.end());

  for (const auto& [result_key, row] : ordered_results) {
    const auto by_rule = derivations.find(result_key);
    if (by_rule == derivations.end() || cost[result_key] == kUnproven) {
      return Status::FailedPrecondition(
          "published result has no finite proof rooted in declared inputs");
    }
    const Derivation* selected = nullptr;
    for (const auto& [rule_id, derivation] : by_rule->second) {
      if (derivation.cost == kUnproven)
        continue;
      if (selected == nullptr || derivation.cost < selected->cost ||
          (derivation.cost == selected->cost &&
           derivation.priority < selected->priority)) {
        selected = &derivation;
      }
      // Equal cost and equal priority fall back to the ordered rule id, which
      // the map iteration already supplies deterministically.
    }
    if (selected == nullptr) {
      return Status::FailedPrecondition(
          "published result has no finite proof rooted in declared inputs");
    }

    auto fact = MakeFact(*row);
    if (!fact.ok())
      return fact.status();
    canonical.facts.push_back(std::move(*fact));

    for (const auto& [input_key, ordinal] : selected->inputs) {
      const SemanticRow* input_row = nullptr;
      if (const auto root = roots.find(input_key); root != roots.end()) {
        input_row = &root->second->fact.row;
      } else {
        input_row = results.find(input_key)->second;
      }
      canonical.witnesses.push_back(
          WitnessEdge{.result = SemanticKey{*row},
                      .rule_id = selected->rule_id,
                      .input = SemanticKey{*input_row},
                      .input_ordinal = ordinal});
    }
  }

  // 6. Canonical order, then the two hashes. Sorting by encoded key keeps the
  // output independent of the order the engine emitted results in.
  std::ranges::sort(canonical.facts, [](const AnalysisFact& a,
                                        const AnalysisFact& b) {
    return EncodeSemanticKey(a.row) < EncodeSemanticKey(b.row);
  });
  std::ranges::sort(canonical.witnesses, [](const WitnessEdge& a,
                                            const WitnessEdge& b) {
    const auto a_result = EncodeSemanticKey(a.result.row);
    const auto b_result = EncodeSemanticKey(b.result.row);
    if (a_result != b_result)
      return a_result < b_result;
    if (a.rule_id != b.rule_id)
      return a.rule_id < b.rule_id;
    if (a.input_ordinal != b.input_ordinal)
      return a.input_ordinal < b.input_ordinal;
    return EncodeSemanticKey(a.input.row) < EncodeSemanticKey(b.input.row);
  });

  // ExternalHash covers only what a predecessor can see: the published
  // semantics. Witness edges are deliberately excluded so re-proving a fact
  // does not schedule predecessors that cannot observe the difference.
  std::string external_bytes;
  AppendField(&external_bytes, "veritas.wpa.external.v1");
  for (const auto& fact : canonical.facts) {
    AppendField(&external_bytes, EncodeSemanticKey(fact.row));
  }
  canonical.external_hash = HashOf(external_bytes);

  std::string fixpoint_bytes;
  AppendField(&fixpoint_bytes, "veritas.wpa.fixpoint.v1");
  AppendField(&fixpoint_bytes, canonical.external_hash);
  for (const auto& edge : canonical.witnesses) {
    AppendField(&fixpoint_bytes, EncodeSemanticKey(edge.result.row));
    AppendField(&fixpoint_bytes, edge.rule_id);
    AppendField(&fixpoint_bytes, EncodeSemanticKey(edge.input.row));
    AppendField(&fixpoint_bytes, std::to_string(edge.input_ordinal));
  }
  canonical.fixpoint_hash = HashOf(fixpoint_bytes);
  return canonical;
}

}  // namespace veritas::facts
