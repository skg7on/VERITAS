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

// EvidenceHandoffIntegrationTest.cpp — HND-006, the cross-translation-unit
// summary fixture built in two independent checkout roots.
//
// The summary fixture's source value (packet.length) is read in entry.cpp and
// reaches the memcpy sink in copy.cpp across a function-call summary boundary.
// HND-006 requires the resulting semantic input and IDs to be deterministic
// across checkout roots and insertion orders, with summary references present
// and no source text copied into the handoff.

#include <algorithm>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "evidence/RealEvidencePipeline.h"
#include "veritas/core/Ids.h"
#include "veritas/evidence/EirText.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCaseBuilder.h"
#include "veritas/evidence/EvidenceProto.h"
#include "veritas/evidence/FactStoreEvidenceBackend.h"
#include "veritas/evidence/OverflowClaimSeed.h"
#include "veritas/facts/ProvenanceStore.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::testing {
namespace {

std::vector<core::StableId> SortedFactIds(
    const std::vector<facts::AnalysisFact>& facts) {
  std::vector<core::StableId> ids;
  ids.reserve(facts.size());
  for (const auto& fact : facts) {
    ids.push_back(fact.fact_id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool HasRelation(const std::vector<facts::AnalysisFact>& facts,
                 facts::RelationId relation) {
  for (const auto& fact : facts) {
    if (fact.row.relation == relation) {
      return true;
    }
  }
  return false;
}

// Real durable provenance, with only the tip matching the range query. The
// completion witness describes the scan, never this three-level derivation.
TEST(EvidenceHandoffIntegrationTest,
     DepthOneAndTwoYieldDifferentRootedFrontiers) {
  EvidenceScenarioBuilder scenario;
  auto request = scenario.BuildRequest(evidence::EvidenceLevel::kL1);
  auto id = [&](core::IdKind kind, std::string_view name) {
    return scenario.Id(kind, name);
  };
  request.input.claim_seed.source_ref = id(core::IdKind::kMemoryRef, "tip");
  auto row = [&](std::string_view name) {
    return facts::SemanticRow{
        facts::RelationId::kDirectRead,
        {id(core::IdKind::kFunctionVariant, "function"),
         name == "tip" ? request.input.claim_seed.source_ref
                       : id(core::IdKind::kMemoryRef, name),
         analysis::semantic::ByteRangeKind::kKnown, std::int64_t{0},
         std::uint64_t{4}, analysis::semantic::EpistemicState::kMust}};
  };
  facts::AnalysisFactBatch batch;
  batch.run.run_id = request.input.flow_slice.metadata.analysis_run_id;
  batch.run.engine = facts::EngineIdentity::kSouffle;
  batch.batch_id = id(core::IdKind::kFact, "depth-batch");
  const auto root = facts::MakeFact(row("root")).value();
  const auto a = facts::MakeFact(row("a")).value();
  const auto b = facts::MakeFact(row("b")).value();
  const auto tip = facts::MakeFact(row("tip")).value();
  batch.rooted_input_fact_ids = {root.fact_id};
  batch.facts = {a, b, tip};
  batch.witnesses = {
      {facts::SemanticKey{a.row}, "root", {}, facts::SemanticKey{root.row}, 0},
      {facts::SemanticKey{b.row}, "step", {}, facts::SemanticKey{a.row}, 0},
      {facts::SemanticKey{tip.row}, "step", {}, facts::SemanticKey{b.row}, 0}};
  std::string path =
      (std::filesystem::temp_directory_path() / "veritas-evidence-depth-XXXXXX")
          .string();
  ASSERT_NE(::mkdtemp(path.data()), nullptr);
  auto store = facts::FactStore::Open(path);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store->Publish(batch).ok());
  evidence::SnapshotDescriptor descriptor;
  descriptor.analysis_run_id = batch.run.run_id;
  evidence::FactStoreEvidenceBackend backend(*store, descriptor);
  cpg::ThinCpg cpg;
  evidence::EvidenceQueryService service(cpg, backend, batch.run.run_id);
  auto one = service.BuildEvidenceInput(request.input.claim_seed,
                                        {16, 256, 16, 256, 1});
  auto two = service.BuildEvidenceInput(request.input.claim_seed,
                                        {16, 256, 16, 256, 2});
  ASSERT_TRUE(one.ok()) << one.status().message();
  ASSERT_TRUE(two.ok()) << two.status().message();
  EXPECT_TRUE(one->provenance.truncated());
  EXPECT_FALSE(two->provenance.truncated());
  auto has = [](const auto &input, const auto &fact) {
    return std::ranges::any_of(input.provenance.nodes(), [&](const auto &node) {
      return node.output_fact_id() == core::ToString(fact.fact_id);
    });
  };
  EXPECT_TRUE(has(*one, tip));
  EXPECT_TRUE(has(*one, b));
  EXPECT_FALSE(has(*one, a));
  EXPECT_TRUE(has(*two, a));
  EXPECT_TRUE(
      std::ranges::any_of(one->provenance.edges(), [&](const auto &edge) {
        return edge.input_id() == core::ToString(a.fact_id);
      }));
  EXPECT_TRUE(
      std::ranges::any_of(two->provenance.edges(), [&](const auto &edge) {
        return edge.input_kind() == "rooted" &&
               edge.input_id() == core::ToString(root.fact_id);
      }));
  ASSERT_EQ(one->fact_explanations.size(), 1);
  EXPECT_EQ(one->fact_explanations.front().frontier_refs,
            std::vector<core::StableId>{a.fact_id});
  EXPECT_EQ(one->fact_explanations.front().truncation_reason, "max_depth");
  auto exact =
      service.BuildEvidenceInput(request.input.claim_seed, {16, 6, 16, 256, 2});
  ASSERT_TRUE(exact.ok()) << exact.status().message();
  EXPECT_FALSE(exact->provenance.truncated());
  auto cut =
      service.BuildEvidenceInput(request.input.claim_seed, {16, 5, 16, 256, 2});
  ASSERT_TRUE(cut.ok()) << cut.status().message();
  EXPECT_TRUE(cut->provenance.truncated());
  EXPECT_EQ(cut->provenance.nodes_size() + cut->provenance.edges_size(), 6 + 5);
  EXPECT_EQ(cut->fact_explanations.front().truncation_reason,
            "max_provenance_members");
  request.input = *one;
  for (auto level : {evidence::EvidenceLevel::kL0, evidence::EvidenceLevel::kL1,
                     evidence::EvidenceLevel::kL2}) {
    request.level = level;
    auto built = evidence::EvidenceCaseBuilder().Build(request);
    ASSERT_TRUE(built.ok()) << built.status().message();
    EXPECT_TRUE(
        std::ranges::any_of(built->omissions, [&](const auto &omission) {
          return omission.kind == "provenance_frontier" &&
                 omission.reason.find(core::ToString(a.fact_id)) !=
                     std::string::npos;
        }));
  }
  ASSERT_TRUE(store->metadata_store()
                  .Execute("UPDATE run_fact_bindings SET selected_witness_id = "
                           "'stale' WHERE fact_id = ?",
                           {core::ToString(b.fact_id)})
                  .ok());
  auto stale_premise = service.BuildEvidenceInput(request.input.claim_seed,
                                                  {16, 256, 16, 256, 16});
  EXPECT_FALSE(stale_premise.ok())
      << "an intermediate selected binding must also be pinned";
  ASSERT_TRUE(
      store->metadata_store()
          .Execute("UPDATE run_fact_bindings SET selected_witness_id = (SELECT "
                   "witness_id FROM provenance_nodes WHERE output_fact_id = ? "
                   "AND selected = 1) WHERE fact_id = ?",
                   {core::ToString(b.fact_id), core::ToString(b.fact_id)})
          .ok());
  ASSERT_TRUE(
      store->metadata_store()
          .Execute("UPDATE provenance_edges SET input_kind = 'derived', "
                   "input_id = ? WHERE output_fact_id = ?",
                   {core::ToString(tip.fact_id), core::ToString(a.fact_id)})
          .ok());
  auto cycle = service.BuildEvidenceInput(request.input.claim_seed,
                                          {16, 256, 16, 256, 16});
  ASSERT_FALSE(cycle.ok());
  EXPECT_NE(cycle.status().message().find("cycle"), std::string::npos);
  std::filesystem::remove_all(path);
}

TEST(EvidenceHandoffIntegrationTest,
     CompletionWitnessDoesNotReplaceFactWitness) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok());
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, {16, 256, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  ASSERT_FALSE(input->flow_slice.supporting_facts.empty());
  EXPECT_LE(input->provenance.nodes_size() + input->provenance.edges_size(),
            256 + 6);
  std::set<std::string> outputs;
  for (const auto &node : input->provenance.nodes())
    EXPECT_TRUE(outputs.insert(node.output_fact_id()).second);
  for (const auto &fact : input->flow_slice.supporting_facts) {
    EXPECT_TRUE(std::ranges::any_of(input->provenance.nodes(),
                                    [&](const auto &node) {
                                      return node.selected() &&
                                             node.output_fact_id() ==
                                                 core::ToString(fact.fact_id);
                                    }))
        << core::ToString(fact.fact_id);
  }
  auto request =
      EvidenceScenarioBuilder().BuildRequest(evidence::EvidenceLevel::kL1);
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  for (const auto &edge : input->provenance.edges()) {
    if (edge.input_kind() != "rooted")
      continue;
    EXPECT_TRUE(std::ranges::any_of(
        built->dependencies,
        [&](const auto &dependency) {
          return core::ToString(dependency.stable_id) == edge.input_id();
        }))
        << "lost rooted EIR premise " << edge.input_id();
    EXPECT_TRUE(
        std::ranges::any_of(built->omissions, [&](const auto &omission) {
          return omission.kind == "provenance_premise" &&
                 omission.reason.find(edge.input_id()) != std::string::npos;
        }));
  }
}

TEST(EvidenceHandoffIntegrationTest,
     AggregateCutRetainsUnexpandedSelectedBindings) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok());
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok());
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, {16, 5, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  ASSERT_FALSE(input->flow_slice.supporting_facts.empty());
  ASSERT_TRUE(input->provenance.truncated());
  for (auto level : {evidence::EvidenceLevel::kL0, evidence::EvidenceLevel::kL1,
                     evidence::EvidenceLevel::kL2}) {
    auto request = EvidenceScenarioBuilder().BuildRequest(level);
    request.input = *input;
    auto built = evidence::EvidenceCaseBuilder().Build(request);
    ASSERT_TRUE(built.ok()) << built.status().message();
    EXPECT_TRUE(std::ranges::all_of(
        built->facts,
        [](const auto &fact) { return !fact.provenance_id.empty(); }))
        << "selected native facts retain their pinned occurrence even before "
           "their witness is expanded";
    for (const auto &fact : built->facts) {
      if (!fact.stable_id)
        continue;
      const bool expanded =
          std::ranges::any_of(input->provenance.nodes(), [&](const auto &node) {
            return node.selected() &&
                   node.output_fact_id() == core::ToString(*fact.stable_id);
          });
      if (expanded)
        continue;
      auto record = std::ranges::find(built->provenance, fact.provenance_id,
                                      &evidence::Provenance::id);
      ASSERT_NE(record, built->provenance.end());
      EXPECT_TRUE(record->rule.empty());
      EXPECT_TRUE(record->source_anchor_id.empty());
      EXPECT_EQ(record->producer, "analysis.souffle");
    }
    EXPECT_TRUE(std::ranges::any_of(built->omissions, [](const auto &omission) {
      return omission.kind == "provenance_frontier";
    }));
  }
}

