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

#include "evidence/EvidenceScenario.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
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

// A valid, complete base metadata record against which invalid shapes are
// derived.
QueryResultMetadata ValidMetadata() {
  EvidenceScenarioBuilder builder;
  return builder.MakeMetadata("run", "provenance", QueryCompleteness::kComplete,
                              {}, 0);
}

std::vector<QueryResultMetadata> InvalidMetadataCases() {
  EvidenceScenarioBuilder builder;

  QueryResultMetadata unspecified = ValidMetadata();
  unspecified.completeness = QueryCompleteness::kUnspecified;

  QueryResultMetadata complete_with_reason = ValidMetadata();
  complete_with_reason.truncation_reasons = {TruncationReason::kMaxFacts};

  QueryResultMetadata truncated_no_reason = ValidMetadata();
  truncated_no_reason.completeness = QueryCompleteness::kTruncated;
  truncated_no_reason.truncation_reasons.clear();

  QueryResultMetadata missing_run = ValidMetadata();
  missing_run.analysis_run_id = core::StableId{};

  QueryResultMetadata wrong_kind_run = ValidMetadata();
  wrong_kind_run.analysis_run_id =
      builder.Id(core::IdKind::kFact, "not-a-run");

  QueryResultMetadata missing_provenance = ValidMetadata();
  missing_provenance.query_provenance_id = core::StableId{};

  QueryResultMetadata wrong_kind_provenance = ValidMetadata();
  wrong_kind_provenance.query_provenance_id =
      builder.Id(core::IdKind::kAnalysisRun, "not-a-fact");

  return {unspecified,      complete_with_reason, truncated_no_reason,
          missing_run,      wrong_kind_run,       missing_provenance,
          wrong_kind_provenance};
}

const char* InvalidMetadataCaseName(std::size_t index) {
  switch (index) {
    case 0: return "unspecified";
    case 1: return "complete_with_reason";
    case 2: return "truncated_no_reason";
    case 3: return "missing_run";
    case 4: return "wrong_kind_run";
    case 5: return "missing_provenance";
    case 6: return "wrong_kind_provenance";
    default: return "unknown";
  }
}

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

EvidenceFactSet MakeFactSet(const EvidenceScenarioBuilder& builder,
                            std::string_view run, std::string_view provenance,
                            QueryCompleteness completeness,
                            std::vector<TruncationReason> reasons,
                            std::size_t examined,
                            std::vector<AnalysisFact> facts) {
  EvidenceFactSet set;
  set.facts = std::move(facts);
  set.metadata =
      builder.MakeMetadata(run, provenance, completeness, std::move(reasons),
                           examined);
  return set;
}

