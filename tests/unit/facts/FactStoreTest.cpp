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

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

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

core::StableId BatchId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFact,
                            std::as_bytes(std::span(text.data(), text.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to,
                      EpistemicState state = EpistemicState::kMay) {
  return SemanticRow{RelationId::kReachableCall,
                     {FunctionId(from), FunctionId(to), state}};
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
  batch.SetFacts({derived});
  batch.SetWitnesses(
      {Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0)});
  return batch;
}

AnalysisFactBatch PublicationRegressionBatch(const AnalysisRunManifest& run) {
  const auto root_fg = MakeFact(DirectCall("f", "g")).value();
  const auto root_gh = MakeFact(DirectCall("g", "h")).value();
  const auto root_gi = MakeFact(DirectCall("g", "i")).value();
  const auto reachable_fg = MakeFact(Reachable("f", "g")).value();
  const auto reachable_fh = MakeFact(Reachable("f", "h")).value();
  const auto reachable_fi = MakeFact(Reachable("f", "i")).value();

  AnalysisFactBatch batch;
  batch.run = run;
  batch.batch_id = BatchId("publication-regression");
  batch.rooted_input_fact_ids = {root_fg.fact_id, root_gh.fact_id,
                                 root_gi.fact_id};
  batch.rooted_input_facts = {
      RootedInputFact{.fact = root_fg,
                      .provenance_ref = "root-fg",
                      .producer_id = "producer-fg",
                      .source_anchor_id = "anchor-fg",
                      .summary_id = "summary-fg",
                      .description = "direct f to g"},
      RootedInputFact{.fact = root_gh,
                      .provenance_ref = "root-gh",
                      .producer_id = "producer-gh",
                      .source_anchor_id = "anchor-gh",
                      .summary_id = "summary-gh",
                      .description = "direct g to h"},
      RootedInputFact{.fact = root_gi,
                      .provenance_ref = "root-gi",
                      .producer_id = "producer-gi",
                      .source_anchor_id = "anchor-gi",
                      .summary_id = "summary-gi",
                      .description = "direct g to i"},
  };
  batch.SetFacts({reachable_fg, reachable_fh, reachable_fi});
  batch.SetWitnesses({
      Edge(Reachable("f", "h"), kTransitive, DirectCall("g", "h"), 1),
      Edge(Reachable("f", "i"), kTransitive, DirectCall("g", "i"), 1),
      Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0),
      Edge(Reachable("f", "h"), kTransitive, Reachable("f", "g"), 0),
      Edge(Reachable("f", "i"), kTransitive, DirectCall("f", "g"), 0),
  });
  return batch;
}

std::vector<std::vector<std::string>> SortedRows(
    std::vector<std::vector<std::string>> rows) {
  std::ranges::sort(rows);
  return rows;
}

std::vector<std::vector<std::string>> SortedEdgeRows(
    std::vector<std::vector<std::string>> rows) {
  std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs[0], lhs[4]) < std::tie(rhs[0], rhs[4]);
  });
  return rows;
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
  const auto fact = MakeFact(Reachable("f", "g")).value();

  auto batch = SuccessfulBatch(run);
  ASSERT_TRUE(store->Publish(batch).ok());

  // Re-delivery of the same (run_id, batch_id) is a successful no-op: it
  // changes no fact, binding, or provenance state.
  ASSERT_TRUE(store->Publish(batch).ok());
  auto after_redelivery = store->GetBindings(run.run_id, fact.fact_id);
  ASSERT_TRUE(after_redelivery.ok());
  ASSERT_EQ(after_redelivery->size(), 1u);

  // A different batch in the same run replaces the current binding and keeps
  // the prior one as history.
  auto batch2 = SuccessfulBatch(run);
  batch2.batch_id = BatchId("batch2");
  ASSERT_TRUE(store->Publish(batch2).ok());

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

