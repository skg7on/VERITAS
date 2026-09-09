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

// A DirectRead range fact with a caller-supplied memory reference, so several
// facts can share one value/object and stress the fact budget.
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

// A value-flow edge through a pair of kParameter nodes (kValueRef IDs).
struct FlowEdge {
  cpg::CpgNode from;
  cpg::CpgNode to;
  cpg::CpgEdge edge;
};

FlowEdge Link(const EvidenceScenarioBuilder& builder, std::string_view from,
              std::string_view to, std::string_view edge_name) {
  FlowEdge link;
  link.from = builder.MakeNode(from, cpg::NodeKind::kParameter);
  link.to = builder.MakeNode(to, cpg::NodeKind::kParameter);
  link.edge = builder.MakeEdge(edge_name, cpg::EdgeKind::kFlowsTo,
                               link.from.node_id, link.to.node_id);
  return link;
}

cpg::ThinCpg BuildCpg(std::vector<FlowEdge> links) {
  cpg::ThinCpg cpg;
  for (auto& link : links) {
    cpg.AddNode(link.from);
    cpg.AddNode(link.to);
    cpg.AddEdge(link.edge);
  }
  return cpg;
}

std::vector<core::StableId> Sorted(const std::vector<core::StableId>& ids) {
  std::vector<core::StableId> out = ids;
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<core::StableId> NodeIds(const FlowSlice& slice) {
  std::vector<core::StableId> ids;
  ids.reserve(slice.nodes.size());
  for (const auto& node : slice.nodes) {
    ids.push_back(node.node_id);
  }
  return Sorted(ids);
}

}  // namespace

