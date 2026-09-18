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

// EvidencePredicateMapperTest.cpp — the M9/M10A relation to EIR predicate
// lowering contract.
//
// The mapper is a lowering, not an analysis, and the properties these tests pin
// are the ones that make that true:
//
//   * it copies the epistemic state instead of strengthening it;
//   * it resolves *every* entity reference a relation carries, not just the
//     first one;
//   * it refuses a relation it has no mapping for rather than dropping the fact;
//   * every producer and predicate spelling it emits is a legal EIR-T
//     `QualifiedId`, so a mapped fact is always writable.

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidencePredicateMapper.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::evidence {
namespace {

namespace sem = analysis::semantic;

using testing::EvidenceScenarioBuilder;

constexpr std::string_view kLeftHandle = "E_left_buffer";
constexpr std::string_view kRightHandle = "E_right_buffer";
constexpr std::string_view kFunctionHandle = "E_decode";
constexpr std::string_view kMemoryHandle = "E_scratch";

// The resolver the builder owns, populated by hand so a test can assert exactly
// which stable ID each operand was resolved from.
LocalIdTable MakeTable(const core::StableId& left, const core::StableId& right) {
  LocalIdTable table;
  table.Bind(left, std::string(kLeftHandle));
  table.Bind(right, std::string(kRightHandle));
  return table;
}

facts::AnalysisFact MakeRow(facts::RelationId relation,
                            std::vector<facts::SemanticCellValue> cells) {
  facts::SemanticRow row;
  row.relation = relation;
  row.cells = std::move(cells);
  auto fact = facts::MakeFact(row);
  EXPECT_TRUE(fact.ok());
  return fact.ok() ? std::move(fact).value() : facts::AnalysisFact{};
}

FactMappingHandle Handle(std::string local_id) {
  FactMappingHandle handle;
  handle.local_id = std::move(local_id);
  handle.provenance_id = "PR_test";
  return handle;
}

// --- Multi-reference resolution ---------------------------------------------

// `alias(@left, @right)` names two entities. A mapper that took a single
// local-ID string would have to drop one of them; this one resolves both to the
// handle each stable ID was bound to.
TEST(EvidencePredicateMapperTest, AliasResolvesBothReferences) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId left =
      scenario.Id(core::IdKind::kMemoryRef, "left_buffer");
  const core::StableId right =
      scenario.Id(core::IdKind::kMemoryRef, "right_buffer");
  const facts::AnalysisFact fact = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kMay);

  const LocalIdTable table = MakeTable(left, right);
  auto mapped = EvidencePredicateMapper().MapFact(fact, table, Handle("F_alias"));

  ASSERT_TRUE(mapped.ok()) << mapped.status().message();
  const Fact& value = mapped.value();
  EXPECT_EQ(value.predicate.kind, Expression::Kind::kCall);
  EXPECT_EQ(value.predicate.text, "alias");
  ASSERT_EQ(value.predicate.operands.size(), 2u);
  EXPECT_EQ(value.predicate.operands[0].kind, Expression::Kind::kReference);
  EXPECT_EQ(value.predicate.operands[0].text, kLeftHandle);
  EXPECT_EQ(value.predicate.operands[1].kind, Expression::Kind::kReference);
  EXPECT_EQ(value.predicate.operands[1].text, kRightHandle);
}

// `reads(@function, @memory)` and `writes(@function, @memory)` likewise name
// two entities, and the direction is the registry's rather than a heuristic.
TEST(EvidencePredicateMapperTest, MemoryEffectResolvesBothReferences) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId function =
      scenario.Id(core::IdKind::kFunctionVariant, "decode");
  const core::StableId memory = scenario.Id(core::IdKind::kMemoryRef, "scratch");

  LocalIdTable table;
  table.Bind(function, std::string(kFunctionHandle));
  table.Bind(memory, std::string(kMemoryHandle));

  const std::pair<facts::RelationId, std::string_view> directions[] = {
      {facts::RelationId::kMayRead, "reads"},
      {facts::RelationId::kMayWrite, "writes"},
  };
  for (const auto& [relation, predicate] : directions) {
    const facts::AnalysisFact fact = MakeRow(
        relation, {function, memory, sem::EpistemicState::kMay});
    auto mapped =
        EvidencePredicateMapper().MapFact(fact, table, Handle("F_effect"));

    ASSERT_TRUE(mapped.ok()) << mapped.status().message();
    const Fact& value = mapped.value();
    EXPECT_EQ(value.predicate.text, predicate);
    ASSERT_EQ(value.predicate.operands.size(), 2u);
    EXPECT_EQ(value.predicate.operands[0].text, kFunctionHandle);
    EXPECT_EQ(value.predicate.operands[1].text, kMemoryHandle);
  }
}