TEST(FactStoreTest,
     PublishPreservesSharedRootsMultiEdgeProofsAndIdempotentRedelivery) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto run = TestRun("publication-regression");
  const auto batch = PublicationRegressionBatch(run);
  ASSERT_TRUE(store->Publish(batch).ok());

  const auto root_fg = MakeFact(DirectCall("f", "g")).value();
  const auto root_gh = MakeFact(DirectCall("g", "h")).value();
  const auto root_gi = MakeFact(DirectCall("g", "i")).value();
  const auto reachable_fg = MakeFact(Reachable("f", "g")).value();
  const auto reachable_fh = MakeFact(Reachable("f", "h")).value();
  const auto reachable_fi = MakeFact(Reachable("f", "i")).value();
  for (const AnalysisFact* expected :
       {&root_fg, &root_gh, &root_gi, &reachable_fg, &reachable_fh,
        &reachable_fi}) {
    auto fact = store->GetFact(expected->fact_id);
    ASSERT_TRUE(fact.ok()) << fact.status().message();
    EXPECT_EQ(*fact, *expected);
  }

  const std::string run_id = core::ToString(run.run_id);
  auto counts = store->metadata_store().Query(
      "SELECT (SELECT COUNT(*) FROM analysis_facts),"
      " (SELECT COUNT(*) FROM run_fact_bindings),"
      " (SELECT COUNT(*) FROM provenance_nodes),"
      " (SELECT COUNT(*) FROM provenance_edges),"
      " (SELECT COUNT(*) FROM fact_batch_receipts)",
      {});
  ASSERT_TRUE(counts.ok()) << counts.status().message();
  EXPECT_EQ(*counts, (std::vector<std::vector<std::string>>{
                         {{"6", "3", "3", "5", "1"}}}));

  auto binding_rows = store->metadata_store().Query(
      "SELECT fact_id, selected_witness_id FROM run_fact_bindings"
      " WHERE run_id = ? ORDER BY fact_id",
      {run_id});
  ASSERT_TRUE(binding_rows.ok()) << binding_rows.status().message();
  ASSERT_EQ(binding_rows->size(), 3u);
  std::map<std::string, std::string> selected_witnesses;
  for (const auto& row : *binding_rows) {
    ASSERT_EQ(row.size(), 2u);
    selected_witnesses.emplace(row[0], row[1]);
  }
  ASSERT_EQ(selected_witnesses.size(), 3u);
  EXPECT_EQ(selected_witnesses[core::ToString(reachable_fg.fact_id)],
            "2af032a893e0118902869038718fb982e76910343f9fe7bea22e0a19e8309d26");
  EXPECT_EQ(selected_witnesses[core::ToString(reachable_fh.fact_id)],
            "ef801f6f0c7aac5385bdcaf76ec3edc90260475871c537a91688faab7ea1ba29");
  EXPECT_EQ(selected_witnesses[core::ToString(reachable_fi.fact_id)],
            "a2b60efa2c9496b73eeea39c096c218cb522fdc34f01561ba7df32efacbb75bf");

  auto node_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, selected, producer_kind,"
      " producer_id, rule_id, source_anchor_id, summary_id, description"
      " FROM provenance_nodes WHERE run_id = ? ORDER BY output_fact_id",
      {run_id});
  ASSERT_TRUE(node_rows.ok()) << node_rows.status().message();
  EXPECT_EQ(
      *node_rows,
      SortedRows({
          {core::ToString(reachable_fg.fact_id),
           selected_witnesses[core::ToString(reachable_fg.fact_id)], "1", "0",
           "producer-fg", std::string(kDirect), "anchor-fg", "summary-fg",
           "direct f to g"},
          {core::ToString(reachable_fh.fact_id),
           selected_witnesses[core::ToString(reachable_fh.fact_id)], "1", "0",
           "producer-gh", std::string(kTransitive), "anchor-gh", "summary-gh",
           "direct g to h"},
          {core::ToString(reachable_fi.fact_id),
           selected_witnesses[core::ToString(reachable_fi.fact_id)], "1", "0",
           "producer-fg", std::string(kTransitive), "anchor-fg", "summary-fg",
           "direct f to g"},
      }));

  auto edge_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, input_kind, input_id, input_ordinal"
      " FROM provenance_edges WHERE run_id = ?"
      " ORDER BY output_fact_id, input_ordinal",
      {run_id});
  ASSERT_TRUE(edge_rows.ok()) << edge_rows.status().message();
  EXPECT_EQ(
      *edge_rows,
      SortedEdgeRows({
          {core::ToString(reachable_fg.fact_id),
           selected_witnesses[core::ToString(reachable_fg.fact_id)], "rooted",
           core::ToString(root_fg.fact_id), "0"},
          {core::ToString(reachable_fh.fact_id),
           selected_witnesses[core::ToString(reachable_fh.fact_id)], "derived",
           core::ToString(reachable_fg.fact_id), "0"},
          {core::ToString(reachable_fh.fact_id),
           selected_witnesses[core::ToString(reachable_fh.fact_id)], "rooted",
           core::ToString(root_gh.fact_id), "1"},
          {core::ToString(reachable_fi.fact_id),
           selected_witnesses[core::ToString(reachable_fi.fact_id)], "rooted",
           core::ToString(root_fg.fact_id), "0"},
          {core::ToString(reachable_fi.fact_id),
           selected_witnesses[core::ToString(reachable_fi.fact_id)], "rooted",
           core::ToString(root_gi.fact_id), "1"},
      }));

  const auto before_redelivery_counts = *counts;
  const auto before_redelivery_bindings = *binding_rows;
  const auto before_redelivery_nodes = *node_rows;
  const auto before_redelivery_edges = *edge_rows;
  ASSERT_TRUE(store->Publish(batch).ok());

  counts = store->metadata_store().Query(
      "SELECT (SELECT COUNT(*) FROM analysis_facts),"
      " (SELECT COUNT(*) FROM run_fact_bindings),"
      " (SELECT COUNT(*) FROM provenance_nodes),"
      " (SELECT COUNT(*) FROM provenance_edges),"
      " (SELECT COUNT(*) FROM fact_batch_receipts)",
      {});
  binding_rows = store->metadata_store().Query(
      "SELECT fact_id, selected_witness_id FROM run_fact_bindings"
      " WHERE run_id = ? ORDER BY fact_id",
      {run_id});
  node_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, selected, producer_kind,"
      " producer_id, rule_id, source_anchor_id, summary_id, description"
      " FROM provenance_nodes WHERE run_id = ? ORDER BY output_fact_id",
      {run_id});
  edge_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, input_kind, input_id, input_ordinal"
      " FROM provenance_edges WHERE run_id = ?"
      " ORDER BY output_fact_id, input_ordinal",
      {run_id});
  ASSERT_TRUE(counts.ok()) << counts.status().message();
  ASSERT_TRUE(binding_rows.ok()) << binding_rows.status().message();
  ASSERT_TRUE(node_rows.ok()) << node_rows.status().message();
  ASSERT_TRUE(edge_rows.ok()) << edge_rows.status().message();
  EXPECT_EQ(*counts, before_redelivery_counts);
  EXPECT_EQ(*binding_rows, before_redelivery_bindings);
  EXPECT_EQ(*node_rows, before_redelivery_nodes);
  EXPECT_EQ(*edge_rows, before_redelivery_edges);

  std::filesystem::remove_all(db);
}