// A fully populated EvidenceBuildInput exercising every one of the ten fields.
EvidenceBuildInput BuildInput() {
  EvidenceScenarioBuilder builder;

  EvidenceBuildInput input;
  input.claim_seed.finding_id = builder.Id(core::IdKind::kFact, "finding");
  input.claim_seed.kind = ClaimKind::kBufferOverflow;
  input.claim_seed.severity = Severity::kHigh;
  input.claim_seed.subject_ref = builder.Id(core::IdKind::kCallSite, "sink");
  input.claim_seed.source_ref = builder.Id(core::IdKind::kValueRef, "length");
  input.claim_seed.sink_ref = builder.Id(core::IdKind::kCallSite, "memcpy");

  const auto source = builder.MakeNode("length", cpg::NodeKind::kParameter);
  const auto sink = builder.MakeNode("memcpy", cpg::NodeKind::kCallSite);
  input.flow_slice.nodes = {source, sink};
  cpg::CpgEdge flow_edge =
      builder.MakeEdge("flow", cpg::EdgeKind::kFlowsTo, source.node_id,
                       sink.node_id);
  flow_edge.support = {
      cpg::SupportRef{builder.Id(core::IdKind::kFunctionSummary, "summary"),
                      "provenance-ref"}};
  input.flow_slice.edges = {flow_edge};
  input.flow_slice.supporting_facts = {builder.MakeRangeFact("range", 0, 65535)};
  input.flow_slice.unknowns = {builder.MakeUnknownFact("opaque", "unmodeled")};
  input.flow_slice.provenance_refs = {
      builder.Id(core::IdKind::kFact, "completion")};
  input.flow_slice.metadata =
      builder.MakeMetadata("run", "completion", QueryCompleteness::kComplete, {},
                           2);

  input.ranges =
      MakeFactSet(builder, "run", "completion", QueryCompleteness::kComplete, {},
                  1, {builder.MakeRangeFact("range", 0, 65535)});
  input.capacities =
      MakeFactSet(builder, "run", "completion", QueryCompleteness::kComplete, {},
                  1, {builder.MakeCapacityFact("buffer", 2048)});
  input.aliases =
      MakeFactSet(builder, "run", "completion", QueryCompleteness::kComplete, {},
                  1,
                  {builder.MakeAliasFact("alias", AliasKind::kMayAlias,
                                         EpistemicState::kMay)});
  input.dominating_checks =
      MakeFactSet(builder, "run", "completion", QueryCompleteness::kComplete, {},
                  1, {builder.MakeCheckFact("check")});
  input.unknowns =
      MakeFactSet(builder, "run", "completion", QueryCompleteness::kComplete, {},
                  1, {builder.MakeUnknownFact("opaque", "unmodeled")});

  QueryCompletionDescriptor descriptor;
  descriptor.query_kind = "value_flow";
  descriptor.ordered_scope_refs = {input.claim_seed.source_ref,
                                   input.claim_seed.sink_ref};
  descriptor.budget = Budget(8, 256, 5, 64, 8);
  descriptor.query_implementation_version = "v1";
  descriptor.input_snapshot_fingerprint = "snapshot-fingerprint";
  descriptor.completeness = QueryCompleteness::kComplete;
  descriptor.examined_items = 1;
  descriptor.returned_member_digest = "member-digest";

  auto completion = builder.MakeCompletionFact(descriptor);
  const auto run_id = builder.Id(core::IdKind::kAnalysisRun, "run");
  input.query_completion_facts = {*completion};
  input.query_completion_bindings = {
      builder.MakeBinding(run_id, completion->fact_id, "w1")};

  auto& provenance = input.provenance;
  provenance.set_run_id(core::ToString(run_id));
  provenance.set_fact_id(core::ToString(completion->fact_id));
  provenance.mutable_fact()->set_fact_id(core::ToString(completion->fact_id));
  provenance.mutable_fact()->set_relation_name("evidence.query_completion.v1");
  provenance.mutable_fact()->set_relation_schema_version("relations.v2");
  provenance.mutable_binding()->set_analysis_run_id(core::ToString(run_id));
  provenance.mutable_binding()->set_fact_id(core::ToString(completion->fact_id));
  auto* witness = provenance.add_nodes();
  witness->set_analysis_run_id(core::ToString(run_id));
  witness->set_output_fact_id(core::ToString(completion->fact_id));
  witness->set_witness_id("w1");
  witness->set_selected(true);
  witness->set_rule_id("evidence.query_completion.v1");

  return input;
}

}  // namespace

// AC-001: two empty fact sets that differ only in metadata are unequal in both
// their typed form and their diagnostic JSON.
TEST(EvidenceContractTest, CompleteEmptyDiffersFromTruncatedEmpty) {
  const auto complete = EmptyFactSet(QueryCompleteness::kComplete, {});
  const auto truncated =
      EmptyFactSet(QueryCompleteness::kTruncated,
                   {TruncationReason::kMaxFacts});
  EXPECT_NE(complete.metadata, truncated.metadata);
  EXPECT_NE(ToDiagnosticJson(complete), ToDiagnosticJson(truncated));
  EXPECT_TRUE(ValidateQueryResultMetadata(complete.metadata).ok());
  EXPECT_TRUE(ValidateQueryResultMetadata(truncated.metadata).ok());
}

// AC-002: every invalid public metadata shape is rejected.
class InvalidQueryResultMetadataTest
    : public ::testing::TestWithParam<QueryResultMetadata> {};

TEST_P(InvalidQueryResultMetadataTest, RejectsInvalidResultMetadata) {
  EXPECT_FALSE(ValidateQueryResultMetadata(GetParam()).ok());
}

INSTANTIATE_TEST_SUITE_P(
    InvalidMetadata, InvalidQueryResultMetadataTest,
    ::testing::ValuesIn(InvalidMetadataCases()),
    [](const ::testing::TestParamInfo<QueryResultMetadata>& info) {
      return InvalidMetadataCaseName(static_cast<std::size_t>(info.index));
    });

