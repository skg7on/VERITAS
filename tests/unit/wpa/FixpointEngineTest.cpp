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
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

v1::FunctionSummary Function(std::string_view name) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v1::FunctionSummary *caller, std::string_view callee,
             v1::EpistemicState epistemic) {
  auto *call = caller->add_calls();
  call->set_callee_symbol(std::string(callee));
  call->set_resolved_callee_function_variant_id(
      core::ToString(FunctionId(callee)));
  call->set_call_site_anchor_id(
      "site:" + caller->identity().function_variant_id() + ":" +
      std::string(callee));
  call->set_epistemic(epistemic);
  call->set_provenance_ref("test:call");
}

void AddUnknownCall(v1::FunctionSummary *caller, std::string anchor,
                    std::string symbol, std::string provenance) {
  auto *call = caller->add_calls();
  call->set_callee_symbol(std::move(symbol));
  call->set_call_site_anchor_id(std::move(anchor));
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  call->set_provenance_ref(std::move(provenance));
}

void AddWrite(v1::FunctionSummary *function, std::string location,
              v1::EpistemicState epistemic = v1::EPISTEMIC_STATE_MUST) {
  auto *effect = function->add_memory_effects();
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

ChainFixture
MakeChainWithWrite(v1::EpistemicState first_edge = v1::EPISTEMIC_STATE_MUST) {
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
                  const std::vector<std::string> &columns,
                  v1::EpistemicState epistemic) {
  for (const auto &result : results) {
    for (const auto &fact : result.facts) {
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
                           {fixture.a_text, "X"}, v1::EPISTEMIC_STATE_MUST));
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
  EXPECT_TRUE(ContainsFact(*results, facts::FactRelation::kReachableCall,
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
  auto memory = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {1});
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

TEST(FixpointEngineTest, RecursiveAlternateProofsHaveClosedProvenance) {
  auto a = Function("A");
  auto b = Function("B");
  auto c = Function("C");
  AddCall(&a, "B", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "A", v1::EPISTEMIC_STATE_MUST);
  AddCall(&a, "C", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "C", v1::EPISTEMIC_STATE_MUST);
  AddWrite(&c, "X");
  const std::vector summaries{a, b, c};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {32});
  ASSERT_TRUE(results.ok()) << results.status().message();

  std::set<core::StableId> available_ids;
  for (const auto &base : engine.BaseFacts())
    available_ids.insert(base.tuple_id);
  for (const auto &result : *results) {
    for (const auto &fact : result.facts)
      available_ids.insert(fact.tuple_id);
  }
  for (const auto &result : *results) {
    for (const auto &fact : result.facts) {
      for (const auto &input : fact.input_tuple_ids) {
        EXPECT_TRUE(available_ids.contains(input));
      }
    }
  }
}

TEST(FixpointEngineTest, RecursiveWeakeningRetainsStrongerProofSupport) {
  auto a = Function("A");
  auto b = Function("B");
  AddWrite(&a, "X");
  AddCall(&a, "B", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "A", v1::EPISTEMIC_STATE_MAY);
  const std::vector summaries{a, b};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);

  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {32});
  ASSERT_TRUE(results.ok()) << results.status().message();
  EXPECT_TRUE(ContainsFact(*results, facts::FactRelation::kMayWrite,
                           {core::ToString(FunctionId("A")), "X"},
                           v1::EPISTEMIC_STATE_MAY));
  EXPECT_TRUE(ContainsFact(*results, facts::FactRelation::kMayWrite,
                           {core::ToString(FunctionId("B")), "X"},
                           v1::EPISTEMIC_STATE_MAY));

  std::set<core::StableId> available_ids;
  for (const auto &base : engine.BaseFacts())
    available_ids.insert(base.tuple_id);
  for (const auto &result : *results) {
    for (const auto &fact : result.facts)
      available_ids.insert(fact.tuple_id);
  }
  for (const auto &result : *results) {
    for (const auto &fact : result.facts) {
      for (const auto &input : fact.input_tuple_ids)
        EXPECT_TRUE(available_ids.contains(input));
    }
  }
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
                           {fixture.a_text, "X"}, v1::EPISTEMIC_STATE_MAY));
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
  EXPECT_TRUE(std::ranges::none_of((*results)[0].facts, [](const auto &fact) {
    return fact.epistemic == v1::EPISTEMIC_STATE_MUST;
  }));
  std::set<core::StableId> derived_ids;
  for (const auto &fact : (*results)[0].facts) {
    derived_ids.insert(fact.tuple_id);
  }
  for (const auto &fact : (*results)[0].facts) {
    if (fact.rule_id.find(".transitive.") == std::string::npos)
      continue;
    const auto derived_support_count =
        std::ranges::count_if(fact.input_tuple_ids, [&](const auto &input) {
          return derived_ids.contains(input);
        });
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
    const auto &left = (*first_results)[index];
    const auto &right = (*second_results)[index];
    EXPECT_EQ(left.scc_id, right.scc_id);
    EXPECT_EQ(left.input_hash, right.input_hash);
    EXPECT_EQ(left.fixpoint_hash, right.fixpoint_hash);
    EXPECT_EQ(left.externally_visible_hash, right.externally_visible_hash);
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
  for (const auto &result : *results) {
    EXPECT_EQ(result.status, SccStatus::kUnsupported);
    EXPECT_TRUE(result.facts.empty());
  }
}