TEST(EvidenceHandoffIntegrationTest,
     AggregateCutMayFactsStillBuildIncompleteEir) {
  EvidenceScenarioBuilder scenario;
  auto request = scenario.BuildRequest(evidence::EvidenceLevel::kL1);
  facts::AnalysisFactBatch batch;
  batch.run.run_id = request.input.flow_slice.metadata.analysis_run_id;
  batch.run.engine = facts::EngineIdentity::kSouffle;
  batch.batch_id = scenario.Id(core::IdKind::kFact, "may-cut-batch");
  const auto root =
      facts::MakeFact(facts::SemanticRow{
                          facts::RelationId::kDirectCall,
                          {scenario.Id(core::IdKind::kCallSite, "call"),
                           scenario.Id(core::IdKind::kFunctionVariant, "from"),
                           scenario.Id(core::IdKind::kFunctionVariant, "to"),
                           analysis::semantic::DispatchKind::kDirect,
                           analysis::semantic::EpistemicState::kMay}})
          .value();
  batch.rooted_input_fact_ids = {root.fact_id};
  for (int i = 0; i < 8; ++i) {
    const auto alias =
        facts::MakeFact(
            facts::SemanticRow{facts::RelationId::kAlias,
                               {request.input.claim_seed.subject_ref,
                                scenario.Id(core::IdKind::kMemoryRef,
                                            "alias-" + std::to_string(i)),
                                analysis::semantic::AliasKind::kMayAlias,
                                analysis::semantic::EpistemicState::kMay}})
            .value();
    batch.facts.push_back(alias);
    batch.witnesses.push_back({facts::SemanticKey{alias.row},
                               "alias",
                               {},
                               facts::SemanticKey{root.row},
                               0});
  }
  std::string path = (std::filesystem::temp_directory_path() /
                      "veritas-evidence-may-cut-XXXXXX")
                         .string();
  ASSERT_NE(::mkdtemp(path.data()), nullptr);
  auto store = facts::FactStore::Open(path);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store->Publish(batch).ok());
  evidence::SnapshotDescriptor descriptor;
  descriptor.analysis_run_id = batch.run.run_id;
  evidence::FactStoreEvidenceBackend backend(*store, descriptor);
  cpg::ThinCpg cpg;
  evidence::EvidenceQueryService service(cpg, backend, batch.run.run_id);
  auto input = service.BuildEvidenceInput(request.input.claim_seed,
                                          {16, 1, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  ASSERT_EQ(input->aliases.facts.size(), 8);
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  EXPECT_EQ(built->facts.size(), 8);
  for (const auto &fact : built->facts) {
    EXPECT_EQ(fact.epistemic, evidence::EpistemicState::kMay);
    EXPECT_TRUE(fact.derived);
    EXPECT_FALSE(fact.provenance_id.empty());
  }
  std::filesystem::remove_all(path);
}

TEST(EvidenceHandoffIntegrationTest,
     ModernExplanationCarrierCannotBeSilentlyRemoved) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok());
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok());
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, {16, 256, 16, 256, 16});
  ASSERT_TRUE(input.ok());
  ASSERT_FALSE(input->fact_explanations.empty());
  auto request =
      EvidenceScenarioBuilder().BuildRequest(evidence::EvidenceLevel::kL1);
  request.input = *input;
  request.input.fact_explanations.clear();
  request.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(request.input);
  EXPECT_FALSE(evidence::EvidenceCaseBuilder().Build(request).ok())
      << "a declared modern provenance capability requires every selected "
         "explanation";
  request.input = *input;
  request.input.provenance_version = "evidence.provenance.future";
  request.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(request.input);
  EXPECT_FALSE(evidence::EvidenceCaseBuilder().Build(request).ok());
  request.input = *input;
  request.input.provenance_version.clear();
  request.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(request.input);
  EXPECT_FALSE(evidence::EvidenceCaseBuilder().Build(request).ok());
  // An actual pre-RDX04 v2 envelope is readable, with explicit capability loss.
  request.input.fact_explanations.clear();
  request.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(request.input);
  for (auto level : {evidence::EvidenceLevel::kL0, evidence::EvidenceLevel::kL1,
                     evidence::EvidenceLevel::kL2}) {
    request.level = level;
    auto legacy = evidence::EvidenceCaseBuilder().Build(request);
    ASSERT_TRUE(legacy.ok()) << legacy.status().message();
    EXPECT_TRUE(
        std::ranges::any_of(legacy->omissions, [&](const auto &omission) {
          return omission.kind == "legacy_provenance" &&
                 omission.subject == legacy->primary_claim.id;
        }));
  }
}

