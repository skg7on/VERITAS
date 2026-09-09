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

// OverflowEvidenceFixtureTest.cpp — the real-pipeline evidence layer.
//
// These tests run the full M1→M6→M9→M10A pipeline on the seven overflow
// fixtures, bind the durable CPG + FactStore through the concrete
// FactStoreEvidenceBackend, and assert the facts the pipeline actually
// produces. They assert typed required outputs and forbidden outputs before
// any presentation-level comparison.
//
// IMPORTANT: the current M9/M10A pipeline produces the flow closure
// (GlobalFlow), the unknown-effect surface (UnknownEffect, including the
// opaque validator), reachability, and the negative soundness-coverage
// certificate ("dominating_check_absence"). It does NOT yet materialize the
// value-range, capacity, positive dominating-check, or alias facts the
// evidence demo (§5 of the M10B design spec) expects; those queries therefore
// return complete-empty rather than manufacturing a fact. That gap is the
// subject of the task-3 report and is asserted here as a forbidden-output
// guarantee (no fabricated MUST_ALIAS, no fabricated positive check, no
// fabricated range/capacity), never as a pass on the demo oracle.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/FactStoreEvidenceBackend.h"
#include "evidence/RealEvidencePipeline.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::testing {
namespace {

namespace sem = analysis::semantic;
namespace ev = evidence;

ev::EvidenceQueryBudget Budget() {
  return ev::EvidenceQueryBudget{/*max_depth=*/16, /*max_nodes=*/512,
                                 /*max_paths=*/64, /*max_facts_per_query=*/128,
                                 /*max_provenance_depth=*/16};
}

const std::string* StringCell(const facts::SemanticRow& row, std::size_t index) {
  return index < row.cells.size() ? std::get_if<std::string>(&row.cells[index])
                                  : nullptr;
}

std::vector<facts::AnalysisFact> FactsOfRelation(
    const std::vector<facts::AnalysisFact>& facts, facts::RelationId relation) {
  std::vector<facts::AnalysisFact> matches;
  for (const auto& fact : facts) {
    if (fact.row.relation == relation) {
      matches.push_back(fact);
    }
  }
  return matches;
}

// The single function-variant node in a single-function fixture.
StatusOr<core::StableId> FunctionNode(const cpg::ThinCpg& cpg) {
  for (const auto& node : cpg.nodes()) {
    if (node.kind == cpg::NodeKind::kFunction) {
      return node.node_id;
    }
  }
  return Status::NotFound("no function node in CPG projection");
}

bool HasUnknownWithReason(const std::vector<facts::AnalysisFact>& facts,
                          std::string_view reason) {
  for (const auto& fact : facts) {
    if (fact.row.relation != facts::RelationId::kUnknownEffect &&
        fact.row.relation != facts::RelationId::kSupportUnknownEffect) {
      continue;
    }
    const std::string* cell = StringCell(fact.row, 2);
    if (cell != nullptr && *cell == reason) {
      return true;
    }
  }
  return false;
}

TEST(OverflowEvidenceFixtureTest, UnsafeFixtureProducesFlowAndUnknownFacts) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(facts.ok()) << facts.status().message();

  // Flow closure is derived (QRY-001's flow requirement).
  EXPECT_FALSE(FactsOfRelation(*facts, facts::RelationId::kGlobalFlow).empty());

  // The unmodeled memcpy call surfaces as an unknown effect on the sink's
  // function (the opaque-callee half of the unknown surface).
  EXPECT_TRUE(HasUnknownWithReason(*facts, "llvm.memcpy.p0.p0.i64"));

  // Forbidden: the pipeline must never manufacture a positive dominating-check
  // fact. The only soundness-coverage fact it derives is the negative
  // "dominating_check_absence" certificate.
  const auto coverage =
      FactsOfRelation(*facts, facts::RelationId::kSoundnessCoverage);
  for (const auto& fact : coverage) {
    const std::string* kind = StringCell(fact.row, 1);
    ASSERT_NE(kind, nullptr);
    EXPECT_NE(*kind, "dominating_check") << "fabricated positive check fact";
  }
}

TEST(OverflowEvidenceFixtureTest, OpaqueValidatorRemainsUnknown) {
  // QRY-009: the external validator has no model, so its postcondition stays
  // unknown. No assumed postcondition and no negative check fact.
  auto snapshot = AnalyzeRealFixture("evidence_overflow_opaque_validator");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(facts.ok()) << facts.status().message();

  EXPECT_TRUE(HasUnknownWithReason(*facts, "vendor_validate"));

  // Forbidden: the unknown external validator must not be promoted to a
  // dominating check (positive) or a MUST_NOT/negative fact.
  const auto coverage =
      FactsOfRelation(*facts, facts::RelationId::kSoundnessCoverage);
  for (const auto& fact : coverage) {
    const std::string* kind = StringCell(fact.row, 1);
    ASSERT_NE(kind, nullptr);
    EXPECT_NE(*kind, "dominating_check");
  }
}