TEST(FixpointEngineTest, LargerBudgetRefreshesConvergedPredecessor) {
  auto root = Function("root");
  auto a = Function("A");
  auto b = Function("B");
  auto c = Function("C");
  AddCall(&root, "A", v1::EPISTEMIC_STATE_MUST);
  AddCall(&a, "B", v1::EPISTEMIC_STATE_MUST);
  AddCall(&b, "C", v1::EPISTEMIC_STATE_MUST);
  AddCall(&c, "A", v1::EPISTEMIC_STATE_MUST);
  AddWrite(&c, "X");
  const std::vector summaries{root, a, b, c};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);
  const auto root_scc = *scc->SccForFunction(FunctionId("root"));

  auto approximated = engine.Compute(
      root_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS, {.max_iterations = 1});
  ASSERT_TRUE(approximated.ok()) << approximated.status().message();
  EXPECT_FALSE(ContainsFact(std::span<const SccResult>(&*approximated, 1),
                            facts::FactRelation::kMayWrite,
                            {core::ToString(FunctionId("root")), "X"},
                            v1::EPISTEMIC_STATE_MUST));

  auto converged = engine.Compute(root_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                  {.max_iterations = 32});
  ASSERT_TRUE(converged.ok()) << converged.status().message();
  EXPECT_TRUE(ContainsFact(std::span<const SccResult>(&*converged, 1),
                           facts::FactRelation::kMayWrite,
                           {core::ToString(FunctionId("root")), "X"},
                           v1::EPISTEMIC_STATE_MUST));
}

TEST(FixpointEngineTest, SmallerBudgetReusesConvergedRecursiveResult) {
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
  const auto recursive_scc = *scc->SccForFunction(FunctionId("A"));

  auto converged = engine.Compute(
      recursive_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS, {.max_iterations = 32});
  ASSERT_TRUE(converged.ok()) << converged.status().message();
  ASSERT_EQ(converged->status, SccStatus::kConverged);

  auto reused = engine.Compute(recursive_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
                               {.max_iterations = 1});
  ASSERT_TRUE(reused.ok()) << reused.status().message();
  EXPECT_EQ(reused->status, SccStatus::kConverged);
  EXPECT_EQ(reused->fixpoint_hash, converged->fixpoint_hash);
}

TEST(FixpointEngineTest,
     ProofCacheRefreshesWhenSuccessorSupportChangesWithoutExternalChange) {
  std::vector<std::string> names{"A", "B", "C"};
  std::ranges::sort(names, [](const auto &left, const auto &right) {
    return FunctionId(left) < FunctionId(right);
  });
  const std::string &source_name = names[0];
  const std::string &middle_name = names[1];
  const std::string &sink_name = names[2];

  auto root = Function("root");
  auto source = Function(source_name);
  auto middle = Function(middle_name);
  auto sink = Function(sink_name);
  AddCall(&root, source_name, v1::EPISTEMIC_STATE_MUST);
  AddCall(&source, sink_name, v1::EPISTEMIC_STATE_MUST);
  AddCall(&source, middle_name, v1::EPISTEMIC_STATE_MAY);
  AddCall(&middle, sink_name, v1::EPISTEMIC_STATE_MUST);
  AddCall(&sink, source_name, v1::EPISTEMIC_STATE_MUST);
  AddWrite(&sink, "X");
  const std::vector summaries{root, source, middle, sink};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  FixpointEngine engine(*graph, *scc, summaries);
  const auto root_scc = *scc->SccForFunction(FunctionId("root"));
  const auto successor_scc = *scc->SccForFunction(FunctionId(source_name));

  auto low_root = engine.Compute(root_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                 {.max_iterations = 1});
  ASSERT_TRUE(low_root.ok()) << low_root.status().message();
  auto low_successor = engine.Compute(
      successor_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS, {.max_iterations = 1});
  ASSERT_TRUE(low_successor.ok()) << low_successor.status().message();
  ASSERT_EQ(low_successor->status, SccStatus::kApproximated);

  auto high = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                {.max_iterations = 32});
  ASSERT_TRUE(high.ok()) << high.status().message();
  const auto high_successor =
      std::ranges::find_if(*high, [&](const auto &result) {
        return result.scc_id == successor_scc;
      });
  ASSERT_NE(high_successor, high->end());
  EXPECT_EQ(high_successor->externally_visible_hash,
            low_successor->externally_visible_hash);
  EXPECT_NE(high_successor->fixpoint_hash, low_successor->fixpoint_hash);

  std::set<core::StableId> available_ids;
  for (const auto &base : engine.BaseFacts())
    available_ids.insert(base.tuple_id);
  for (const auto &result : *high) {
    for (const auto &fact : result.facts)
      available_ids.insert(fact.tuple_id);
  }
  for (const auto &result : *high) {
    for (const auto &fact : result.facts) {
      for (const auto &input : fact.input_tuple_ids)
        EXPECT_TRUE(available_ids.contains(input));
    }
  }
}

