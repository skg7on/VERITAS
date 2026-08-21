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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(
      core::IdKind::kFunctionVariant,
      std::as_bytes(std::span(name.data(), name.size())));
}

v1::FunctionSummary Function(std::string_view name) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v1::FunctionSummary* caller, std::string_view callee,
             v1::EpistemicState epistemic) {
  auto* call = caller->add_calls();
  call->set_callee_symbol(std::string(callee));
  call->set_resolved_callee_function_variant_id(
      core::ToString(FunctionId(callee)));
  call->set_call_site_anchor_id(
      "site:" + caller->identity().function_variant_id() + ":" +
      std::string(callee));
  call->set_epistemic(epistemic);
  call->set_provenance_ref("test:call");
}

void AddWrite(v1::FunctionSummary* function, std::string location,
              v1::EpistemicState epistemic = v1::EPISTEMIC_STATE_MUST) {
  auto* effect = function->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_location(std::move(location));
  effect->set_epistemic(epistemic);
  effect->set_provenance_ref("test:write");
}

struct ChainFixture {
  std::vector<v1::FunctionSummary> summaries;
  core::StableId a;
  core::StableId b;
  core::StableId c;
  std::string a_text;
};

ChainFixture MakeChainWithWrite(v1::EpistemicState first_edge =
                                    v1::EPISTEMIC_STATE_MUST) {
  auto a = Function("A");
  auto b = Function("B");
  auto c = Function("C");
  AddCall(&a, "B", first_edge);
  AddCall(&b, "C", v1::EPISTEMIC_STATE_MUST);
  AddWrite(&c, "X");
  return ChainFixture{.summaries = {std::move(a), std::move(b), std::move(c)},
                      .a = FunctionId("A"),
                      .b = FunctionId("B"),
                      .c = FunctionId("C"),
                      .a_text = core::ToString(FunctionId("A"))};
}

bool ContainsFact(std::span<const SccResult> results,
                  facts::FactRelation relation,
                  const std::vector<std::string>& columns,
                  v1::EpistemicState epistemic) {
  for (const auto& result : results) {
    for (const auto& fact : result.facts) {
      if (fact.relation == relation && fact.columns == columns &&
          fact.epistemic == epistemic) {
        return true;
      }
    }
  }
  return false;
}

TEST(FixpointEngineTest, DerivesMayWriteThroughThreeFunctionChain) {
  auto fixture = MakeChainWithWrite();
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());

  FixpointEngine engine(*graph, *scc, fixture.summaries);
  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                   FixpointBudget{.max_iterations = 32});
  ASSERT_TRUE(results.ok()) << results.status().message();
  EXPECT_TRUE(ContainsFact(*results, facts::FactRelation::kMayWrite,
                           {fixture.a_text, "X"},
                           v1::EPISTEMIC_STATE_MUST));
}

TEST(FixpointEngineTest, DerivesTransitiveReachableCall) {
  auto fixture = MakeChainWithWrite();
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, fixture.summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {1});
  ASSERT_TRUE(results.ok()) << results.status().message();
  EXPECT_TRUE(ContainsFact(
      *results, facts::FactRelation::kReachableCall,
      {fixture.a_text, core::ToString(fixture.c)},
      v1::EPISTEMIC_STATE_MUST));
}

TEST(FixpointEngineTest, EmptySupportedDomainsHaveDomainSeparatedHashes) {
  const std::vector summaries{Function("A")};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto calls = engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {1});
  auto memory =
      engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {1});
  ASSERT_TRUE(calls.ok());
  ASSERT_TRUE(memory.ok());
  ASSERT_EQ(calls->size(), 1u);
  ASSERT_EQ(memory->size(), 1u);
  EXPECT_NE((*calls)[0].input_hash, (*memory)[0].input_hash);
  EXPECT_NE((*calls)[0].fixpoint_hash, (*memory)[0].fixpoint_hash);
  EXPECT_NE((*calls)[0].externally_visible_hash,
            (*memory)[0].externally_visible_hash);
}

TEST(FixpointEngineTest, ComputeAllReturnsCalleesBeforeCallers) {
  auto fixture = MakeChainWithWrite();
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  const auto c_scc = *scc->SccForFunction(fixture.c);
  const auto b_scc = *scc->SccForFunction(fixture.b);
  const auto a_scc = *scc->SccForFunction(fixture.a);
  FixpointEngine engine(*graph, *scc, fixture.summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {32});
  ASSERT_TRUE(results.ok());
  ASSERT_EQ(results->size(), 3u);
  EXPECT_EQ((*results)[0].scc_id, c_scc);
  EXPECT_EQ((*results)[1].scc_id, b_scc);
  EXPECT_EQ((*results)[2].scc_id, a_scc);
}

TEST(FixpointEngineTest, SelfRecursiveFunctionConverges) {
  auto a = Function("A");
  AddCall(&a, "A", v1::EPISTEMIC_STATE_MUST);
  const std::vector summaries{a};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {1});
  ASSERT_TRUE(results.ok());
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ((*results)[0].status, SccStatus::kConverged);
  EXPECT_TRUE(ContainsFact(
      *results, facts::FactRelation::kReachableCall,
      {core::ToString(FunctionId("A")), core::ToString(FunctionId("A"))},
      v1::EPISTEMIC_STATE_MUST));
}