// A reference the case declares no entity for is a typed failure, never a
// silently dropped operand.
TEST(EvidencePredicateMapperTest, UnresolvedReferenceIsRefused) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId left =
      scenario.Id(core::IdKind::kMemoryRef, "left_buffer");
  const core::StableId right =
      scenario.Id(core::IdKind::kMemoryRef, "right_buffer");
  const facts::AnalysisFact fact = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kMay);

  // The table binds only the left reference.
  LocalIdTable table;
  table.Bind(left, std::string(kLeftHandle));
  auto mapped = EvidencePredicateMapper().MapFact(fact, table, Handle("F_alias"));

  EXPECT_FALSE(mapped.ok());
  EXPECT_EQ(mapped.status().code(), StatusCode::kInvalidArgument);
}

// --- Epistemic fidelity ------------------------------------------------------

// The mapper's authority stops at transcription: an M9 fact the analysis
// inferred stays inferred, and is never promoted to a MUST by assembly.
TEST(EvidencePredicateMapperTest, CopiesEpistemicStateWithoutStrengthening) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId left =
      scenario.Id(core::IdKind::kMemoryRef, "left_buffer");
  const core::StableId right =
      scenario.Id(core::IdKind::kMemoryRef, "right_buffer");
  const LocalIdTable table = MakeTable(left, right);

  const std::pair<sem::EpistemicState, EpistemicState> expected[] = {
      {sem::EpistemicState::kMust, EpistemicState::kMust},
      {sem::EpistemicState::kMay, EpistemicState::kMay},
      {sem::EpistemicState::kInferred, EpistemicState::kInferred},
      {sem::EpistemicState::kAssumed, EpistemicState::kAssumed},
      {sem::EpistemicState::kUnknown, EpistemicState::kUnknown},
      {sem::EpistemicState::kMustNot, EpistemicState::kMustNot},
  };
  for (const auto& [input, output] : expected) {
    const facts::AnalysisFact fact = scenario.MakeAliasFact(
        left, right, sem::AliasKind::kMayAlias, input);
    auto mapped =
        EvidencePredicateMapper().MapFact(fact, table, Handle("F_alias"));

    ASSERT_TRUE(mapped.ok()) << mapped.status().message();
    EXPECT_EQ(mapped.value().epistemic, output)
        << "an input state must lower one-for-one";
  }

  // The strengthening that matters: an inference the analysis made is never
  // promoted to a fact this milestone asserts. Promotion is the verifier's
  // authority, not the assembler's (CLAUDE.md P8).
  const facts::AnalysisFact inferred = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kInferred);
  auto mapped_inferred =
      EvidencePredicateMapper().MapFact(inferred, table, Handle("F_inferred"));
  ASSERT_TRUE(mapped_inferred.ok()) << mapped_inferred.status().message();
  EXPECT_EQ(mapped_inferred.value().epistemic, EpistemicState::kInferred);
  EXPECT_NE(mapped_inferred.value().epistemic, EpistemicState::kMust);
  EXPECT_EQ(mapped_inferred.value().confidence, Confidence::kLow);
  EXPECT_TRUE(mapped_inferred.value().derived);
}

TEST(EvidencePredicateMapperTest, ConfidenceNeverExceedsTheStateItLowers) {
  EXPECT_EQ(ConfidenceForState(EpistemicState::kInferred), Confidence::kLow);
  EXPECT_EQ(ConfidenceForState(EpistemicState::kMust), Confidence::kExact);
  EXPECT_EQ(ConfidenceForState(EpistemicState::kMustNot), Confidence::kExact);
  EXPECT_EQ(ConfidenceForState(EpistemicState::kMay), Confidence::kMedium);
  EXPECT_EQ(ConfidenceForState(EpistemicState::kUnknown), Confidence::kUnknown);
  EXPECT_EQ(ConfidenceForState(EpistemicState::kUnspecified),
            Confidence::kUnknown);
}

// A fact the analysis stated as MUST is an observation, so it owes no
// derivation; anything weaker is derived and must carry provenance.
TEST(EvidencePredicateMapperTest, DerivationTracksTheInputState) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId left =
      scenario.Id(core::IdKind::kMemoryRef, "left_buffer");
  const core::StableId right =
      scenario.Id(core::IdKind::kMemoryRef, "right_buffer");
  const LocalIdTable table = MakeTable(left, right);

  const facts::AnalysisFact observed = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kMust);
  auto observed_fact =
      EvidencePredicateMapper().MapFact(observed, table, Handle("F_observed"));
  ASSERT_TRUE(observed_fact.ok());
  EXPECT_FALSE(observed_fact.value().derived);

  const facts::AnalysisFact inferred = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kInferred);
  auto inferred_fact =
      EvidencePredicateMapper().MapFact(inferred, table, Handle("F_inferred"));
  ASSERT_TRUE(inferred_fact.ok());
  EXPECT_TRUE(inferred_fact.value().derived);
}