TEST(FixpointEngineTest, ProvenanceOnlyChangeUpdatesInputButNotExternalHash) {
  auto first_summaries = MakeChainWithWrite().summaries;
  auto second_summaries = first_summaries;
  second_summaries[0].mutable_calls(0)->set_provenance_ref("test:changed");
  auto first_graph = CallGraph::FromSummaries(first_summaries);
  auto second_graph = CallGraph::FromSummaries(second_summaries);
  ASSERT_TRUE(first_graph.ok());
  ASSERT_TRUE(second_graph.ok());
  auto first_scc = SccGraph::Build(*first_graph);
  auto second_scc = SccGraph::Build(*second_graph);
  ASSERT_TRUE(first_scc.ok());
  ASSERT_TRUE(second_scc.ok());
  FixpointEngine first(*first_graph, *first_scc, first_summaries);
  FixpointEngine second(*second_graph, *second_scc, second_summaries);

  auto left = first.Compute(*first_scc->SccForFunction(FunctionId("A")),
                            v1::COMPONENT_KIND_CALLS, {32});
  auto right = second.Compute(*second_scc->SccForFunction(FunctionId("A")),
                              v1::COMPONENT_KIND_CALLS, {32});
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());
  EXPECT_NE(left->input_hash, right->input_hash);
  EXPECT_NE(left->fixpoint_hash, right->fixpoint_hash);
  EXPECT_EQ(left->externally_visible_hash, right->externally_visible_hash);
}

TEST(FixpointEngineTest, UnknownCallInputHashUsesUnambiguousFields) {
  auto first_summary = Function("A");
  auto second_summary = Function("A");
  AddUnknownCall(&first_summary, "a:b", "c", "d");
  AddUnknownCall(&second_summary, "a", "b", "c:d");
  const std::vector first_summaries{first_summary};
  const std::vector second_summaries{second_summary};
  auto first_graph = CallGraph::FromSummaries(first_summaries);
  auto second_graph = CallGraph::FromSummaries(second_summaries);
  ASSERT_TRUE(first_graph.ok());
  ASSERT_TRUE(second_graph.ok());
  auto first_scc = SccGraph::Build(*first_graph);
  auto second_scc = SccGraph::Build(*second_graph);
  ASSERT_TRUE(first_scc.ok());
  ASSERT_TRUE(second_scc.ok());
  FixpointEngine first(*first_graph, *first_scc, first_summaries);
  FixpointEngine second(*second_graph, *second_scc, second_summaries);

  auto left = first.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {1});
  auto right = second.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS, {1});
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());
  ASSERT_EQ(left->size(), 1u);
  ASSERT_EQ(right->size(), 1u);
  EXPECT_NE((*left)[0].input_hash, (*right)[0].input_hash);
  EXPECT_EQ((*left)[0].externally_visible_hash,
            (*right)[0].externally_visible_hash);
}

TEST(FixpointEngineTest, MissingCurrentSummaryReturnsNotFound) {
  const std::vector summaries{Function("A")};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());
  const std::vector<v1::FunctionSummary> no_summaries;
  FixpointEngine engine(*graph, *scc, no_summaries);

  auto result =
      engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {.max_iterations = 1});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kNotFound);
}

} // namespace
} // namespace veritas::wpa
