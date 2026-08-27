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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;
namespace sem = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId MemoryId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kMemoryRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

facts::AnalysisRunSemanticDescriptor Semantics() {
  facts::AnalysisRunSemanticDescriptor semantics;
  semantics.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  semantics.summary_schema_version = "summary.v2";
  semantics.relation_schema_version = "relations.v2";
  semantics.rule_bundle_version = "rules.v2";
  semantics.model_bundle_version = "models.v1";
  semantics.svf_configuration_hash = std::string(64, 'a');
  semantics.wpa_configuration_hash = std::string(64, 'b');
  return semantics;
}

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v2::FunctionSummary* summary, std::string_view from,
             std::string_view to, v1::EpistemicState epistemic) {
  auto* call = summary->add_calls();
  call->set_call_site_id(
      core::ToString(CallSiteId(std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(
      core::ToString(FunctionId(to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(epistemic);
  call->set_provenance_ref("test:call");
}

void AddWrite(v2::FunctionSummary* summary, std::string_view memory) {
  auto* effect = summary->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
  effect->mutable_location()->set_memory_location_id(
      core::ToString(MemoryId(memory)));
}

facts::SemanticRow Reachable(std::string_view from, std::string_view to,
                             sem::EpistemicState epistemic) {
  return facts::SemanticRow{
      facts::RelationId::kReachableCall,
      {FunctionId(from), FunctionId(to), epistemic}};
}

facts::SemanticRow MayWriteRow(std::string_view function,
                               std::string_view memory,
                               sem::EpistemicState epistemic) {
  return facts::SemanticRow{facts::RelationId::kMayWrite,
                            {FunctionId(function), MemoryId(memory),
                             epistemic}};
}

bool ContainsSemantic(const std::vector<facts::SemanticRow>& rows,
                      const facts::SemanticRow& wanted) {
  return std::ranges::find(rows, wanted) != rows.end();
}

bool ContainsWitness(const facts::RawWpaEvaluation& raw,
                     const facts::SemanticRow& result, std::string_view rule,
                     std::uint32_t ordinal) {
  return std::ranges::any_of(raw.witnesses, [&](const facts::WitnessEdge& e) {
    return e.result.row == result && e.rule_id == rule &&
           e.input_ordinal == ordinal;
  });
}

StatusOr<WpaLogicalComponentInput> InputFor(
    const std::vector<summary::SummaryArtifact>& artifacts,
    WpaComponentKind component, std::string_view root,
    std::span<const facts::AnalysisFact> support = {}) {
  auto graph = CallGraph::FromSummaries(artifacts);
  if (!graph.ok())
    return graph.status();
  auto scc = SccGraph::Build(*graph);
  if (!scc.ok())
    return scc.status();
  auto scc_id = scc->SccForFunction(FunctionId(root));
  if (!scc_id.ok())
    return scc_id.status();

  WpaMaterializationRequest request;
  request.semantics = Semantics();
  request.scc_id = *scc_id;
  request.component = component;
  request.summaries = artifacts;
  request.successor_support = support;
  return WpaInputMaterializer::Build(request);
}

// f and g are mutually recursive, so they share one SCC and both of their
// call edges are local to this component. g also calls h. Reaching h from f
// therefore requires the locally transitive rule, not just a direct edge.
std::vector<summary::SummaryArtifact> RecursiveReachabilityProgram() {
  auto f = V2Summary("f");
  AddCall(&f, "f", "g", v1::EPISTEMIC_STATE_MUST);
  auto g = V2Summary("g");
  AddCall(&g, "g", "f", v1::EPISTEMIC_STATE_MUST);
  AddCall(&g, "g", "h", v1::EPISTEMIC_STATE_MUST);
  return {f, g, V2Summary("h")};
}

TEST(CppRuleEvaluatorTest, DerivesReachabilityAndImmediateWitnesses) {
  const auto program = RecursiveReachabilityProgram();
  auto input = InputFor(program, WpaComponentKind::kReachability, "f");
  ASSERT_TRUE(input.ok());

  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());

  EXPECT_TRUE(ContainsSemantic(raw->results,
                               Reachable("f", "h", sem::EpistemicState::kMust)));
  // A two-input join emits one witness per argument position, so the proof
  // cannot be mistaken for two alternative single-input derivations.
  EXPECT_TRUE(ContainsWitness(*raw,
                              Reachable("f", "h", sem::EpistemicState::kMust),
                              "wpa.reachability.transitive.v2", 0));
  EXPECT_TRUE(ContainsWitness(*raw,
                              Reachable("f", "h", sem::EpistemicState::kMust),
                              "wpa.reachability.transitive.v2", 1));
}

// A call edge that is only MAY cannot yield a MUST conclusion: the result is
// never better warranted than the weakest step.
TEST(CppRuleEvaluatorTest, WeakensEpistemicAlongADerivation) {
  auto f = V2Summary("f");
  AddCall(&f, "f", "g", v1::EPISTEMIC_STATE_MAY);
  auto g = V2Summary("g");
  AddCall(&g, "g", "f", v1::EPISTEMIC_STATE_MUST);
  AddCall(&g, "g", "h", v1::EPISTEMIC_STATE_MUST);
  const std::vector<summary::SummaryArtifact> program = {f, g, V2Summary("h")};

  auto input = InputFor(program, WpaComponentKind::kReachability, "f");
  ASSERT_TRUE(input.ok());
  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());

  EXPECT_TRUE(ContainsSemantic(raw->results,
                               Reachable("f", "h", sem::EpistemicState::kMay)));
  EXPECT_FALSE(ContainsSemantic(
      raw->results, Reachable("f", "h", sem::EpistemicState::kMust)));
}

TEST(CppRuleEvaluatorTest, PropagatesMayWriteFromSuccessorSupport) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "caller", "target", v1::EPISTEMIC_STATE_MUST);
  const std::vector<summary::SummaryArtifact> program = {caller,
                                                         V2Summary("target")};

  auto support_row = MayWriteRow("target", "heap", sem::EpistemicState::kMust);
  auto support_fact = facts::MakeFact(support_row);
  ASSERT_TRUE(support_fact.ok());
  const std::vector<facts::AnalysisFact> support = {*support_fact};

  auto input =
      InputFor(program, WpaComponentKind::kMemoryEffects, "caller", support);
  ASSERT_TRUE(input.ok());
  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());

  EXPECT_TRUE(ContainsSemantic(
      raw->results, MayWriteRow("caller", "heap", sem::EpistemicState::kMust)));
  EXPECT_TRUE(ContainsWitness(
      *raw, MayWriteRow("caller", "heap", sem::EpistemicState::kMust),
      "wpa.memory.may_write.support.v2", 1));
}

// A member's own write is published directly.
TEST(CppRuleEvaluatorTest, DerivesMayWriteFromMemberWrite) {
  auto caller = V2Summary("caller");
  AddWrite(&caller, "heap");
  const std::vector<summary::SummaryArtifact> program = {caller};

  auto input = InputFor(program, WpaComponentKind::kMemoryEffects, "caller");
  ASSERT_TRUE(input.ok());
  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());

  EXPECT_TRUE(ContainsSemantic(
      raw->results, MayWriteRow("caller", "heap", sem::EpistemicState::kMust)));
}

