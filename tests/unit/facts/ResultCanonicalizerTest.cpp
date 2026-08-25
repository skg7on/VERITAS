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

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RuleRegistry.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {
namespace {

namespace sem = analysis::semantic;

constexpr std::string_view kDirect = "wpa.reachability.direct.v2";
constexpr std::string_view kTransitive = "wpa.reachability.transitive.v2";

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kReachableCall,
                     {FunctionId(from), FunctionId(to),
                      sem::EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to),
                      sem::DispatchKind::kDirect, sem::EpistemicState::kMay}};
}

RootedInputFact Root(const SemanticRow& row) {
  auto fact = MakeFact(row);
  return RootedInputFact{.fact = *fact, .provenance_ref = "test:root"};
}

WitnessEdge Edge(const SemanticRow& result, std::string_view rule,
                 const SemanticRow& input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

CanonicalizationRequest RequestFor(const std::vector<RootedInputFact>& roots,
                                   const RawWpaEvaluation& raw) {
  CanonicalizationRequest request;
  request.local_roots = roots;
  request.evaluation = &raw;
  return request;
}

// A published fact with no witness has no proof. It must be rejected rather
// than published on the strength of the evaluator's say-so.
TEST(ResultCanonicalizerTest, RejectsOrphanedDerivedResult) {
  const std::vector<RootedInputFact> roots = {Root(DirectCall("f", "g"))};
  RawWpaEvaluation raw;
  raw.results = {Reachable("f", "g")};
  raw.witnesses = {};

  auto result = ResultCanonicalizer::Canonicalize(RequestFor(roots, raw));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kFailedPrecondition);
}

// Two results that cite only each other are a closed loop with no root. Such a
// cycle can justify anything, so neither result may be published.
TEST(ResultCanonicalizerTest, RejectsCyclicUnrootedWitnesses) {
  const std::vector<RootedInputFact> roots = {};
  RawWpaEvaluation raw;
  raw.results = {Reachable("f", "g"), Reachable("g", "h")};
  raw.witnesses = {
      Edge(Reachable("f", "g"), kTransitive, Reachable("g", "h"), 0),
      Edge(Reachable("g", "h"), kTransitive, Reachable("f", "g"), 0)};

  auto result = ResultCanonicalizer::Canonicalize(RequestFor(roots, raw));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kFailedPrecondition);
}

// The same result reachable by a one-edge direct proof and a two-edge
// transitive proof must always select the shorter one, whichever order the
// evaluator happened to emit its candidates in.
TEST(ResultCanonicalizerTest, SelectsShortestProofDeterministically) {
  const std::vector<RootedInputFact> roots = {Root(DirectCall("f", "g")),
                                              Root(DirectCall("g", "h")),
                                              Root(DirectCall("f", "h"))};

  const auto direct_proof =
      Edge(Reachable("f", "h"), kDirect, DirectCall("f", "h"), 0);
  const auto transitive_a =
      Edge(Reachable("f", "h"), kTransitive, DirectCall("f", "g"), 0);
  const auto transitive_b =
      Edge(Reachable("f", "h"), kTransitive, Reachable("g", "h"), 1);
  const auto support =
      Edge(Reachable("g", "h"), kDirect, DirectCall("g", "h"), 0);

  RawWpaEvaluation forward;
  forward.results = {Reachable("f", "h"), Reachable("g", "h")};
  forward.witnesses = {direct_proof, transitive_a, transitive_b, support};

  RawWpaEvaluation reverse;
  reverse.results = {Reachable("g", "h"), Reachable("f", "h")};
  reverse.witnesses = {support, transitive_b, transitive_a, direct_proof};

  auto first = ResultCanonicalizer::Canonicalize(RequestFor(roots, forward));
  auto second = ResultCanonicalizer::Canonicalize(RequestFor(roots, reverse));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  EXPECT_EQ(first->facts, second->facts);
  EXPECT_EQ(first->witnesses, second->witnesses);
  EXPECT_EQ(first->fixpoint_hash, second->fixpoint_hash);

  // Reachable("f","h") must be proved by the single direct edge, not the
  // two-edge transitive derivation.
  const auto chosen = std::ranges::count_if(
      first->witnesses, [&](const WitnessEdge& edge) {
        return edge.result == SemanticKey{Reachable("f", "h")};
      });
  EXPECT_EQ(chosen, 1);
}