TEST(OverflowEvidenceFixtureTest, DescriptorAgreesWithCpgProjectionMetadata) {
  // T2m4: the snapshot descriptor strings must agree with the CPG's
  // ProjectionMetadata rather than silently disagreeing into a different
  // fingerprint.
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  EXPECT_EQ(snapshot->descriptor.revision,
            core::ToString(snapshot->cpg.metadata().revision_id));
  EXPECT_EQ(snapshot->descriptor.build_variant,
            core::ToString(snapshot->cpg.metadata().build_variant_id));
  EXPECT_EQ(snapshot->descriptor.analysis_run_id, snapshot->run_id);
  EXPECT_EQ(core::ToString(snapshot->run_id), snapshot->analysis.wpa_run_id);
}

TEST(OverflowEvidenceFixtureTest, UnknownsQuerySurfacesRealFacts) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_opaque_validator");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  auto function = FunctionNode(snapshot->cpg);
  ASSERT_TRUE(function.ok()) << function.status().message();

  FactStoreEvidenceBackend backend(snapshot->fact_store, snapshot->descriptor);
  ev::EvidenceQueryService service(snapshot->cpg, backend, snapshot->run_id);

  auto unknowns = service.GetUnknowns(*function, Budget());
  ASSERT_TRUE(unknowns.ok()) << unknowns.status().message();
  EXPECT_EQ(unknowns->metadata.completeness, ev::QueryCompleteness::kComplete);
  EXPECT_FALSE(unknowns->facts.empty());
  bool saw_vendor = false;
  for (const auto& fact : unknowns->facts) {
    const std::string* reason = StringCell(fact.row, 2);
    saw_vendor = saw_vendor || (reason != nullptr && *reason == "vendor_validate");
  }
  EXPECT_TRUE(saw_vendor) << "vendor_validate unknown not surfaced";
}

TEST(OverflowEvidenceFixtureTest, NoRangeCapacityAliasOrCheckFactIsManufactured) {
  // Forbidden-output guarantees for the not-yet-materialized relations: the
  // query service must return a complete empty result (open world) rather than
  // fabricating range, capacity, alias, or positive-check facts.
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  auto function = FunctionNode(snapshot->cpg);
  ASSERT_TRUE(function.ok()) << function.status().message();

  FactStoreEvidenceBackend backend(snapshot->fact_store, snapshot->descriptor);
  ev::EvidenceQueryService service(snapshot->cpg, backend, snapshot->run_id);

  const core::StableId ref = *function;

  auto ranges = service.GetRanges(ref, Budget());
  ASSERT_TRUE(ranges.ok()) << ranges.status().message();
  EXPECT_EQ(ranges->metadata.completeness, ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(ranges->facts.empty());

  auto capacities = service.GetCapacities(ref, Budget());
  ASSERT_TRUE(capacities.ok()) << capacities.status().message();
  EXPECT_EQ(capacities->metadata.completeness, ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(capacities->facts.empty());

  auto aliases = service.GetAliases(ref, Budget());
  ASSERT_TRUE(aliases.ok()) << aliases.status().message();
  EXPECT_EQ(aliases->metadata.completeness, ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(aliases->facts.empty());
  for (const auto& fact : aliases->facts) {
    const auto* kind = std::get_if<sem::AliasKind>(&fact.row.cells[2]);
    ASSERT_NE(kind, nullptr);
    EXPECT_NE(*kind, sem::AliasKind::kMustAlias)
        << "fabricated MUST_ALIAS fact";
  }

  auto checks = service.GetDominatingChecks(ref, Budget());
  ASSERT_TRUE(checks.ok()) << checks.status().message();
  EXPECT_EQ(checks->metadata.completeness, ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(checks->facts.empty());
}

TEST(OverflowEvidenceFixtureTest, ExplainReturnsProvenanceGraph) {
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(facts.ok()) << facts.status().message();
  const auto flows = FactsOfRelation(*facts, facts::RelationId::kGlobalFlow);
  ASSERT_FALSE(flows.empty());
  const core::StableId fact_id = flows.front().fact_id;

  facts::ExplainBudget budget;
  budget.max_depth = 8;
  budget.max_nodes = 128;

  facts::ProvenanceStore provenance(snapshot->fact_store.metadata_store());
  auto explained = provenance.Explain(snapshot->run_id, fact_id, budget);
  ASSERT_TRUE(explained.ok()) << explained.status().message();
  EXPECT_EQ(explained->fact_id(), core::ToString(fact_id));
  EXPECT_GT(explained->nodes_size(), 0);
}

}  // namespace
}  // namespace veritas::testing