// The evaluator's output must survive the canonicalizer: every result it
// asserts has to be groundable in the input's declared roots. This is the
// contract that lets a Souffle run and a C++ run be compared.
TEST(CppRuleEvaluatorTest, EvaluationCanonicalizesToRootedFacts) {
  const auto program = RecursiveReachabilityProgram();
  auto input = InputFor(program, WpaComponentKind::kReachability, "f");
  ASSERT_TRUE(input.ok());
  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());

  facts::CanonicalizationRequest request;
  request.local_roots = input->local_roots;
  request.successor_roots = input->successor_roots;
  request.evaluation = &*raw;

  auto canonical = facts::ResultCanonicalizer::Canonicalize(request);
  ASSERT_TRUE(canonical.ok()) << canonical.status().message();
  EXPECT_FALSE(canonical->facts.empty());
  EXPECT_FALSE(canonical->fixpoint_hash.empty());
  EXPECT_FALSE(canonical->external_hash.empty());
}

// Evaluation is a pure function of the logical input, so repeating it yields
// identical results and witnesses.
TEST(CppRuleEvaluatorTest, EvaluationIsDeterministic) {
  const auto program = RecursiveReachabilityProgram();
  auto input = InputFor(program, WpaComponentKind::kReachability, "f");
  ASSERT_TRUE(input.ok());

  auto first = CppRuleEvaluator().Evaluate(*input);
  auto second = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first->results, second->results);
  EXPECT_EQ(first->witnesses, second->witnesses);
}

}  // namespace
}  // namespace veritas::wpa