// QRY-002: a disconnected graph with an ample budget is complete-empty, never
// an error and never a truncated result.
TEST(EvidenceQueryServiceTest, NoPathIsCompleteEmpty) {
  EvidenceScenarioBuilder builder;
  const auto src = builder.Id(core::IdKind::kValueRef, "src");
  const auto dst = builder.Id(core::IdKind::kValueRef, "dst");

  cpg::ThinCpg cpg;
  cpg.AddNode(builder.MakeNode("src", cpg::NodeKind::kParameter));
  cpg.AddNode(builder.MakeNode("dst", cpg::NodeKind::kParameter));

  FakeEvidenceBackend backend(Descriptor(builder));
  EvidenceQueryService service(cpg, backend, backend.run_id());

  auto result = service.GetValueFlow(src, dst, Budget(8, 256, 5, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();
  const FlowSlice& slice = result.value();
  EXPECT_TRUE(slice.nodes.empty());
  EXPECT_TRUE(slice.edges.empty());
  EXPECT_EQ(slice.metadata.completeness, QueryCompleteness::kComplete);
  EXPECT_TRUE(slice.metadata.truncation_reasons.empty());
  EXPECT_EQ(slice.metadata.examined_items, 0u);
  EXPECT_EQ(slice.metadata.analysis_run_id, backend.run_id());
  EXPECT_EQ(slice.metadata.query_provenance_id.kind, core::IdKind::kFact);
  EXPECT_TRUE(ValidateQueryResultMetadata(slice.metadata).ok());
}

// QRY-003 (depth): a path longer than max_depth is truncated-empty with the
// exact depth reason, never misreported as a complete empty result.
TEST(EvidenceQueryServiceTest, DepthBudgetTruncatesEmpty) {
  EvidenceScenarioBuilder builder;
  const auto src = builder.Id(core::IdKind::kValueRef, "src");
  const auto dst = builder.Id(core::IdKind::kValueRef, "dst");

  auto l1 = Link(builder, "src", "a", "e1");
  auto l2 = Link(builder, "a", "dst", "e2");
  cpg::ThinCpg cpg = BuildCpg({std::move(l1), std::move(l2)});

  FakeEvidenceBackend backend(Descriptor(builder));
  EvidenceQueryService service(cpg, backend, backend.run_id());

  auto result = service.GetValueFlow(src, dst, Budget(1, 256, 5, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();
  const FlowSlice& slice = result.value();
  EXPECT_TRUE(slice.nodes.empty());
  EXPECT_EQ(slice.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(slice.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(slice.metadata.truncation_reasons[0], TruncationReason::kMaxDepth);
}

// QRY-003 (paths): one more simple path than max_paths returns the canonical
// prefix and reports kMaxPaths with an examined count that proves the probe.
TEST(EvidenceQueryServiceTest, PathBudgetReturnsCanonicalPrefix) {
  EvidenceScenarioBuilder builder;
  const auto src = builder.Id(core::IdKind::kValueRef, "src");
  const auto dst = builder.Id(core::IdKind::kValueRef, "dst");

  cpg::ThinCpg cpg = BuildCpg({
      Link(builder, "src", "a", "e_a"),
      Link(builder, "a", "dst", "e_a_dst"),
      Link(builder, "src", "b", "e_b"),
      Link(builder, "b", "dst", "e_b_dst"),
      Link(builder, "src", "c", "e_c"),
      Link(builder, "c", "dst", "e_c_dst"),
  });

  FakeEvidenceBackend backend(Descriptor(builder));
  EvidenceQueryService service(cpg, backend, backend.run_id());

  auto result = service.GetValueFlow(src, dst, Budget(8, 256, 2, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();
  const FlowSlice& slice = result.value();

  EXPECT_EQ(slice.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(slice.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(slice.metadata.truncation_reasons[0], TruncationReason::kMaxPaths);
  EXPECT_EQ(slice.metadata.examined_items, 3u);
  EXPECT_EQ(slice.nodes.size(), 4u);  // src + two intermediates + dst
  EXPECT_EQ(slice.edges.size(), 4u);

  // The two returned intermediates are the two smallest of {a, b, c} by
  // canonical node ID, not by symbolic name.
  const auto a = builder.Id(core::IdKind::kValueRef, "a");
  const auto b = builder.Id(core::IdKind::kValueRef, "b");
  const auto c = builder.Id(core::IdKind::kValueRef, "c");
  std::vector<core::StableId> expected = Sorted({a, b, c});
  expected.resize(2);

  std::vector<core::StableId> intermediates;
  for (const auto& node : slice.nodes) {
    if (node.node_id != src && node.node_id != dst) {
      intermediates.push_back(node.node_id);
    }
  }
  EXPECT_EQ(Sorted(intermediates), Sorted(expected));
}

// QRY-003 (nodes): a path that would push the slice past max_nodes truncates
// with kMaxNodes while retaining the canonical node prefix already assembled.
TEST(EvidenceQueryServiceTest, NodeBudgetRetainsCanonicalPrefix) {
  EvidenceScenarioBuilder builder;
  const auto src = builder.Id(core::IdKind::kValueRef, "src");
  const auto dst = builder.Id(core::IdKind::kValueRef, "dst");

  cpg::ThinCpg cpg = BuildCpg({
      Link(builder, "src", "a", "e_a"),
      Link(builder, "a", "dst", "e_a_dst"),
      Link(builder, "src", "b", "e_b"),
      Link(builder, "b", "c", "e_b_c"),
      Link(builder, "c", "dst", "e_c_dst"),
  });

  FakeEvidenceBackend backend(Descriptor(builder));
  EvidenceQueryService service(cpg, backend, backend.run_id());

  auto result = service.GetValueFlow(src, dst, Budget(8, 4, 5, 64, 8));
  ASSERT_TRUE(result.ok()) << result.status().message();
  const FlowSlice& slice = result.value();

  EXPECT_EQ(slice.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(slice.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(slice.metadata.truncation_reasons[0], TruncationReason::kMaxNodes);
  EXPECT_EQ(slice.metadata.examined_items, 2u);

  // The canonical-first path must be fully retained; its node set equals the
  // lexicographically smaller of the two path node-id sequences.
  const auto a = builder.Id(core::IdKind::kValueRef, "a");
  const auto b = builder.Id(core::IdKind::kValueRef, "b");
  const auto c = builder.Id(core::IdKind::kValueRef, "c");
  std::vector<std::vector<core::StableId>> paths = {
      {src, a, dst},
      {src, b, c, dst},
  };
  std::sort(paths.begin(), paths.end());
  EXPECT_EQ(NodeIds(slice), Sorted(paths[0]));
}

// QRY-004: the fact budget applies independently to the range and capacity
// queries; one query's truncation never contaminates the other.
TEST(EvidenceQueryServiceTest, RangeAndCapacityFactBudgetsAreVisible) {
  EvidenceScenarioBuilder builder;
  const auto mem = builder.Id(core::IdKind::kMemoryRef, "buf");

  std::vector<AnalysisFact> facts = {
      RangeFact(builder, "f1", mem, 0, 4),
      RangeFact(builder, "f2", mem, 4, 8),
      RangeFact(builder, "f3", mem, 8, 12),
      CapacityFact(builder, "g1", mem, 64),
      CapacityFact(builder, "g2", mem, 128),
      CapacityFact(builder, "g3", mem, 256),
  };
  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts(facts);

  cpg::ThinCpg cpg;
  EvidenceQueryService service(cpg, backend, backend.run_id());
  const auto budget = Budget(8, 256, 5, 2, 8);

  auto ranges = service.GetRanges(mem, budget);
  ASSERT_TRUE(ranges.ok()) << ranges.status().message();
  EXPECT_EQ(ranges->facts.size(), 2u);
  EXPECT_EQ(ranges->metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(ranges->metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(ranges->metadata.truncation_reasons[0], TruncationReason::kMaxFacts);
  EXPECT_EQ(ranges->metadata.examined_items, 3u);
  for (const auto& fact : ranges->facts) {
    EXPECT_EQ(fact.row.relation, facts::RelationId::kDirectRead);
  }

  auto capacities = service.GetCapacities(mem, budget);
  ASSERT_TRUE(capacities.ok()) << capacities.status().message();
  EXPECT_EQ(capacities->facts.size(), 2u);
  EXPECT_EQ(capacities->metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(capacities->metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(capacities->metadata.truncation_reasons[0], TruncationReason::kMaxFacts);
  EXPECT_EQ(capacities->metadata.examined_items, 3u);
  for (const auto& fact : capacities->facts) {
    EXPECT_EQ(fact.row.relation, facts::RelationId::kDirectWrite);
  }

  // Canonical prefix: the returned facts are the two smallest by fact ID.
  std::vector<AnalysisFact> range_candidates = {
      facts[0], facts[1], facts[2]};
  std::sort(range_candidates.begin(), range_candidates.end(),
            [](const AnalysisFact& x, const AnalysisFact& y) {
              return x.fact_id < y.fact_id;
            });
  EXPECT_EQ(ranges->facts[0].fact_id, range_candidates[0].fact_id);
  EXPECT_EQ(ranges->facts[1].fact_id, range_candidates[1].fact_id);
}

// QRY-008: all four alias semantic values round-trip unchanged, keeping their
// epistemic states independent; no alias kind is converted to another.
TEST(EvidenceQueryServiceTest, PreservesAllAliasStates) {
  EvidenceScenarioBuilder builder;
  const auto mem = builder.Id(core::IdKind::kMemoryRef, "obj");
  const auto other = builder.Id(core::IdKind::kMemoryRef, "other");
  const sem::EpistemicState epistemic = sem::EpistemicState::kMay;

  const std::vector<sem::AliasKind> kinds = {
      sem::AliasKind::kMustAlias, sem::AliasKind::kMayAlias,
      sem::AliasKind::kNoAlias, sem::AliasKind::kUnknownAlias};

  for (const sem::AliasKind kind : kinds) {
    const auto fact = AliasFact(builder, mem, other, kind, epistemic);

    FakeEvidenceBackend backend(Descriptor(builder));
    backend.SetFacts({fact});
    cpg::ThinCpg cpg;
    EvidenceQueryService service(cpg, backend, backend.run_id());

    auto result = service.GetAliases(mem, Budget(8, 256, 5, 64, 8));
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result->facts.size(), 1u);
    EXPECT_EQ(result->facts[0].fact_id, fact.fact_id);

    const auto* alias =
        std::get_if<sem::AliasKind>(&result->facts[0].row.cells[2]);
    const auto* state =
        std::get_if<sem::EpistemicState>(&result->facts[0].row.cells[3]);
    ASSERT_NE(alias, nullptr);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(*alias, kind);
    EXPECT_EQ(*state, epistemic);
  }
}

// QRY-010: provenance truncation is independent and visible — the semantic
// fact remains present while the provenance graph reports its own reason.
TEST(EvidenceQueryServiceTest, ProvenanceBudgetIsIndependentAndVisible) {
  EvidenceScenarioBuilder builder;
  const auto mem = builder.Id(core::IdKind::kMemoryRef, "buf");
  const auto fact = RangeFact(builder, "f1", mem, 0, 4);

  FakeEvidenceBackend backend(Descriptor(builder));
  backend.SetFacts({fact});
  backend.SetProvenanceDepth(5);

  cpg::ThinCpg cpg;
  EvidenceQueryService service(cpg, backend, backend.run_id());

  // The semantic fact is still returned in full by the range query.
  auto ranges = service.GetRanges(mem, Budget(8, 256, 5, 64, 8));
  ASSERT_TRUE(ranges.ok()) << ranges.status().message();
  ASSERT_EQ(ranges->facts.size(), 1u);
  EXPECT_EQ(ranges->facts[0].fact_id, fact.fact_id);

  // A tight provenance budget truncates the explanation but keeps the fact.
  facts::ExplainBudget explain_budget;
  explain_budget.max_depth = 1;
  auto provenance = service.Explain(backend.run_id(), fact.fact_id,
                                    explain_budget);
  ASSERT_TRUE(provenance.ok()) << provenance.status().message();
  EXPECT_TRUE(provenance->truncated());
  EXPECT_EQ(provenance->truncation_reason(), "max_depth");
  EXPECT_EQ(provenance->fact().fact_id(), core::ToString(fact.fact_id));
}