TEST(EvidenceHandoffIntegrationTest, L2CarriesCurrentSummaryComponents) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok());
  auto summaries =
      summarydb::SummaryRepository::Open(snapshot->output_root.string());
  ASSERT_TRUE(summaries.ok()) << summaries.status().message();
  evidence::FactStoreEvidenceBackend backend(
      snapshot->fact_store, snapshot->descriptor, summaries->get());
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, {16, 1024, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto request =
      EvidenceScenarioBuilder().BuildRequest(evidence::EvidenceLevel::kL2);
  request.context.repository_id = snapshot->descriptor.repository;
  request.context.revision_id = snapshot->descriptor.revision;
  request.context.build_variant_id = snapshot->descriptor.build_variant;
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  ASSERT_FALSE(built->summaries.empty())
      << "l2 must reference the current immutable summaries";
  ASSERT_EQ(input->summaries.size(), input->summary_selections.size());
  ASSERT_EQ(built->summaries.size(), input->summary_selections.size());
  for (const auto &selected : input->summary_selections) {
    auto current_summary = (*summaries)
                               ->GetCurrentSummaryArtifact(core::ToString(
                                   selected.function_variant_id));
    ASSERT_TRUE(current_summary.ok());
    EXPECT_EQ(summary::ComputeFunctionSummaryId(*current_summary).value(),
              selected.summary_id);
    for (const auto &component : selected.components) {
      auto stored = snapshot->fact_store.metadata_store().Query(
          "SELECT semantic_hash, evidence_hash FROM summary_components WHERE "
          "summary_id = ? AND component_kind = ?",
          {core::ToString(selected.summary_id),
           std::to_string(static_cast<int>(component.kind))});
      ASSERT_TRUE(stored.ok());
      ASSERT_EQ(stored->size(), 1);
      EXPECT_EQ((*stored)[0][0], core::DigestToHex(component.semantic_hash));
      EXPECT_EQ((*stored)[0][1], core::DigestToHex(component.evidence_hash));
    }
  }
  for (const auto &ref : built->summaries) {
    EXPECT_EQ(ref.summary_id.kind, core::IdKind::kFunctionSummary);
    EXPECT_FALSE(ref.components.empty());
    EXPECT_NE(std::ranges::find(ref.components, "value_flow"),
              ref.components.end());
    EXPECT_NE(std::ranges::find(ref.components, "unknowns"),
              ref.components.end());
  }
  auto text =
      evidence::WriteEirText(*built, evidence::EirTextStyle::kCanonical);
  ASSERT_TRUE(text.ok()) << text.status().message();
  auto parsed = evidence::ParseEirText(*text, nullptr);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->evidence_id, built->evidence_id);
  auto wire = evidence::EncodeEvidenceProto(*built);
  ASSERT_TRUE(wire.ok());
  auto decoded = evidence::DecodeEvidenceProto(*wire);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  EXPECT_EQ(decoded->evidence_id, built->evidence_id);

  std::reverse(request.input.summaries.begin(), request.input.summaries.end());
  std::reverse(request.input.summary_selections.begin(),
               request.input.summary_selections.end());
  for (auto &selected : request.input.summary_selections)
    std::reverse(selected.components.begin(), selected.components.end());
  EXPECT_EQ(evidence::ComputeEvidenceHandoffDigest(request.input),
            input->handoff_digest);
  auto reordered = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(reordered.ok()) << reordered.status().message();
  EXPECT_EQ(reordered->evidence_id, built->evidence_id);
  for (auto level :
       {evidence::EvidenceLevel::kL0, evidence::EvidenceLevel::kL1}) {
    auto compact_request = request;
    compact_request.level = level;
    auto compact = evidence::EvidenceCaseBuilder().Build(compact_request);
    ASSERT_TRUE(compact.ok()) << compact.status().message();
    EXPECT_TRUE(compact->summaries.empty());
    EXPECT_TRUE(
        std::ranges::any_of(compact->omissions, [](const auto &omission) {
          return omission.kind == "summary_expansion" && omission.expandable &&
                 omission.reason.find("summary:sha256:") != std::string::npos;
        }));
  }

  // A consistent semantic summary delta is part of EIR identity. This is a
  // handoff identity oracle; current native bindings were checked above.
  auto changed = request;
  auto &artifact = changed.input.summaries.front();
  const auto old_id = summary::ComputeFunctionSummaryId(artifact).value();
  std::visit(
      [](auto &value) { value.add_unknowns()->set_kind("semantic_delta"); },
      artifact);
  const auto components = summary::ComputeComponentDigests(artifact);
  std::visit(
      [&](auto &value) {
        value.clear_component_digests();
        for (const auto &component : components) {
          auto *digest = value.add_component_digests();
          digest->set_kind(component.kind);
          digest->set_semantic_hash(component.semantic_hash.data(),
                                    component.semantic_hash.size());
          digest->set_evidence_hash(component.evidence_hash.data(),
                                    component.evidence_hash.size());
          digest->set_item_count(component.item_count);
          digest->set_payload_offset(component.payload_offset);
          digest->set_payload_length(component.payload_length);
        }
      },
      artifact);
  const auto new_id = summary::ComputeFunctionSummaryId(artifact).value();
  ASSERT_NE(new_id, old_id);
  for (auto &selected : changed.input.summary_selections) {
    if (selected.summary_id != old_id)
      continue;
    selected.summary_id = new_id;
    selected.components = components;
  }
  for (auto &node : *changed.input.provenance.mutable_nodes())
    if (node.summary_id() == core::ToString(old_id))
      node.set_summary_id(core::ToString(new_id));
  changed.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(changed.input);
  EXPECT_NE(changed.input.handoff_digest, request.input.handoff_digest);
  auto changed_case = evidence::EvidenceCaseBuilder().Build(changed);
  ASSERT_TRUE(changed_case.ok()) << changed_case.status().message();
  EXPECT_NE(changed_case->evidence_id, built->evidence_id);
}