// --- Refusal -----------------------------------------------------------------

// The registry is the authority on which relations exist; the mapper's table is
// the authority on which ones EIR can state. A relation outside the table is a
// typed failure, because a silently omitted fact would leave the case claiming
// a completeness it does not have.
TEST(EvidencePredicateMapperTest, UnsupportedRelationIsRefused) {
  const EvidenceScenarioBuilder scenario;
  const facts::AnalysisFact fact = scenario.MakeCheckFact("check:memcpy");

  LocalIdTable table;
  auto mapped = EvidencePredicateMapper().MapFact(fact, table, Handle("F_check"));

  EXPECT_FALSE(mapped.ok());
  EXPECT_EQ(mapped.status().code(), StatusCode::kInvalidArgument);
  EXPECT_NE(mapped.status().message().find("no built-in EIR predicate mapping"),
            std::string_view::npos);
}

// A row whose arity disagrees with its relation never reaches the mapping: the
// registry's own validator rejects it first, so no malformed fact can be
// lowered into a well-formed-looking predicate.
TEST(EvidencePredicateMapperTest, MalformedRowIsRefusedByTheRegistry) {
  const EvidenceScenarioBuilder scenario;
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {scenario.Id(core::IdKind::kMemoryRef, "only_one")};
  auto fact = facts::MakeFact(row);
  EXPECT_FALSE(fact.ok());
  EXPECT_EQ(fact.status().code(), StatusCode::kInvalidArgument);
}

// --- Spelling ----------------------------------------------------------------

bool IsQualifiedId(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  bool at_segment_start = true;
  for (char c : text) {
    if (c == '.') {
      at_segment_start = true;
      continue;
    }
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    const bool underscore = c == '_';
    if (at_segment_start) {
      if (!letter) {
        return false;
      }
      at_segment_start = false;
      continue;
    }
    if (!(letter || digit || underscore)) {
      return false;
    }
  }
  return !at_segment_start;
}

// Every producer and predicate the mapper emits is a legal EIR-T `QualifiedId`,
// which is what makes a mapped fact writable at all. `Producer` was
// deliberately not widened for this milestone (ruling L43), so the mapper is the
// layer that has to spell things EIR can carry.
TEST(EvidencePredicateMapperTest, EveryEmittedSpellingIsAQualifiedId) {
  const facts::RelationId supported[] = {
      facts::RelationId::kDirectRead,
      facts::RelationId::kDirectWrite,
      facts::RelationId::kReachableCall,
      facts::RelationId::kAlias,
      facts::RelationId::kMayRead,
      facts::RelationId::kMayWrite,
      facts::RelationId::kGlobalFlow,
  };
  for (facts::RelationId relation : supported) {
    const auto predicate = EvidencePredicateMapper::PredicateName(relation);
    const auto producer = EvidencePredicateMapper::ProducerName(relation);
    ASSERT_TRUE(predicate.has_value());
    ASSERT_TRUE(producer.has_value());
    EXPECT_TRUE(IsQualifiedId(*predicate)) << std::string(*predicate);
    EXPECT_TRUE(IsQualifiedId(*producer)) << std::string(*producer);
  }

  // The relations EIR cannot state have no spelling at all, rather than a
  // lossy one.
  EXPECT_FALSE(
      EvidencePredicateMapper::PredicateName(facts::RelationId::kQueryCompletion)
          .has_value());
  EXPECT_FALSE(
      EvidencePredicateMapper::ProducerName(facts::RelationId::kQueryCompletion)
          .has_value());
}

// The mapper keeps the M9 fact's content-addressed identity, so a case can name
// the cross-run fact it was lowered from.
TEST(EvidencePredicateMapperTest, KeepsTheStableIdentityOfTheSourceFact) {
  const EvidenceScenarioBuilder scenario;
  const core::StableId left =
      scenario.Id(core::IdKind::kMemoryRef, "left_buffer");
  const core::StableId right =
      scenario.Id(core::IdKind::kMemoryRef, "right_buffer");
  const facts::AnalysisFact fact = scenario.MakeAliasFact(
      left, right, sem::AliasKind::kMayAlias, sem::EpistemicState::kMay);

  const LocalIdTable table = MakeTable(left, right);
  auto mapped = EvidencePredicateMapper().MapFact(fact, table, Handle("F_alias"));

  ASSERT_TRUE(mapped.ok());
  ASSERT_TRUE(mapped.value().stable_id.has_value());
  EXPECT_EQ(*mapped.value().stable_id, fact.fact_id);
  EXPECT_EQ(mapped.value().id, "F_alias");
  EXPECT_EQ(mapped.value().provenance_id, "PR_test");
}

}  // namespace
}  // namespace veritas::evidence
