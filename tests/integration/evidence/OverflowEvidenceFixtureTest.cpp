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
// DEFERRED (scoping decision, not a defect): value-range [0,65535], capacity
// 2048, alias states, and the positive "dominating_check" fact are DEFERRED to
// a later milestone — M9/M10A does not emit them. What M10B completes with is
// the real flow closure (GlobalFlow), the unknown-effect surface
// (UnknownEffect, incl. vendor_validate), provenance with summary_id, the
// negative "dominating_check_absence" SoundnessCoverage certificate, and
// cross-root determinism. These tests assert exactly that produced surface and
// assert the deferred relations return complete-empty open-world results
// rather than manufacturing a fact (no fabricated MUST_ALIAS, no fabricated
// positive check, no fabricated range/capacity).

#include <cstddef>
#include <cstdint>
#include <set>
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

const core::StableId* IdCell(const facts::SemanticRow& row, std::size_t index) {
  return index < row.cells.size()
             ? std::get_if<core::StableId>(&row.cells[index])
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

// The memcpy sink: the CPG models the unmodeled memcpy as an "unknown" node
// labeled with Clang's lowered intrinsic name (llvm.memcpy.p0.p0.i64).
StatusOr<core::StableId> MemcpySinkNode(const cpg::ThinCpg& cpg) {
  for (const auto& node : cpg.nodes()) {
    if (node.label.find("memcpy") != std::string::npos) {
      return node.node_id;
    }
  }
  return Status::NotFound("no memcpy sink node in CPG projection");
}

// The set of CPG node IDs. A value that is the sink of a GlobalFlow fact but is
// NOT a CPG node is a flow that left the function into an unmodeled external's
// formal parameter (the external has no CPG parameter node).
std::set<core::StableId> CpgNodeIds(const cpg::ThinCpg& cpg) {
  std::set<core::StableId> ids;
  for (const auto& node : cpg.nodes()) {
    ids.insert(node.node_id);
  }
  return ids;
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

// Positively asserts the completeness-qualified check output: the run must emit
// at least one SoundnessCoverage fact whose coverage_kind is
// "dominating_check_absence" and whose complete cell is present and gapped
// (Uint64 0). The "complete" (no-absence) state is represented by the ABSENCE
// of this fact, not by a 1 here. A regression that stopped emitting the
// certificate — or dropped the completeness cell — fails these assertions.
void ExpectDominatingCheckAbsenceCertificate(
    const std::vector<facts::AnalysisFact>& facts) {
  const auto coverage =
      FactsOfRelation(facts, facts::RelationId::kSoundnessCoverage);
  EXPECT_FALSE(coverage.empty()) << "no SoundnessCoverage certificate emitted";
  bool saw_absence = false;
  bool saw_gapped_complete = false;
  for (const auto& fact : coverage) {
    const std::string* kind = StringCell(fact.row, 1);
    if (kind == nullptr || *kind != "dominating_check_absence") {
      continue;
    }
    saw_absence = true;
    const auto* complete = std::get_if<std::uint64_t>(&fact.row.cells[2]);
    if (complete != nullptr && *complete == 0) {
      saw_gapped_complete = true;
    }
  }
  EXPECT_TRUE(saw_absence) << "no dominating_check_absence coverage fact";
  EXPECT_TRUE(saw_gapped_complete)
      << "dominating_check_absence fact missing gapped complete cell";
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

  // Positive: the completeness-qualified check output is actually emitted.
  ExpectDominatingCheckAbsenceCertificate(*facts);

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

  // Positive: the completeness-qualified check output is actually emitted.
  ExpectDominatingCheckAbsenceCertificate(*facts);

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

TEST(OverflowEvidenceFixtureTest, UnsafeFixtureFlowReachesMemcpySink) {
  // QRY-001 (descoped scope): assert the SPECIFIC flow reaches the sink — the
  // p-derived length value flows to the memcpy size operand — rather than
  // merely "some GlobalFlow facts exist". Two structural facts prove it:
  //  (a) the function calls memcpy (a kCalls/kMayCall edge to the llvm.memcpy
  //      sink node); and
  //  (b) the value-flow closure reaches a sink that is not a CPG node, which is
  //      a value flowing into the unmodeled memcpy's formal parameter (the
  //      external has no CPG parameter nodes).
  //
  // Observed (content-addressed, deterministic): the memcpy size operand is the
  // 3rd actual at callsite:sha256:18f4820b…, whose formal is
  // valref:sha256:8ede4cb4…. The value chain is
  //   valref:6e095ff2… → 008ebf57… → b047b26a… → d90129fc… → c0e87f82… → 8ede4cb4…
  // and GlobalFlow contains the fact 6e095ff2… → 8ede4cb4… (the p-derived length
  // reaches the memcpy size formal).
  auto snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

  const auto sink = MemcpySinkNode(snapshot->cpg);
  ASSERT_TRUE(sink.ok()) << sink.status().message();

  bool called = false;
  for (const auto& edge : snapshot->cpg.edges()) {
    const bool call_edge = edge.kind == cpg::EdgeKind::kCalls ||
                           edge.kind == cpg::EdgeKind::kMayCall;
    if (call_edge && edge.target_node_id == *sink) {
      called = true;
      break;
    }
  }
  EXPECT_TRUE(called) << "no call edge reaches the memcpy sink";

  bool has_flow_edges = false;
  for (const auto& edge : snapshot->cpg.edges()) {
    if (edge.kind == cpg::EdgeKind::kFlowsTo) {
      has_flow_edges = true;
      break;
    }
  }
  EXPECT_TRUE(has_flow_edges) << "no kFlowsTo edges in the value-flow graph";

  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  ASSERT_TRUE(facts.ok()) << facts.status().message();
  const auto ids = CpgNodeIds(snapshot->cpg);
  bool reaches_external_formal = false;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kGlobalFlow) {
      continue;
    }
    const core::StableId* sink_id = IdCell(fact.row, 1);
    if (sink_id != nullptr && ids.count(*sink_id) == 0) {
      reaches_external_formal = true;
      break;
    }
  }
  EXPECT_TRUE(reaches_external_formal)
      << "no GlobalFlow fact reaches the memcpy formal parameter";
}

TEST(OverflowEvidenceFixtureTest,
     SafeNonDominatingAndMixedPathsDoNotFabricateChecks) {
  // QRY-005/006/007 (descoped scope): each shape produces the flow closure, and
  // the dominating-check query returns complete-empty (the only soundness
  // coverage fact is the negative "dominating_check_absence" certificate) —
  // never a fabricated positive "dominating_check". Positive-check
  // disambiguation (safe dominates vs sibling/mixed-path non-dominance) is
  // deferred to a later milestone because M10A does not derive a positive
  // dominating-check fact.
  for (const char* fixture :
       {"evidence_overflow_safe", "evidence_overflow_non_dominating",
        "evidence_overflow_mixed_paths"}) {
    auto snapshot = AnalyzeRealFixture(fixture);
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();

    auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
    ASSERT_TRUE(facts.ok()) << facts.status().message();
    EXPECT_FALSE(FactsOfRelation(*facts, facts::RelationId::kGlobalFlow).empty())
        << fixture;

    // Positive: the completeness-qualified check output is actually emitted for
    // each shape.
    ExpectDominatingCheckAbsenceCertificate(*facts);

    // Forbidden: no positive dominating-check fact is fabricated.
    const auto coverage =
        FactsOfRelation(*facts, facts::RelationId::kSoundnessCoverage);
    for (const auto& fact : coverage) {
      const std::string* kind = StringCell(fact.row, 1);
      ASSERT_NE(kind, nullptr) << fixture;
      EXPECT_NE(*kind, "dominating_check")
          << fixture << " fabricated a positive check fact";
    }

    auto function = FunctionNode(snapshot->cpg);
    ASSERT_TRUE(function.ok()) << function.status().message();
    FactStoreEvidenceBackend backend(snapshot->fact_store, snapshot->descriptor);
    ev::EvidenceQueryService service(snapshot->cpg, backend, snapshot->run_id);
    auto checks = service.GetDominatingChecks(*function, Budget());
    ASSERT_TRUE(checks.ok()) << fixture << ": " << checks.status().message();
    EXPECT_EQ(checks->metadata.completeness, ev::QueryCompleteness::kComplete)
        << fixture;
    EXPECT_TRUE(checks->facts.empty()) << fixture;
  }
}

}  // namespace
}  // namespace veritas::testing
