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

#include "veritas/facts/ProvenanceStore.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

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
                     {FunctionId(from), FunctionId(to), EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to), DispatchKind::kDirect,
                      EpistemicState::kMay}};
}

WitnessEdge Edge(const SemanticRow& result, std::string_view rule,
                 const SemanticRow& input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

AnalysisRunManifest TestRun() {
  AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("chain-rev", 9)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("chain-bv", 8)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = "test-toolchain";
  return std::move(MakeAnalysisRun(descriptor)).value();
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-provenance-XXXXXX")
          .string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// A three-level chain: direct(f,g) -> reachable(f,g) -> reachable(f,h) ->
// reachable(f,i). Explaining the tip (f,i) walks back to the rooted direct
// call.
struct Chain {
  AnalysisRunManifest run;
  AnalysisFactBatch batch;
  core::StableId tip;
};

Chain MakeChain() {
  Chain chain;
  chain.run = TestRun();
  const auto root = MakeFact(DirectCall("f", "g")).value();
  const auto a = MakeFact(Reachable("f", "g")).value();
  const auto b = MakeFact(Reachable("f", "h")).value();
  const auto c = MakeFact(Reachable("f", "i")).value();
  chain.tip = c.fact_id;
  chain.batch.run = chain.run;
  chain.batch.batch_id = core::MakeStableId(
      core::IdKind::kFact, std::as_bytes(std::span("chain-batch", 11)));
  chain.batch.rooted_input_fact_ids = {root.fact_id};
  chain.batch.facts = {a, b, c};
  chain.batch.witnesses = {
      Edge(Reachable("f", "g"), "direct", DirectCall("f", "g"), 0),
      Edge(Reachable("f", "h"), "transitive", Reachable("f", "g"), 0),
      Edge(Reachable("f", "i"), "transitive", Reachable("f", "h"), 0),
  };
  return chain;
}

}  // namespace

TEST(ProvenanceStoreTest, ExplainWalksTheFullDag) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();
  auto chain = MakeChain();
  ASSERT_TRUE(store->Publish(chain.batch).ok());

  ProvenanceStore provenance(store->metadata_store());
  ExplainBudget budget;
  budget.max_depth = 10;
  budget.max_nodes = 100;
  auto graph = provenance.Explain(chain.run.run_id, chain.tip, budget);
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  EXPECT_EQ(graph->fact_id(), core::ToString(chain.tip));
  EXPECT_TRUE(graph->binding().is_current());
  EXPECT_EQ(graph->nodes_size(), 3);
  EXPECT_EQ(graph->edges_size(), 3);
  EXPECT_FALSE(graph->truncated());

  std::filesystem::remove_all(db);
}

TEST(ProvenanceStoreTest, ExplainTruncatesAtMaxDepth) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();
  auto chain = MakeChain();
  ASSERT_TRUE(store->Publish(chain.batch).ok());

  ProvenanceStore provenance(store->metadata_store());
  ExplainBudget budget;
  budget.max_depth = 1;
  budget.max_nodes = 100;
  auto graph = provenance.Explain(chain.run.run_id, chain.tip, budget);
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  EXPECT_TRUE(graph->truncated());
  EXPECT_EQ(graph->truncation_reason(), "max_depth");
  // The tip and one hop are expanded before the budget is exhausted.
  EXPECT_EQ(graph->nodes_size(), 2);

  std::filesystem::remove_all(db);
}

TEST(ProvenanceStoreTest, ExplainTruncatesAtMaxNodes) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();
  auto chain = MakeChain();
  ASSERT_TRUE(store->Publish(chain.batch).ok());

  ProvenanceStore provenance(store->metadata_store());
  ExplainBudget budget;
  budget.max_depth = 10;
  budget.max_nodes = 1;
  auto graph = provenance.Explain(chain.run.run_id, chain.tip, budget);
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  EXPECT_TRUE(graph->truncated());
  EXPECT_EQ(graph->truncation_reason(), "max_nodes");
  EXPECT_EQ(graph->nodes_size(), 1);

  std::filesystem::remove_all(db);
}