// Two results of one relation sharing every cell but the last, so their
// encoded keys agree up to their final field, each derived by two edges. A
// grouping keyed on anything coarser than the whole encoded result key -- a
// prefix of it, the relation alone, or the two derivations of one result --
// merges or splits these proofs. Every published row the grouping decides is
// asserted: one node per result, carrying that result's own rule, its own
// rooted input's evidence, and its own edges at their own ordinals.
TEST(FactStoreTest, WitnessGroupingIsKeyedByRankNotByEncodedKey) {
  const auto db = TempDbPath();
  auto store = FactStore::Open(db);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto run = TestRun("witness-grouping");
  const auto direct_result = MakeFact(Reachable("f", "g")).value();
  const auto inferred_result =
      MakeFact(Reachable("f", "g", EpistemicState::kInferred)).value();

  // The premise, asserted rather than assumed: the two keys differ only inside
  // their last field, so they share all but the final few bytes and a grouping
  // that reads less than the whole key merges them.
  const std::string direct_key = EncodeSemanticKey(direct_result.row);
  const std::string inferred_key = EncodeSemanticKey(inferred_result.row);
  ASSERT_NE(direct_key, inferred_key);
  std::size_t shared = 0;
  while (shared < direct_key.size() && shared < inferred_key.size() &&
         direct_key[shared] == inferred_key[shared]) {
    ++shared;
  }
  EXPECT_GT(shared, direct_key.size() - 8u);

  const auto root_direct = MakeFact(DirectCall("f", "g")).value();
  const auto root_inferred = MakeFact(DirectCall("g", "h")).value();
  const auto via_direct = MakeFact(Reachable("f", "h")).value();
  const auto via_inferred = MakeFact(Reachable("g", "i")).value();

  AnalysisFactBatch batch;
  batch.run = run;
  batch.batch_id = BatchId("witness-grouping");
  batch.rooted_input_fact_ids = {root_direct.fact_id, root_inferred.fact_id};
  batch.rooted_input_facts = {
      RootedInputFact{.fact = root_direct,
                      .provenance_ref = "root-direct",
                      .producer_id = "producer-direct",
                      .source_anchor_id = "anchor-direct",
                      .summary_id = "summary-direct",
                      .description = "direct root"},
      RootedInputFact{.fact = root_inferred,
                      .provenance_ref = "root-inferred",
                      .producer_id = "producer-inferred",
                      .source_anchor_id = "anchor-inferred",
                      .summary_id = "summary-inferred",
                      .description = "inferred root"},
  };
  batch.SetFacts({direct_result, inferred_result, via_direct, via_inferred});
  // Interleaved, and each group's edges appended out of ordinal order: the
  // group loop is the only thing that separates the two results' edges, and
  // each group's front edge -- the one that names the rule and carries the
  // rooted evidence -- is its ordinal-0 edge, not the first one appended.
  batch.SetWitnesses({
      Edge(Reachable("f", "g", EpistemicState::kInferred), kTransitive,
           Reachable("g", "i"), 1),
      Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0),
      Edge(Reachable("f", "g", EpistemicState::kInferred), kTransitive,
           DirectCall("g", "h"), 0),
      Edge(Reachable("f", "g"), kDirect, Reachable("f", "h"), 1),
  });
  ASSERT_TRUE(store->Publish(batch).ok());

  const std::string run_id = core::ToString(run.run_id);
  auto counts = store->metadata_store().Query(
      "SELECT (SELECT COUNT(*) FROM analysis_facts),"
      " (SELECT COUNT(*) FROM run_fact_bindings),"
      " (SELECT COUNT(*) FROM provenance_nodes),"
      " (SELECT COUNT(*) FROM provenance_edges)",
      {});
  ASSERT_TRUE(counts.ok()) << counts.status().message();
  // Four published facts plus two rooted inputs; one binding per published
  // fact; one node and two edges per result. A grouping that merged the two
  // results publishes one node instead of two.
  EXPECT_EQ(*counts,
            (std::vector<std::vector<std::string>>{{"6", "4", "2", "4"}}));

  auto binding_rows = store->metadata_store().Query(
      "SELECT fact_id, selected_witness_id FROM run_fact_bindings"
      " WHERE run_id = ? ORDER BY fact_id",
      {run_id});
  ASSERT_TRUE(binding_rows.ok()) << binding_rows.status().message();
  ASSERT_EQ(binding_rows->size(), 4u);
  std::map<std::string, std::string> selected_witnesses;
  for (const auto& row : *binding_rows) {
    ASSERT_EQ(row.size(), 2u);
    selected_witnesses.emplace(row[0], row[1]);
  }
  ASSERT_EQ(selected_witnesses.size(), 4u);
  // Pinned derivation identities: each covers one result's own ordered edges.
  // A merged group derives one identity for both results, and a group ordered
  // by arrival rather than by ordinal derives a different one.
  EXPECT_EQ(selected_witnesses[core::ToString(direct_result.fact_id)],
            "57f195d4c9904d4d9f5bd1926d3edc1fe1a75a90e1faee8ab7deb928b142495e");
  EXPECT_EQ(selected_witnesses[core::ToString(inferred_result.fact_id)],
            "15ea53e4b8afa903bb9886295642db92c0fd5b76c57e016cbbfd5b2f92ac3428");
  EXPECT_NE(selected_witnesses[core::ToString(direct_result.fact_id)],
            selected_witnesses[core::ToString(inferred_result.fact_id)]);
  // A published fact with no derivation of its own falls back to its semantic
  // identity rather than borrowing another result's witness.
  EXPECT_EQ(selected_witnesses[core::ToString(via_direct.fact_id)],
            core::ToString(via_direct.fact_id));

  auto node_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, selected, producer_kind,"
      " producer_id, rule_id, source_anchor_id, summary_id, description"
      " FROM provenance_nodes WHERE run_id = ? ORDER BY output_fact_id",
      {run_id});
  ASSERT_TRUE(node_rows.ok()) << node_rows.status().message();
  EXPECT_EQ(
      *node_rows,
      SortedRows({
          {core::ToString(direct_result.fact_id),
           selected_witnesses[core::ToString(direct_result.fact_id)], "1", "0",
           "producer-direct", std::string(kDirect), "anchor-direct",
           "summary-direct", "direct root"},
          {core::ToString(inferred_result.fact_id),
           selected_witnesses[core::ToString(inferred_result.fact_id)], "1",
           "0", "producer-inferred", std::string(kTransitive),
           "anchor-inferred", "summary-inferred", "inferred root"},
      }));

  auto edge_rows = store->metadata_store().Query(
      "SELECT output_fact_id, witness_id, input_kind, input_id, input_ordinal"
      " FROM provenance_edges WHERE run_id = ?"
      " ORDER BY output_fact_id, input_ordinal",
      {run_id});
  ASSERT_TRUE(edge_rows.ok()) << edge_rows.status().message();
  EXPECT_EQ(
      *edge_rows,
      SortedEdgeRows({
          {core::ToString(direct_result.fact_id),
           selected_witnesses[core::ToString(direct_result.fact_id)], "rooted",
           core::ToString(root_direct.fact_id), "0"},
          {core::ToString(direct_result.fact_id),
           selected_witnesses[core::ToString(direct_result.fact_id)], "derived",
           core::ToString(via_direct.fact_id), "1"},
          {core::ToString(inferred_result.fact_id),
           selected_witnesses[core::ToString(inferred_result.fact_id)],
           "rooted", core::ToString(root_inferred.fact_id), "0"},
          {core::ToString(inferred_result.fact_id),
           selected_witnesses[core::ToString(inferred_result.fact_id)],
           "derived", core::ToString(via_inferred.fact_id), "1"},
      }));

  // The groups are visited in their encoded keys' own ascending order, which
  // is the order the rows are inserted in. Both results have the same relation
  // and shape, so this is a real ordering claim and not a restatement of the
  // fact ids.
  auto write_order = store->metadata_store().Query(
      "SELECT output_fact_id FROM provenance_nodes WHERE run_id = ?"
      " ORDER BY rowid",
      {run_id});
  ASSERT_TRUE(write_order.ok()) << write_order.status().message();
  ASSERT_EQ(write_order->size(), 2u);
  const bool direct_first = direct_key < inferred_key;
  EXPECT_EQ((*write_order)[0][0],
            core::ToString(direct_first ? direct_result.fact_id
                                        : inferred_result.fact_id));
  EXPECT_EQ((*write_order)[1][0],
            core::ToString(direct_first ? inferred_result.fact_id
                                        : direct_result.fact_id));

  std::filesystem::remove_all(db);
}