TEST(EvidenceHandoffIntegrationTest, RejectsStaleSummaryBinding) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(snapshot.ok());
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok());
  auto summaries =
      summarydb::SummaryRepository::Open(snapshot->output_root.string());
  ASSERT_TRUE(summaries.ok());
  evidence::FactStoreEvidenceBackend backend(
      snapshot->fact_store, snapshot->descriptor, summaries->get());
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, {16, 1024, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto request =
      EvidenceScenarioBuilder().BuildRequest(evidence::EvidenceLevel::kL2);
  request.context.repository_id = snapshot->descriptor.repository;
  request.context.revision_id = snapshot->descriptor.revision;
  request.context.build_variant_id = snapshot->descriptor.build_variant;
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  {
    auto foreign = snapshot->descriptor;
    foreign.revision = core::ToString(EvidenceScenarioBuilder().Id(
        core::IdKind::kRevision, "different-run-context"));
    evidence::FactStoreEvidenceBackend mismatched(snapshot->fact_store, foreign,
                                                  summaries->get());
    auto view = mismatched.OpenSnapshot(snapshot->run_id);
    EXPECT_FALSE(view.ok()) << "the current summary scope must agree with the "
                               "pinned run manifest before reading any refs";
  }
  // The immutable handoff suffices even after the backing bindings are stale.
  // A new assembly must refuse the same stale binding.
  const auto &selection = input->summary_selections.front();
  auto other = snapshot->fact_store.metadata_store().Query(
      "SELECT summary_id FROM summary_bindings WHERE summary_id <> ? LIMIT 1",
      {core::ToString(selection.summary_id)});
  ASSERT_TRUE(other.ok());
  ASSERT_EQ(other->size(), 1);
  ASSERT_TRUE(snapshot->fact_store.metadata_store()
                  .Execute("UPDATE summary_bindings SET summary_id = ? WHERE "
                           "function_variant_id = ?",
                           {(*other)[0][0],
                            core::ToString(selection.function_variant_id)})
                  .ok());
  auto immutable = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(immutable.ok()) << immutable.status().message();
  EXPECT_EQ(immutable->evidence_id, built->evidence_id);
  auto stale = service.BuildEvidenceInput(*seed, {16, 1024, 16, 256, 16});
  EXPECT_FALSE(stale.ok());

  // A supplied artifact/selection cannot be patched and rehashed into a
  // current binding: content identity, component hashes and run all bind it.
  request.input.summary_selections.front()
      .components.front()
      .semantic_hash[0] ^= std::byte{1};
  request.input.handoff_digest =
      evidence::ComputeEvidenceHandoffDigest(request.input);
  EXPECT_FALSE(evidence::EvidenceCaseBuilder().Build(request).ok());
}

