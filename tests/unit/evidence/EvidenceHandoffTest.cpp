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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "evidence/FakeEvidenceBackend.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/QueryCompletion.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::evidence;
using namespace veritas::facts;
using namespace veritas::testing;

namespace {

namespace sem = analysis::semantic;

EvidenceQueryBudget Budget(std::size_t depth, std::size_t nodes,
                           std::size_t paths, std::size_t facts,
                           std::size_t provenance) {
  EvidenceQueryBudget budget;
  budget.max_depth = depth;
  budget.max_nodes = nodes;
  budget.max_paths = paths;
  budget.max_facts_per_query = facts;
  budget.max_provenance_depth = provenance;
  return budget;
}

SnapshotDescriptor Descriptor(const EvidenceScenarioBuilder& builder) {
  SnapshotDescriptor descriptor;
  descriptor.repository = "repo";
  descriptor.revision = "rev";
  descriptor.build_variant = "variant";
  descriptor.analysis_config = "config";
  descriptor.analysis_run_id = builder.Id(core::IdKind::kAnalysisRun, "run");
  descriptor.fact_snapshot_fingerprint = "fact-fingerprint";
  return descriptor;
}

facts::AnalysisFact MakeFactOrAbort(facts::SemanticRow row) {
  auto fact = facts::MakeFact(std::move(row));
  if (!fact.ok()) {
    std::abort();
  }
  return std::move(fact).value();
}

facts::AnalysisFact RangeFact(const EvidenceScenarioBuilder& builder,
                              std::string_view fn_name,
                              core::StableId memory_ref, std::int64_t offset,
                              std::uint64_t size) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectRead;
  row.cells = {builder.Id(core::IdKind::kFunctionVariant, fn_name), memory_ref,
               sem::ByteRangeKind::kKnown, offset, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact CapacityFact(const EvidenceScenarioBuilder& builder,
                                 std::string_view fn_name,
                                 core::StableId memory_ref,
                                 std::uint64_t size) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectWrite;
  row.cells = {builder.Id(core::IdKind::kFunctionVariant, fn_name), memory_ref,
               sem::ByteRangeKind::kKnown, std::int64_t{0}, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact AliasFact(const EvidenceScenarioBuilder& builder,
                              core::StableId left, core::StableId right,
                              sem::AliasKind kind,
                              sem::EpistemicState epistemic) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {left, right, kind, epistemic};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact UnknownFact(const EvidenceScenarioBuilder& builder,
                                core::StableId function_id,
                                std::string_view subject,
                                std::string_view reason) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kUnknownEffect;
  row.cells = {function_id, std::string(subject), std::string(reason),
               sem::EpistemicState::kUnknown};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact GlobalFlowFact(const EvidenceScenarioBuilder& builder,
                                   core::StableId source, core::StableId sink,
                                   sem::EpistemicState epistemic) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kGlobalFlow;
  row.cells = {source, sink, epistemic};
  return MakeFactOrAbort(std::move(row));
}

// A check fact whose scope_id is the sink's canonical text.
facts::AnalysisFact CheckFact(const EvidenceScenarioBuilder& builder,
                              core::StableId callsite) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kSoundnessCoverage;
  row.cells = {core::ToString(callsite), std::string("dominating_check"),
               std::uint64_t{1}, sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

ClaimSeed Claim(const EvidenceScenarioBuilder& builder) {
  ClaimSeed seed;
  seed.finding_id = builder.Id(core::IdKind::kFact, "finding");
  seed.kind = ClaimKind::kBufferOverflow;
  seed.severity = Severity::kHigh;
  seed.subject_ref = builder.Id(core::IdKind::kMemoryRef, "dstbuf");
  seed.source_ref = builder.Id(core::IdKind::kMemoryRef, "srcbuf");
  seed.sink_ref = builder.Id(core::IdKind::kCallSite, "memcpy");
  return seed;
}

// A CPG with a function containing the sink and a value-flow path
// srcbuf -> v1 -> v2 -> memcpy. The intermediate kValueRef nodes let flow facts
// be scoped to the query's discovered path.
cpg::ThinCpg FlowCpg(const EvidenceScenarioBuilder& builder,
                     const ClaimSeed& seed) {
  cpg::ThinCpg cpg;
  cpg.AddNode(builder.MakeNode("copy", cpg::NodeKind::kFunction));
  cpg.AddNode(builder.MakeNode("srcbuf", cpg::NodeKind::kGlobal));
  cpg.AddNode(builder.MakeNode("v1", cpg::NodeKind::kParameter));
  cpg.AddNode(builder.MakeNode("v2", cpg::NodeKind::kParameter));
  cpg.AddNode(builder.MakeNode("memcpy", cpg::NodeKind::kCallSite));

  const auto v1 = builder.Id(core::IdKind::kValueRef, "v1");
  const auto v2 = builder.Id(core::IdKind::kValueRef, "v2");

  cpg.AddEdge(builder.MakeEdge(
      "contains", cpg::EdgeKind::kContains,
      builder.Id(core::IdKind::kFunctionVariant, "copy"), seed.sink_ref));
  cpg.AddEdge(builder.MakeEdge("flow1", cpg::EdgeKind::kFlowsTo, seed.source_ref,
                               v1));
  cpg.AddEdge(builder.MakeEdge("flow2", cpg::EdgeKind::kFlowsTo, v1, v2));
  cpg.AddEdge(builder.MakeEdge("flow3", cpg::EdgeKind::kFlowsTo, v2,
                               seed.sink_ref));
  return cpg;
}

}  // namespace

// HND-001: a complete unsafe claim bundles every required query result, the
// query-completion facts and run bindings, and a selected witness per result.
TEST(EvidenceHandoffTest, BundlesEveryRequiredQueryResult) {
  EvidenceScenarioBuilder builder;
  const ClaimSeed seed = Claim(builder);
  const auto function_id = builder.Id(core::IdKind::kFunctionVariant, "copy");

  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts({
      RangeFact(builder, "f", seed.source_ref, 0, 65535),
      CapacityFact(builder, "g", seed.subject_ref, 2048),
      AliasFact(builder, seed.subject_ref,
                builder.Id(core::IdKind::kMemoryRef, "other"),
                sem::AliasKind::kMayAlias, sem::EpistemicState::kMay),
      CheckFact(builder, seed.sink_ref),
      UnknownFact(builder, function_id, "vendor_validate", "unmodeled"),
  });

  cpg::ThinCpg cpg = FlowCpg(builder, seed);
  EvidenceQueryService service(cpg, backend, backend.run_id());
  const auto budget = Budget(8, 256, 5, 64, 8);

  auto result = service.BuildEvidenceInput(seed, budget);
  ASSERT_TRUE(result.ok()) << result.status().message();
  const EvidenceBuildInput& input = result.value();

  EXPECT_EQ(input.claim_seed.finding_id, seed.finding_id);
  EXPECT_EQ(input.claim_seed.kind, seed.kind);
  EXPECT_EQ(input.claim_seed.severity, seed.severity);
  EXPECT_EQ(input.claim_seed.subject_ref, seed.subject_ref);
  EXPECT_EQ(input.claim_seed.source_ref, seed.source_ref);
  EXPECT_EQ(input.claim_seed.sink_ref, seed.sink_ref);
  EXPECT_FALSE(input.flow_slice.nodes.empty());
  EXPECT_FALSE(input.flow_slice.edges.empty());
  EXPECT_EQ(input.ranges.facts.size(), 1u);
  EXPECT_EQ(input.capacities.facts.size(), 1u);
  EXPECT_EQ(input.aliases.facts.size(), 1u);
  EXPECT_EQ(input.dominating_checks.facts.size(), 1u);
  EXPECT_EQ(input.unknowns.facts.size(), 1u);

  // Six bounded queries publish six completion facts, each with a run binding
  // and a selected witness in the provenance closure.
  EXPECT_EQ(input.query_completion_facts.size(), 6u);
  EXPECT_EQ(input.query_completion_bindings.size(), 6u);
  EXPECT_GE(input.provenance.nodes_size(), 6);

  for (const auto& fact : input.query_completion_facts) {
    EXPECT_EQ(fact.row.relation, facts::RelationId::kQueryCompletion);
  }
  for (const auto& binding : input.query_completion_bindings) {
    EXPECT_EQ(binding.run_id, backend.run_id());
  }

  // Every result validates at the public boundary.
  EXPECT_TRUE(ValidateQueryResultMetadata(input.flow_slice.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(input.ranges.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(input.capacities.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(input.aliases.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(input.dominating_checks.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(input.unknowns.metadata).ok());

  // Every query provenance reference resolves to a completion fact and a
  // binding in the bundle.
  for (const auto* metadata : {&input.flow_slice.metadata, &input.ranges.metadata,
                               &input.capacities.metadata,
                               &input.aliases.metadata,
                               &input.dominating_checks.metadata,
                               &input.unknowns.metadata}) {
    const auto prov_id = metadata->query_provenance_id;
    bool has_fact = false;
    bool has_binding = false;
    for (const auto& fact : input.query_completion_facts) {
      if (fact.fact_id == prov_id) has_fact = true;
    }
    for (const auto& binding : input.query_completion_bindings) {
      if (binding.fact_id == prov_id) has_binding = true;
    }
    EXPECT_TRUE(has_fact);
    EXPECT_TRUE(has_binding);
  }
}

// HND-002: a backend whose current binding changes mid-assembly produces the
// stable retryable failure, never a mixed-run success.
TEST(EvidenceHandoffTest, UsesOneImmutableSnapshot) {
  EvidenceScenarioBuilder builder;
  const ClaimSeed seed = Claim(builder);
  const auto function_id = builder.Id(core::IdKind::kFunctionVariant, "copy");

  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts({
      RangeFact(builder, "f", seed.source_ref, 0, 65535),
      UnknownFact(builder, function_id, "vendor_validate", "unmodeled"),
  });
  backend.SetAutoBump(true);

  cpg::ThinCpg cpg = FlowCpg(builder, seed);
  EvidenceQueryService service(cpg, backend, backend.run_id());
  auto result = service.BuildEvidenceInput(seed, Budget(8, 256, 5, 64, 8));

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(result.status().message(), "evidence snapshot changed; retry");
}

// HND-003: supporting and contradicting facts for one predicate stay separate,
// retaining IDs and epistemic states; the conflict is not resolved by dropping
// one side. Flow facts are scoped to the query's discovered path, so an
// unrelated value-pair fact is excluded.
TEST(EvidenceHandoffTest, KeepsSupportingAndContradictingFactsSeparate) {
  EvidenceScenarioBuilder builder;
  const ClaimSeed seed = Claim(builder);

  const auto v1 = builder.Id(core::IdKind::kValueRef, "v1");
  const auto v2 = builder.Id(core::IdKind::kValueRef, "v2");

  const auto supporting = GlobalFlowFact(builder, v1, v2, sem::EpistemicState::kMay);
  // kMustNot GlobalFlow is a schema-invalid placeholder by GlobalFlow's
  // allowed_epistemic (which excludes MUST_NOT and is not yet enforced by
  // facts::MakeFact); it stands in for the currently unrepresentable
  // "contradicting flow" shape.
  const auto contradicting =
      GlobalFlowFact(builder, v1, v2, sem::EpistemicState::kMustNot);
  // An unrelated flow fact whose endpoints are not on the src->dst path.
  const auto unrelated =
      GlobalFlowFact(builder, builder.Id(core::IdKind::kValueRef, "ux"),
                     builder.Id(core::IdKind::kValueRef, "uy"),
                     sem::EpistemicState::kMust);

  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts({supporting, contradicting, unrelated});

  cpg::ThinCpg cpg = FlowCpg(builder, seed);
  EvidenceQueryService service(cpg, backend, backend.run_id());
  auto result = service.BuildEvidenceInput(seed, Budget(8, 256, 5, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();

  const FlowSlice& slice = result->flow_slice;
  ASSERT_EQ(slice.supporting_facts.size(), 1u);
  ASSERT_EQ(slice.contradicting_facts.size(), 1u);
  EXPECT_EQ(slice.supporting_facts[0].fact_id, supporting.fact_id);
  EXPECT_EQ(slice.contradicting_facts[0].fact_id, contradicting.fact_id);

  // The unrelated flow fact is excluded from every collection.
  for (const auto& fact : slice.supporting_facts) {
    EXPECT_NE(fact.fact_id, unrelated.fact_id);
  }
  for (const auto& fact : slice.contradicting_facts) {
    EXPECT_NE(fact.fact_id, unrelated.fact_id);
  }
  EXPECT_TRUE(slice.unknowns.empty());

  const auto* supporting_state =
      std::get_if<sem::EpistemicState>(&slice.supporting_facts[0].row.cells[2]);
  const auto* contradicting_state =
      std::get_if<sem::EpistemicState>(&slice.contradicting_facts[0].row.cells[2]);
  ASSERT_NE(supporting_state, nullptr);
  ASSERT_NE(contradicting_state, nullptr);
  EXPECT_EQ(*supporting_state, sem::EpistemicState::kMay);
  EXPECT_EQ(*contradicting_state, sem::EpistemicState::kMustNot);
}

// HND-004: a closed-world dominating-check query that completes empty retains
// its scope, run, query provenance, examined count, and complete state.
TEST(EvidenceHandoffTest, CarriesCompleteEmptyCheckEvidence) {
  EvidenceScenarioBuilder builder;
  const ClaimSeed seed = Claim(builder);

  // One dominating check for a *different* callsite, so this sink has none but
  // the query still examines a candidate.
  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts(
      {CheckFact(builder, builder.Id(core::IdKind::kCallSite, "elsewhere"))});

  cpg::ThinCpg cpg = FlowCpg(builder, seed);
  EvidenceQueryService service(cpg, backend, backend.run_id());
  auto result = service.BuildEvidenceInput(seed, Budget(8, 256, 5, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();

  const EvidenceFactSet& checks = result->dominating_checks;
  EXPECT_TRUE(checks.facts.empty());
  EXPECT_EQ(checks.metadata.completeness, QueryCompleteness::kComplete);
  EXPECT_TRUE(checks.metadata.truncation_reasons.empty());
  EXPECT_EQ(checks.metadata.examined_items, 1u);
  EXPECT_EQ(checks.metadata.analysis_run_id, backend.run_id());
  EXPECT_EQ(checks.metadata.query_provenance_id.kind, core::IdKind::kFact);
}

// HND-005: a dominating-check query that truncates before finding a result is
// distinguishable from HND-004's complete-empty result.
TEST(EvidenceHandoffTest, CarriesTruncatedEmptyCheckEvidence) {
  EvidenceScenarioBuilder builder;
  const ClaimSeed seed = Claim(builder);

  // Many candidate checks (for other callsites) exceed the fact budget, so the
  // closed-world search for this sink truncates without finding a match.
  std::vector<AnalysisFact> facts;
  for (int i = 0; i < 8; ++i) {
    facts.push_back(CheckFact(
        builder,
        builder.Id(core::IdKind::kCallSite, "elsewhere-" + std::to_string(i))));
  }
  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts(facts);

  cpg::ThinCpg cpg = FlowCpg(builder, seed);
  EvidenceQueryService service(cpg, backend, backend.run_id());
  auto result = service.BuildEvidenceInput(seed, Budget(8, 256, 5, 2, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();

  const EvidenceFactSet& checks = result->dominating_checks;
  EXPECT_TRUE(checks.facts.empty());
  EXPECT_EQ(checks.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(checks.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(checks.metadata.truncation_reasons[0], TruncationReason::kMaxFacts);
  EXPECT_EQ(checks.metadata.examined_items, 2u);
}

// M2 carry-forward: ValidateQueryCompletion accepts a self-consistent
// certificate and rejects every mismatched field; it never synthesizes a
// missing witness.
TEST(EvidenceHandoffTest, CompletionValidationRejectsMismatches) {
  EvidenceScenarioBuilder builder;
  const auto run = builder.Id(core::IdKind::kAnalysisRun, "run");

  QueryCompletionDescriptor descriptor;
  descriptor.query_kind = "range";
  descriptor.ordered_scope_refs = {builder.Id(core::IdKind::kMemoryRef, "buf")};
  descriptor.budget = Budget(8, 256, 5, 64, 8);
  descriptor.query_implementation_version = "veritas-evidence-query.v1";
  descriptor.input_snapshot_fingerprint = "fingerprint";
  descriptor.completeness = QueryCompleteness::kComplete;
  descriptor.examined_items = 1;
  descriptor.returned_member_digest = "digest";

  auto fact_or = builder.MakeCompletionFact(descriptor);
  ASSERT_TRUE(fact_or.ok());
  const auto fact = *fact_or;

  const auto binding = builder.MakeBinding(run, fact.fact_id, "w1");
  const auto witness = builder.MakeWitness(run, fact.fact_id, "w1");

  QueryResultMetadata metadata =
      builder.MakeMetadata("run", "prov", QueryCompleteness::kComplete, {}, 1);
  metadata.analysis_run_id = run;
  metadata.query_provenance_id = fact.fact_id;

  EXPECT_TRUE(
      ValidateQueryCompletion(fact, binding, witness, metadata, descriptor)
          .ok());

  auto wrong_digest = descriptor;
  wrong_digest.returned_member_digest = "different-digest";
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, metadata, wrong_digest)
          .ok());

  auto wrong_scope = descriptor;
  wrong_scope.ordered_scope_refs = {builder.Id(core::IdKind::kMemoryRef, "x")};
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, metadata, wrong_scope)
          .ok());

  auto wrong_budget = descriptor;
  wrong_budget.budget = Budget(1, 2, 3, 4, 5);
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, metadata, wrong_budget)
          .ok());

  auto wrong_count = descriptor;
  wrong_count.examined_items = 2;
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, metadata, wrong_count)
          .ok());

  auto wrong_impl = descriptor;
  wrong_impl.query_implementation_version = "other-version";
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, metadata, wrong_impl)
          .ok());

  auto wrong_run = metadata;
  wrong_run.analysis_run_id = builder.Id(core::IdKind::kAnalysisRun, "other");
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, witness, wrong_run, descriptor)
          .ok());

  auto wrong_producer = witness;
  wrong_producer.producer_kind = facts::ProducerKind::kWpaCppConformance;
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, wrong_producer, metadata,
                              descriptor)
          .ok());

  auto unselected = witness;
  unselected.selected = false;
  EXPECT_FALSE(
      ValidateQueryCompletion(fact, binding, unselected, metadata, descriptor)
          .ok());

  auto missing_witness = witness;
  missing_witness.output_fact_id = builder.Id(core::IdKind::kFact, "other");
  EXPECT_FALSE(ValidateQueryCompletion(fact, binding, missing_witness, metadata,
                                       descriptor)
                   .ok());
}