TEST(FixpointEngineTest, MutualRecursionConvergesAsOneScc) {
  auto a = Function("A");
  auto b = Function("B");
  AddCall(&a, "B", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "A", v1::EPISTEMIC_STATE_MUST);
  const std::vector summaries{a, b};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {32});
  ASSERT_TRUE(results.ok());
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ((*results)[0].status, SccStatus::kConverged);
  EXPECT_TRUE(ContainsFact(
      *results, facts::FactRelation::kReachableCall,
      {core::ToString(FunctionId("A")), core::ToString(FunctionId("A"))},
      v1::EPISTEMIC_STATE_MUST));
}

TEST(FixpointEngineTest, MayCallWeakensTransitiveMayWrite) {
  auto fixture = MakeChainWithWrite(v1::EPISTEMIC_STATE_MAY);
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, fixture.summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {32});
  ASSERT_TRUE(results.ok());
  EXPECT_TRUE(ContainsFact(*results, facts::FactRelation::kMayWrite,
                           {fixture.a_text, "X"},
                           v1::EPISTEMIC_STATE_MAY));
}

TEST(FixpointEngineTest, RecursiveBudgetExhaustionIsExplicitlyApproximated) {
  auto a = Function("A");
  auto b = Function("B");
  auto c = Function("C");
  AddCall(&a, "B", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "C", v1::EPISTEMIC_STATE_MUST);
  AddCall(&c, "A", v1::EPISTEMIC_STATE_MUST);
  AddWrite(&c, "X");
  const std::vector summaries{a, b, c};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {1});
  ASSERT_TRUE(results.ok());
  ASSERT_EQ(results->size(), 1u);
  EXPECT_EQ((*results)[0].status, SccStatus::kApproximated);
  EXPECT_TRUE(std::ranges::none_of((*results)[0].facts, [](const auto& fact) {
    return fact.epistemic == v1::EPISTEMIC_STATE_MUST;
  }));
  std::set<core::StableId> derived_ids;
  for (const auto& fact : (*results)[0].facts) {
    derived_ids.insert(fact.tuple_id);
  }
  for (const auto& fact : (*results)[0].facts) {
    if (fact.rule_id.find(".transitive.") == std::string::npos) continue;
    const auto derived_support_count = std::ranges::count_if(
        fact.input_tuple_ids,
        [&](const auto& input) { return derived_ids.contains(input); });
    EXPECT_EQ(derived_support_count, 1);
  }
}

TEST(FixpointEngineTest, ReorderedSummariesProduceIdenticalFactsAndHashes) {
  auto first_fixture = MakeChainWithWrite();
  auto second_summaries = first_fixture.summaries;
  std::ranges::reverse(second_summaries);
  auto first_graph = CallGraph::FromSummaries(first_fixture.summaries);
  auto second_graph = CallGraph::FromSummaries(second_summaries);
  ASSERT_TRUE(first_graph.ok());
  ASSERT_TRUE(second_graph.ok());
  auto first_scc = SccGraph::Build(*first_graph);
  auto second_scc = SccGraph::Build(*second_graph);
  ASSERT_TRUE(first_scc.ok());
  ASSERT_TRUE(second_scc.ok());
  FixpointEngine first(*first_graph, *first_scc, first_fixture.summaries);
  FixpointEngine second(*second_graph, *second_scc, second_summaries);

  auto first_results =
      first.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {32});
  auto second_results =
      second.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {32});
  ASSERT_TRUE(first_results.ok());
  ASSERT_TRUE(second_results.ok());
  ASSERT_EQ(first_results->size(), second_results->size());
  for (std::size_t index = 0; index < first_results->size(); ++index) {
    const auto& left = (*first_results)[index];
    const auto& right = (*second_results)[index];
    EXPECT_EQ(left.scc_id, right.scc_id);
    EXPECT_EQ(left.input_hash, right.input_hash);
    EXPECT_EQ(left.fixpoint_hash, right.fixpoint_hash);
    EXPECT_EQ(left.externally_visible_hash,
              right.externally_visible_hash);
    ASSERT_EQ(left.facts.size(), right.facts.size());
    for (std::size_t fact = 0; fact < left.facts.size(); ++fact) {
      EXPECT_EQ(left.facts[fact].tuple_id, right.facts[fact].tuple_id);
    }
  }
}

TEST(FixpointEngineTest, UnsupportedComponentReturnsExplicitStatus) {
  auto fixture = MakeChainWithWrite();
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, fixture.summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_RANGE_FACTS, {32});
  ASSERT_TRUE(results.ok());
  ASSERT_EQ(results->size(), 3u);
  for (const auto& result : *results) {
    EXPECT_EQ(result.status, SccStatus::kUnsupported);
    EXPECT_TRUE(result.facts.empty());
  }
}

}  // namespace
}  // namespace veritas::wpa