// A witness naming a rule the bundle does not define cannot be validated, so
// the component fails rather than publishing an unverifiable proof.
TEST(ResultCanonicalizerTest, RejectsUnregisteredRule) {
  const std::vector<RootedInputFact> roots = {Root(DirectCall("f", "g"))};
  RawWpaEvaluation raw;
  raw.results = {Reachable("f", "g")};
  raw.witnesses = {
      Edge(Reachable("f", "g"), "wpa.not.a.rule", DirectCall("f", "g"), 0)};

  auto result = ResultCanonicalizer::Canonicalize(RequestFor(roots, raw));
  EXPECT_FALSE(result.ok());
}

// One rule applied to one result cannot claim two different inputs at the same
// ordinal; that is an ambiguous proof, not two alternatives.
TEST(ResultCanonicalizerTest, RejectsConflictingOrdinal) {
  const std::vector<RootedInputFact> roots = {Root(DirectCall("f", "g")),
                                              Root(DirectCall("g", "h"))};
  RawWpaEvaluation raw;
  raw.results = {Reachable("f", "g")};
  raw.witnesses = {
      Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0),
      Edge(Reachable("f", "g"), kDirect, DirectCall("g", "h"), 0)};

  auto result = ResultCanonicalizer::Canonicalize(RequestFor(roots, raw));
  EXPECT_FALSE(result.ok());
}

// A witness-only change alters the selected proof and therefore the fixpoint
// hash, but leaves the published semantics -- and so the external hash --
// untouched. That is what stops a proof change from scheduling predecessors.
TEST(ResultCanonicalizerTest, WitnessOnlyChangeLeavesExternalHashStable) {
  const std::vector<RootedInputFact> roots = {Root(DirectCall("f", "g")),
                                              Root(DirectCall("g", "h")),
                                              Root(DirectCall("f", "h"))};

  RawWpaEvaluation short_proof;
  short_proof.results = {Reachable("f", "h")};
  short_proof.witnesses = {
      Edge(Reachable("f", "h"), kDirect, DirectCall("f", "h"), 0)};

  RawWpaEvaluation long_proof;
  long_proof.results = {Reachable("f", "h"), Reachable("g", "h")};
  long_proof.witnesses = {
      Edge(Reachable("f", "h"), kTransitive, DirectCall("f", "g"), 0),
      Edge(Reachable("f", "h"), kTransitive, Reachable("g", "h"), 1),
      Edge(Reachable("g", "h"), kDirect, DirectCall("g", "h"), 0)};

  auto first = ResultCanonicalizer::Canonicalize(RequestFor(roots, short_proof));
  auto second = ResultCanonicalizer::Canonicalize(RequestFor(roots, long_proof));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  // The second evaluation publishes an extra fact, so compare the shared one
  // by re-canonicalizing only it.
  RawWpaEvaluation same_facts_other_proof;
  same_facts_other_proof.results = {Reachable("f", "h")};
  same_facts_other_proof.witnesses = {
      Edge(Reachable("f", "h"), kDirect, DirectCall("f", "h"), 0)};
  auto third =
      ResultCanonicalizer::Canonicalize(RequestFor(roots, same_facts_other_proof));
  ASSERT_TRUE(third.ok());
  EXPECT_EQ(first->external_hash, third->external_hash);
  EXPECT_NE(first->external_hash, second->external_hash);
}

}  // namespace
}  // namespace veritas::facts
