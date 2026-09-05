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

#include "veritas/facts/FactStore.h"

#include <cstdint>
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

constexpr std::string_view kDirect = "wpa.reachability.direct.v2";

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId BatchId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFact,
                            std::as_bytes(std::span(text.data(), text.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kReachableCall,
                     {FunctionId(from), FunctionId(to),
                      EpistemicState::kMay}};
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

AnalysisRunManifest TestRun(std::string_view seed = "run") {
  AnalysisRunDescriptor descriptor;
  const std::string rev = std::string(seed) + "-revision";
  const std::string bv = std::string(seed) + "-variant";
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span(rev.data(), rev.size())));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant,
      std::as_bytes(std::span(bv.data(), bv.size())));
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
      (std::filesystem::temp_directory_path() / "veritas-factstore-XXXXXX")
          .string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

AnalysisFactBatch SuccessfulBatch(const AnalysisRunManifest& run) {
  const auto root = MakeFact(DirectCall("f", "g")).value();
  const auto derived = MakeFact(Reachable("f", "g")).value();
  AnalysisFactBatch batch;
  batch.run = run;
  batch.batch_id = BatchId("batch");
  batch.rooted_input_fact_ids = {root.fact_id};
  batch.facts = {derived};
  batch.witnesses = {Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0)};
  return batch;
}

}  // namespace

TEST(FactStoreTest, PublishStoresFactsAndBindings) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto run = TestRun();
  const auto batch = SuccessfulBatch(run);
  ASSERT_TRUE(store->Publish(batch).ok());

  const auto derived = MakeFact(Reachable("f", "g")).value();
  const auto root = MakeFact(DirectCall("f", "g")).value();

  // The derived fact is stored and retrievable.
  auto got = store->GetFact(derived.fact_id);
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ(got->fact_id, derived.fact_id);
  EXPECT_EQ(got->row, derived.row);

  // The rooted input is stored (for display) even though it has no binding.
  auto got_root = store->GetFact(root.fact_id);
  ASSERT_TRUE(got_root.ok()) << got_root.status().message();

  // Only the derived fact has a current binding in this run.
  auto binding = store->GetBinding(run.run_id, derived.fact_id);
  ASSERT_TRUE(binding.ok()) << binding.status().message();
  EXPECT_TRUE(binding->is_current);

  auto current = store->GetCurrentFacts(run.run_id);
  ASSERT_TRUE(current.ok());
  ASSERT_EQ(current->size(), 1u);
  EXPECT_EQ((*current)[0].fact_id, derived.fact_id);

  std::filesystem::remove_all(db);
}

TEST(FactStoreTest, CurrentReplacementPreservesHistory) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto run = TestRun();
  const auto batch = SuccessfulBatch(run);
  const auto fact = MakeFact(Reachable("f", "g")).value();

  ASSERT_TRUE(store->Publish(batch).ok());
  ASSERT_TRUE(store->Publish(batch).ok());  // re-derivation replaces the current

  auto bindings = store->GetBindings(run.run_id, fact.fact_id);
  ASSERT_TRUE(bindings.ok());
  ASSERT_EQ(bindings->size(), 2u);
  // Newest first: the current binding, then the historical one.
  EXPECT_TRUE((*bindings)[0].is_current);
  EXPECT_FALSE((*bindings)[1].is_current);

  // The current binding is still addressable by the single-binding accessor.
  auto current = store->GetBinding(run.run_id, fact.fact_id);
  ASSERT_TRUE(current.ok());
  EXPECT_TRUE(current->is_current);

  std::filesystem::remove_all(db);
}

TEST(FactStoreTest, SameFactAcrossRunsSharesFactId) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto run1 = TestRun("run1");
  const auto run2 = TestRun("run2");
  const auto batch1 = SuccessfulBatch(run1);
  const auto batch2 = SuccessfulBatch(run2);
  const auto fact = MakeFact(Reachable("f", "g")).value();

  ASSERT_TRUE(store->Publish(batch1).ok());
  ASSERT_TRUE(store->Publish(batch2).ok());

  // Both runs bind the same canonical fact id.
  auto b1 = store->GetBinding(run1.run_id, fact.fact_id);
  auto b2 = store->GetBinding(run2.run_id, fact.fact_id);
  ASSERT_TRUE(b1.ok());
  ASSERT_TRUE(b2.ok());
  EXPECT_EQ(b1->fact_id, fact.fact_id);
  EXPECT_EQ(b2->fact_id, fact.fact_id);

  std::filesystem::remove_all(db);
}