// AC-003: exactly max_facts_per_query matching facts is complete, not truncated.
TEST(EvidenceContractTest, ExactFactBudgetBoundaryIsComplete) {
  EvidenceScenarioBuilder builder;
  const EvidenceQueryBudget budget = Budget(8, 256, 5, 3, 8);
  std::vector<AnalysisFact> candidates = {
      builder.MakeAliasFact("a", AliasKind::kMustAlias, EpistemicState::kMust),
      builder.MakeAliasFact("b", AliasKind::kMayAlias, EpistemicState::kMay),
      builder.MakeAliasFact("c", AliasKind::kNoAlias, EpistemicState::kMust),
  };
  auto result = ApplyFactBudget(std::move(candidates), budget,
                                QueryResultMetadata{});
  ASSERT_EQ(result.facts.size(), 3u);
  EXPECT_EQ(result.metadata.completeness, QueryCompleteness::kComplete);
  EXPECT_TRUE(result.metadata.truncation_reasons.empty());
  EXPECT_EQ(result.metadata.examined_items, 3u);
}

// AC-004: one row over the limit returns the canonical prefix with kMaxFacts,
// and the examined count proves the overflow.
TEST(EvidenceContractTest, FactBudgetOverflowReturnsCanonicalPrefix) {
  EvidenceScenarioBuilder builder;
  const EvidenceQueryBudget budget = Budget(8, 256, 5, 2, 8);
  const auto fact_a =
      builder.MakeAliasFact("a", AliasKind::kMustAlias, EpistemicState::kMust);
  const auto fact_m =
      builder.MakeAliasFact("m", AliasKind::kMayAlias, EpistemicState::kMay);
  const auto fact_z =
      builder.MakeAliasFact("z", AliasKind::kNoAlias, EpistemicState::kMust);

  // Canonical order is by fact ID, not by the symbolic names used here.
  std::vector<AnalysisFact> canonical = {fact_a, fact_m, fact_z};
  std::sort(canonical.begin(), canonical.end(),
            [](const AnalysisFact& x, const AnalysisFact& y) {
              return x.fact_id < y.fact_id;
            });

  // Deliberately unsorted input: the result must be the canonical prefix.
  std::vector<AnalysisFact> candidates = {fact_z, fact_a, fact_m};
  auto result = ApplyFactBudget(std::move(candidates), budget,
                                QueryResultMetadata{});

  ASSERT_EQ(result.facts.size(), 2u);
  EXPECT_EQ(result.facts[0].fact_id, canonical[0].fact_id);
  EXPECT_EQ(result.facts[1].fact_id, canonical[1].fact_id);
  EXPECT_NE(result.facts[0].fact_id, canonical[2].fact_id);
  EXPECT_NE(result.facts[1].fact_id, canonical[2].fact_id);
  EXPECT_EQ(result.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(result.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(result.metadata.truncation_reasons[0], TruncationReason::kMaxFacts);
  EXPECT_EQ(result.metadata.examined_items, 3u);
}

// AC-004 (discriminating case): a candidate set larger than limit + 1 must
// record every assessed candidate, not the limit + 1 "probe" count that happens
// to coincide with `matches.size()` when the set is exactly one over the limit.
// Both budget paths — ApplyFactBudget and RunFactQuery — must agree on the
// honest reading of examined_items: "candidates assessed by the query".
TEST(EvidenceContractTest, FactBudgetCountsEveryAssessedCandidate) {
  EvidenceScenarioBuilder builder;
  const EvidenceQueryBudget budget = Budget(8, 256, 5, 2, 8);
  const std::vector<std::string> names = {"a", "b", "c", "d", "e"};
  const std::vector<AliasKind> kinds = {
      AliasKind::kMustAlias, AliasKind::kMayAlias, AliasKind::kNoAlias,
      AliasKind::kMustAlias, AliasKind::kMayAlias};

  std::vector<AnalysisFact> candidates;
  for (std::size_t i = 0; i < names.size(); ++i) {
    candidates.push_back(builder.MakeAliasFact(names[i], kinds[i],
                                               EpistemicState::kMust));
  }
  std::vector<AnalysisFact> canonical = candidates;
  std::sort(canonical.begin(), canonical.end(),
            [](const AnalysisFact& x, const AnalysisFact& y) {
              return x.fact_id < y.fact_id;
            });

  auto result = ApplyFactBudget(std::move(candidates), budget,
                                QueryResultMetadata{});

  ASSERT_EQ(result.facts.size(), 2u);
  EXPECT_EQ(result.facts[0].fact_id, canonical[0].fact_id);
  EXPECT_EQ(result.facts[1].fact_id, canonical[1].fact_id);
  EXPECT_EQ(result.metadata.completeness, QueryCompleteness::kTruncated);
  ASSERT_EQ(result.metadata.truncation_reasons.size(), 1u);
  EXPECT_EQ(result.metadata.truncation_reasons[0], TruncationReason::kMaxFacts);
  // Five candidates were assessed even though two were returned: limit + 1
  // would have claimed three (and passed under the AC-004 shape).
  EXPECT_EQ(result.metadata.examined_items, 5u);
  EXPECT_GT(result.metadata.examined_items, budget.max_facts_per_query + 1);
}

// AC-005: duplicate truncation reasons are rejected, and valid inputs differing
// only in insertion order serialize identically.
TEST(EvidenceContractTest, MetadataOrderingIsCanonical) {
  EvidenceScenarioBuilder builder;

  const QueryResultMetadata duplicate = builder.MakeMetadata(
      "run", "provenance", QueryCompleteness::kTruncated,
      {TruncationReason::kMaxFacts, TruncationReason::kMaxFacts}, 2);
  EXPECT_FALSE(ValidateQueryResultMetadata(duplicate).ok());

  const auto fact_a =
      builder.MakeAliasFact("a", AliasKind::kMustAlias, EpistemicState::kMust);
  const auto fact_b =
      builder.MakeAliasFact("b", AliasKind::kMayAlias, EpistemicState::kMay);

  EvidenceFactSet first;
  first.facts = {fact_b, fact_a};
  first.metadata = builder.MakeMetadata(
      "run", "provenance", QueryCompleteness::kTruncated,
      {TruncationReason::kMaxFacts, TruncationReason::kMaxDepth}, 2);

  EvidenceFactSet second;
  second.facts = {fact_a, fact_b};
  second.metadata = builder.MakeMetadata(
      "run", "provenance", QueryCompleteness::kTruncated,
      {TruncationReason::kMaxDepth, TruncationReason::kMaxFacts}, 2);

  EXPECT_EQ(ToDiagnosticJson(first), ToDiagnosticJson(second));
}

// AC-006: a fully populated EvidenceBuildInput serializes to byte-identical,
// complete JSON with one final newline, independent of member ordering.
TEST(EvidenceContractTest, EvidenceBuildInputJsonIsDeterministicAndComplete) {
  EvidenceBuildInput input = BuildInput();

  const std::string first = ToDiagnosticJson(input);
  const std::string second = ToDiagnosticJson(input);
  EXPECT_EQ(first, second);

  EvidenceScenarioBuilder::Reverse(input.query_completion_facts);
  EvidenceScenarioBuilder::Reverse(input.query_completion_bindings);
  EvidenceScenarioBuilder::Reverse(input.ranges.facts);
  EvidenceScenarioBuilder::Reverse(input.aliases.facts);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.edges);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.nodes);
  EXPECT_EQ(first, ToDiagnosticJson(input));

  for (const char* field : {"\"claim_seed\"",
                            "\"flow_slice\"",
                            "\"ranges\"",
                            "\"capacities\"",
                            "\"aliases\"",
                            "\"dominating_checks\"",
                            "\"unknowns\"",
                            "\"query_completion_facts\"",
                            "\"query_completion_bindings\"",
                            "\"provenance\"",
                            "\"completeness\"",
                            "\"truncation_reasons\"",
                            "\"examined_items\"",
                            "\"analysis_run_id\"",
                            "\"query_provenance_id\""}) {
    EXPECT_NE(first.find(field), std::string::npos) << field;
  }

  EXPECT_EQ(first.back(), '\n');
  EXPECT_EQ(first.find('\n'), first.size() - 1);
  EXPECT_EQ(first.rfind('\n'), first.size() - 1);
}