TEST(EvidenceHandoffIntegrationTest, NativeUnsupportedEmptyIsNotNegative) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  auto current = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(current.ok());
  auto seed =
      evidence::ResolveOverflowClaimSeed(snapshot->cpg, *current, "memcpy");
  ASSERT_TRUE(seed.ok()) << seed.status().message();
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  const evidence::EvidenceQueryBudget budget{16, 256, 16, 256, 16};
  auto input = service.BuildEvidenceInput(*seed, budget);
  ASSERT_TRUE(input.ok()) << input.status().message();
  ASSERT_TRUE(input->dominating_checks.facts.empty());
  ASSERT_EQ(input->dominating_checks.metadata.completeness,
            evidence::QueryCompleteness::kComplete);
  auto request =
      EvidenceScenarioBuilder().BuildRequest(evidence::EvidenceLevel::kL1);
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  EXPECT_TRUE(std::ranges::none_of(built->facts, [](const auto &fact) {
    return fact.predicate.text == "dominates_bounds_check" &&
           fact.epistemic == evidence::EpistemicState::kMustNot;
  }));
  EXPECT_TRUE(std::ranges::any_of(built->omissions, [](const auto &omission) {
    return omission.kind == "unsupported_predicate" &&
           omission.reason.find("dominating_check") != std::string::npos;
  }));

  // The same real producer, cut before it can publish a path, must retain why.
  auto small = budget;
  small.max_nodes = 1;
  auto truncated = service.BuildEvidenceInput(*seed, small);
  ASSERT_TRUE(truncated.ok()) << truncated.status().message();
  ASSERT_EQ(truncated->flow_slice.metadata.completeness,
            evidence::QueryCompleteness::kTruncated);
  request.input = *truncated;
  auto partial = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(partial.ok()) << partial.status().message();
  EXPECT_TRUE(std::ranges::any_of(partial->omissions, [](const auto &omission) {
    return omission.kind == "truncated_query" &&
           omission.reason.find("value_flow") != std::string::npos &&
           omission.reason.find("max_nodes") != std::string::npos;
  }));
}

TEST(EvidenceHandoffIntegrationTest, MissingOwnerIsUnresolvedScope) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EvidenceScenarioBuilder scenario;
  auto request = scenario.BuildRequest(evidence::EvidenceLevel::kL1);
  auto seed = request.input.claim_seed;
  seed.callsite_ref.reset();
  seed.sink_ref = scenario.Id(core::IdKind::kValueRef, "unowned-sink");
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  evidence::EvidenceQueryService service(snapshot->cpg, backend,
                                         snapshot->run_id);
  auto input = service.BuildEvidenceInput(seed, {16, 256, 16, 256, 16});
  ASSERT_TRUE(input.ok()) << input.status().message();
  ASSERT_TRUE(input->unknowns.facts.empty());
  request.input = *input;
  auto built = evidence::EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  EXPECT_TRUE(std::ranges::any_of(built->unknowns, [](const auto &unknown) {
    return unknown.reason.find("UNRESOLVED_SCOPE") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(built->omissions, [&](const auto &omission) {
    return omission.kind == "unresolved_scope" &&
           omission.subject == built->primary_claim.id;
  }));
}

TEST(EvidenceHandoffIntegrationTest, SummaryFixtureIsDeterministicAcrossRoots) {
  // Two independent materializations in different temporary roots.
  auto first = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(first.ok()) << first.status().message();
  auto second = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(second.ok()) << second.status().message();

  auto first_facts = first->fact_store.GetCurrentFacts(first->run_id);
  ASSERT_TRUE(first_facts.ok()) << first_facts.status().message();
  auto second_facts = second->fact_store.GetCurrentFacts(second->run_id);
  ASSERT_TRUE(second_facts.ok()) << second_facts.status().message();

  // Determinism: identical run identity and identical canonical fact IDs
  // across the two checkout roots. The semantic identity (analysis run, facts,
  // revision/build-variant/repository) is content-addressed and therefore
  // path-independent.
  EXPECT_EQ(first->run_id, second->run_id);
  EXPECT_EQ(first->descriptor.repository, second->descriptor.repository);
  EXPECT_EQ(first->descriptor.revision, second->descriptor.revision);
  EXPECT_EQ(first->descriptor.build_variant, second->descriptor.build_variant);
  EXPECT_EQ(SortedFactIds(*first_facts), SortedFactIds(*second_facts));

  // NOTE: the CPG projection_id (and its module_hash) is NOT asserted equal
  // here. The `-g` flag embeds the absolute source path in the LLVM module's
  // debug info, so module_hash — and therefore projection_id — is
  // path-dependent across checkout roots. That is a real HND-006 gap for the
  // snapshot fingerprint (which folds module_hash into
  // cpg_projection_fingerprint), reported in the task-3 report; the semantic
  // fact identity asserted above is unaffected.

  // The source and sink cross translation units, so the analysis must have
  // crossed the call boundary: a reachable-call fact plus value flow exist.
  EXPECT_TRUE(HasRelation(*first_facts, facts::RelationId::kReachableCall));
  EXPECT_TRUE(HasRelation(*first_facts, facts::RelationId::kGlobalFlow));

  // Summary references are present in the provenance of a derived flow fact,
  // while no source text is copied into the handoff (facts are semantic rows).
  core::StableId flow_fact_id;
  for (const auto& fact : *first_facts) {
    if (fact.row.relation == facts::RelationId::kGlobalFlow) {
      flow_fact_id = fact.fact_id;
      break;
    }
  }
  ASSERT_FALSE(flow_fact_id.digest_hex.empty());

  facts::ExplainBudget budget;
  budget.max_depth = 8;
  budget.max_nodes = 128;
  facts::ProvenanceStore provenance(first->fact_store.metadata_store());
  auto graph = provenance.Explain(first->run_id, flow_fact_id, budget);
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  bool saw_summary_ref = false;
  for (const auto& node : graph->nodes()) {
    if (!node.summary_id().empty()) {
      saw_summary_ref = true;
      break;
    }
  }
  EXPECT_TRUE(saw_summary_ref) << "no summary reference in provenance closure";
}

}  // namespace
}  // namespace veritas::testing
