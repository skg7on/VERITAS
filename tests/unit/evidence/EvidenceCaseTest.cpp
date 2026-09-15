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

// EvidenceCaseTest.cpp — the `eir.v1` semantic model contract: enum spellings
// that must agree with the stabilized EIR-T 1.0 grammar, the typed expression
// AST, and the invariants the shared scenario factories must satisfy so every
// later M10C unit test starts from a fact-bearing case.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/SliceTypes.h"

using namespace veritas;
using namespace veritas::evidence;
using namespace veritas::testing;

namespace {

// Pins one enum family against the exact EIR-T 1.0 spellings: every spelling
// renders to its enumerator and parses back to the same value.
template <typename Enum, typename ToText, typename FromText, std::size_t N>
void ExpectRoundTrips(
    const std::array<std::pair<Enum, std::string_view>, N>& pairs,
    ToText to_text, FromText from_text) {
  for (const auto& [value, spelling] : pairs) {
    EXPECT_EQ(to_text(value), spelling);
    auto parsed = from_text(spelling);
    ASSERT_TRUE(parsed.ok())
        << spelling << ": " << parsed.status().message();
    EXPECT_EQ(*parsed, value) << spelling;
  }
}

// Every reference a case can carry, resolved against the members that declare
// it. Returns the sorted list of unresolvable references.
std::vector<std::string> UnresolvedReferences(const EvidenceCase& value) {
  std::set<std::string> entities;
  std::set<std::string> facts;
  std::set<std::string> provenance;
  std::set<std::string> summaries;
  std::set<std::string> members;
  for (const auto& entity : value.entities) {
    entities.insert(entity.id);
    members.insert(entity.id);
  }
  for (const auto& fact : value.facts) {
    facts.insert(fact.id);
    members.insert(fact.id);
  }
  for (const auto& record : value.provenance) {
    provenance.insert(record.id);
    members.insert(record.id);
  }
  for (const auto& summary : value.summaries) {
    summaries.insert(summary.id);
    members.insert(summary.id);
  }
  for (const auto& member : value.edges) {
    members.insert(member.id);
  }
  for (const auto& member : value.paths) {
    members.insert(member.id);
  }
  for (const auto& member : value.assumptions) {
    members.insert(member.id);
  }
  for (const auto& member : value.hypotheses) {
    members.insert(member.id);
  }
  for (const auto& member : value.unknowns) {
    members.insert(member.id);
  }
  for (const auto& member : value.constraints) {
    members.insert(member.id);
  }
  for (const auto& member : value.proof_obligations) {
    members.insert(member.id);
  }
  for (const auto& member : value.dependencies) {
    members.insert(member.id);
  }
  for (const auto& member : value.omissions) {
    members.insert(member.id);
  }

  std::vector<std::string> unresolved;
  auto require = [&unresolved](const std::string& reference,
                               const std::set<std::string>& scope,
                               const char* what) {
    if (scope.count(reference) == 0) {
      unresolved.push_back(std::string(what) + ": " + reference);
    }
  };

  if (entities.count(value.primary_claim.subject) == 0) {
    unresolved.push_back("claim.subject: " + value.primary_claim.subject);
  }
  for (const auto& edge : value.edges) {
    require(edge.from, entities, "edge.from");
    require(edge.to, entities, "edge.to");
    if (!edge.provenance_id.empty()) {
      require(edge.provenance_id, provenance, "edge.provenance_id");
    }
    if (!edge.summarized_by.empty()) {
      require(edge.summarized_by, summaries, "edge.summarized_by");
    }
  }
  for (const auto& path : value.paths) {
    for (const auto& entity_id : path.entity_ids) {
      require(entity_id, entities, "path.entity_ids");
    }
    if (!path.provenance_id.empty()) {
      require(path.provenance_id, provenance, "path.provenance_id");
    }
  }
  for (const auto& fact : value.facts) {
    if (!fact.provenance_id.empty()) {
      require(fact.provenance_id, provenance, "fact.provenance_id");
    }
  }
  for (const auto& unknown : value.unknowns) {
    for (const auto& blocking : unknown.blocking_ids) {
      require(blocking, facts, "unknown.blocking_ids");
    }
  }
  for (const auto& constraint : value.constraints) {
    if (!constraint.provenance_id.empty()) {
      require(constraint.provenance_id, provenance, "constraint.provenance_id");
    }
  }
  for (const auto& record : value.provenance) {
    for (const auto& input : record.input_fact_ids) {
      require(input, facts, "provenance.input_fact_ids");
    }
  }
  for (const auto& summary : value.summaries) {
    require(summary.function_id, entities, "summary.function_id");
  }
  for (const auto& omission : value.omissions) {
    require(omission.subject, members, "omission.subject");
  }
  std::sort(unresolved.begin(), unresolved.end());
  return unresolved;
}

// The expression `value(@x) > capacity(@y)`.
bool IsOverflowComparison(const Expression& expression) {
  if (expression.kind != Expression::Kind::kCompare ||
      expression.text != ">" || expression.operands.size() != 2) {
    return false;
  }
  const Expression& left = expression.operands[0];
  const Expression& right = expression.operands[1];
  return left.kind == Expression::Kind::kCall && left.text == "value" &&
         left.operands.size() == 1 &&
         left.operands[0].kind == Expression::Kind::kReference &&
         right.kind == Expression::Kind::kCall && right.text == "capacity" &&
         right.operands.size() == 1 &&
         right.operands[0].kind == Expression::Kind::kReference;
}

}  // namespace

TEST(EvidenceCaseTest, RepresentsInitialOverflowCaseState) {
  EvidenceCase value;
  value.schema_version = "eir.v1";
  value.level = EvidenceLevel::kL1;
  value.verification_state = VerificationState::kPossibleDefect;
  value.primary_claim.kind = ClaimKind::kBufferOverflow;
  value.primary_claim.severity = Severity::kHigh;
  EXPECT_EQ(value.schema_version, "eir.v1");
  EXPECT_EQ(value.proof_obligations.size(), 0U);
}

// Every enum family starts at the invalid default, so a partly built case is
// distinguishable from one that deliberately carries a grammar value.
TEST(EvidenceCaseTest, EnumFamiliesDefaultToUnspecified) {
  EXPECT_EQ(static_cast<int>(EvidenceLevel::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(VerificationState::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(EntityKind::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(RelationKind::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(PathKind::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(EpistemicState::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(Confidence::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(ProofStatus::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(DependencyKind::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(Feasibility::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(ProofGoalKind::kUnspecified), 0);
  EXPECT_EQ(static_cast<int>(Expression::Kind::kUnspecified), 0);

  const EvidenceCase value;
  EXPECT_EQ(value.schema_version, "");
  EXPECT_EQ(value.level, EvidenceLevel::kUnspecified);
  EXPECT_EQ(value.verification_state, VerificationState::kUnspecified);
  EXPECT_EQ(value.primary_claim.kind, ClaimKind::kUnspecified);
  EXPECT_FALSE(value.evidence_id.has_value());
}

// EIR-T 1.0 §3: EvidenceState is the closed UPPERCASE set.
TEST(EvidenceCaseTest, VerificationStateSpellingsMatchStabilizedGrammar) {
  const std::array<std::pair<VerificationState, std::string_view>, 7> pairs{{
      {VerificationState::kUnreviewed, "UNREVIEWED"},
      {VerificationState::kPossibleDefect, "POSSIBLE_DEFECT"},
      {VerificationState::kLikelyDefect, "LIKELY_DEFECT"},
      {VerificationState::kVerifiedDefect, "VERIFIED_DEFECT"},
      {VerificationState::kLikelyFalsePositive, "LIKELY_FALSE_POSITIVE"},
      {VerificationState::kVerifiedSafe, "VERIFIED_SAFE"},
      {VerificationState::kInconclusive, "INCONCLUSIVE"},
  }};
  ExpectRoundTrips<VerificationState>(
      pairs, [](VerificationState v) { return ToString(v); },
      [](std::string_view text) { return ParseVerificationState(text); });
}

// EIR-T 1.0 §3 and §3.1: levels and dependency kinds are lowercase.
TEST(EvidenceCaseTest, LevelAndDependencyKindSpellingsAreLowercase) {
  const std::array<std::pair<EvidenceLevel, std::string_view>, 3> levels{{
      {EvidenceLevel::kL0, "l0"},
      {EvidenceLevel::kL1, "l1"},
      {EvidenceLevel::kL2, "l2"},
  }};
  ExpectRoundTrips<EvidenceLevel>(
      levels, [](EvidenceLevel v) { return ToString(v); },
      [](std::string_view text) { return ParseEvidenceLevel(text); });

  const std::array<std::pair<DependencyKind, std::string_view>, 5> kinds{{
      {DependencyKind::kSummary, "summary"},
      {DependencyKind::kFact, "fact"},
      {DependencyKind::kTypeLayout, "type_layout"},
      {DependencyKind::kConfiguration, "configuration"},
      {DependencyKind::kSpecification, "specification"},
  }};
  ExpectRoundTrips<DependencyKind>(
      kinds, [](DependencyKind v) { return ToString(v); },
      [](std::string_view text) { return ParseDependencyKind(text); });
}

// EIR-T 1.0 §4.1/§5.1/§6.1/§8.1/§9/§10.1/§12: entity, relation, path, epistemic,
// confidence, proof-status, feasibility, and proof-goal spellings.
TEST(EvidenceCaseTest, SemanticEnumSpellingsMatchStabilizedGrammar) {
  const std::array<std::pair<EntityKind, std::string_view>, 5> entities{{
      {EntityKind::kFunction, "function"},
      {EntityKind::kCallSite, "callsite"},
      {EntityKind::kValue, "value"},
      {EntityKind::kMemoryObject, "memory_object"},
      {EntityKind::kBasicBlock, "basic_block"},
  }};
  ExpectRoundTrips<EntityKind>(
      entities, [](EntityKind v) { return ToString(v); },
      [](std::string_view text) { return ParseEntityKind(text); });

  const std::array<std::pair<RelationKind, std::string_view>, 6> relations{{
      {RelationKind::kCalls, "CALLS"},
      {RelationKind::kFlowsTo, "FLOWS_TO"},
      {RelationKind::kReads, "READS"},
      {RelationKind::kWrites, "WRITES"},
      {RelationKind::kDominates, "DOMINATES"},
      {RelationKind::kMayAlias, "MAY_ALIAS"},
  }};
  ExpectRoundTrips<RelationKind>(
      relations, [](RelationKind v) { return ToString(v); },
      [](std::string_view text) { return ParseRelationKind(text); });

  const std::array<std::pair<PathKind, std::string_view>, 3> paths{{
      {PathKind::kCall, "call"},
      {PathKind::kValueFlow, "value_flow"},
      {PathKind::kControl, "control"},
  }};
  ExpectRoundTrips<PathKind>(
      paths, [](PathKind v) { return ToString(v); },
      [](std::string_view text) { return ParsePathKind(text); });

  const std::array<std::pair<EpistemicState, std::string_view>, 6> epistemic{{
      {EpistemicState::kMust, "must"},
      {EpistemicState::kMay, "may"},
      {EpistemicState::kMustNot, "must_not"},
      {EpistemicState::kInferred, "inferred"},
      {EpistemicState::kAssumed, "assumed"},
      {EpistemicState::kUnknown, "unknown"},
  }};
  ExpectRoundTrips<EpistemicState>(
      epistemic, [](EpistemicState v) { return ToString(v); },
      [](std::string_view text) { return ParseEpistemicState(text); });

  const std::array<std::pair<Confidence, std::string_view>, 5> confidence{{
      {Confidence::kExact, "exact"},
      {Confidence::kHigh, "high"},
      {Confidence::kMedium, "medium"},
      {Confidence::kLow, "low"},
      {Confidence::kUnknown, "unknown"},
  }};
  ExpectRoundTrips<Confidence>(
      confidence, [](Confidence v) { return ToString(v); },
      [](std::string_view text) { return ParseConfidence(text); });

  const std::array<std::pair<ProofStatus, std::string_view>, 6> statuses{{
      {ProofStatus::kPending, "PENDING"},
      {ProofStatus::kProved, "PROVED"},
      {ProofStatus::kRefuted, "REFUTED"},
      {ProofStatus::kUnknown, "UNKNOWN"},
      {ProofStatus::kTimeout, "TIMEOUT"},
      {ProofStatus::kUnsupported, "UNSUPPORTED"},
  }};
  ExpectRoundTrips<ProofStatus>(
      statuses, [](ProofStatus v) { return ToString(v); },
      [](std::string_view text) { return ParseProofStatus(text); });

  const std::array<std::pair<Feasibility, std::string_view>, 7> feasibility{{
      {Feasibility::kProvedFeasible, "PROVED_FEASIBLE"},
      {Feasibility::kSat, "SAT"},
      {Feasibility::kMaybe, "MAYBE"},
      {Feasibility::kUntested, "UNTESTED"},
      {Feasibility::kUnsat, "UNSAT"},
      {Feasibility::kProvedInfeasible, "PROVED_INFEASIBLE"},
      {Feasibility::kUnknown, "UNKNOWN"},
  }};
  ExpectRoundTrips<Feasibility>(
      feasibility, [](Feasibility v) { return ToString(v); },
      [](std::string_view text) { return ParseFeasibility(text); });

  const std::array<std::pair<ProofGoalKind, std::string_view>, 3> goals{{
      {ProofGoalKind::kProve, "prove"},
      {ProofGoalKind::kRefute, "refute"},
      {ProofGoalKind::kCheck, "check"},
  }};
  ExpectRoundTrips<ProofGoalKind>(
      goals, [](ProofGoalKind v) { return ToString(v); },
      [](std::string_view text) { return ParseProofGoalKind(text); });
}

// EIR-T 1.0 §7.3: the closed `UnknownReason` set, in the grammar's terminal
// order.
TEST(EvidenceCaseTest, UnknownReasonCodeSpellingsMatchStabilizedGrammar) {
  const std::array<std::pair<UnknownReasonCode, std::string_view>, 10> codes{{
      {UnknownReasonCode::kUnresolvedCall, "UNRESOLVED_CALL"},
      {UnknownReasonCode::kUnknownAlias, "UNKNOWN_ALIAS"},
      {UnknownReasonCode::kExternalFunction, "EXTERNAL_FUNCTION"},
      {UnknownReasonCode::kMissingSpecification, "MISSING_SPECIFICATION"},
      {UnknownReasonCode::kAnalysisTimeout, "ANALYSIS_TIMEOUT"},
      {UnknownReasonCode::kStateExplosion, "STATE_EXPLOSION"},
      {UnknownReasonCode::kUnsupportedLanguageFeature,
       "UNSUPPORTED_LANGUAGE_FEATURE"},
      {UnknownReasonCode::kInlineAssembly, "INLINE_ASSEMBLY"},
      {UnknownReasonCode::kDynamicLoading, "DYNAMIC_LOADING"},
      {UnknownReasonCode::kUnknownBuildConfiguration,
       "UNKNOWN_BUILD_CONFIGURATION"},
  }};
  ExpectRoundTrips<UnknownReasonCode>(
      codes, [](UnknownReasonCode v) { return ToString(v); },
      [](std::string_view text) { return ParseUnknownReasonCode(text); });
}

// The invalid default is domain-only in every family: `ToString` stays total
// and renders it, and no parser accepts that rendering as a spelling. The
// grammar's own spellings are UPPERCASE, so a lowercased one is not a terminal
// either.
TEST(EvidenceCaseTest, UnknownReasonCodeSentinelIsNotASpelling) {
  EXPECT_EQ(ToString(UnknownReasonCode::kUnspecified), "unspecified");
  EXPECT_FALSE(ParseUnknownReasonCode("unspecified").ok());
  EXPECT_EQ(ToString(EvidenceLevel::kUnspecified), "unspecified");
  EXPECT_FALSE(ParseEvidenceLevel("unspecified").ok());
}

TEST(EvidenceCaseTest, UnknownReasonCodeRejectsOtherSpellings) {
  EXPECT_FALSE(ParseUnknownReasonCode("analysis_timeout").ok());
  EXPECT_FALSE(ParseUnknownReasonCode("").ok());
  EXPECT_FALSE(ParseUnknownReasonCode("UNRESOLVED").ok());
  EXPECT_FALSE(ParseUnknownReasonCode("UNKNOWN").ok());
  EXPECT_FALSE(ParseUnknownReasonCode("dominating-check query truncated").ok());
}

// The closed enumerations of the stabilized grammar reject anything else.
TEST(EvidenceCaseTest, EnumParsersRejectUnknownSpellings) {
  EXPECT_FALSE(ParseVerificationState("possible_defect").ok());
  EXPECT_FALSE(ParseVerificationState("").ok());
  EXPECT_FALSE(ParseEvidenceLevel("L1").ok());
  EXPECT_FALSE(ParseEntityKind("parameter").ok());
  EXPECT_FALSE(ParseRelationKind("calls").ok());
  EXPECT_FALSE(ParsePathKind("CFG").ok());
  EXPECT_FALSE(ParseEpistemicState("MUST").ok());
  EXPECT_FALSE(ParseConfidence("PROBABLE").ok());
  EXPECT_FALSE(ParseProofStatus("pending").ok());
  EXPECT_FALSE(ParseDependencyKind("summaries").ok());
  EXPECT_FALSE(ParseFeasibility("maybe_unsat").ok());
  EXPECT_FALSE(ParseProofGoalKind("VERIFY").ok());
}

// The expression model is a typed tree, not opaque text: operands carry the
// structure and `text` carries only the operator, callee, or literal payload.
TEST(EvidenceCaseTest, ExpressionCarriesTypedOperandTree) {
  Expression call;
  call.kind = Expression::Kind::kCall;
  call.text = "range";
  Expression bound;
  bound.kind = Expression::Kind::kReference;
  bound.text = "E_len";
  Expression low;
  low.kind = Expression::Kind::kInteger;
  low.integer = 0;
  Expression high;
  high.kind = Expression::Kind::kInteger;
  high.integer = 65535;
  call.operands = {bound, low, high};

  ASSERT_EQ(call.operands.size(), 3U);
  EXPECT_EQ(call.operands[0].kind, Expression::Kind::kReference);
  EXPECT_EQ(call.operands[0].text, "E_len");
  EXPECT_EQ(call.operands[2].integer, 65535);

  Expression conjunction;
  conjunction.kind = Expression::Kind::kAnd;
  conjunction.operands = {call, call};
  EXPECT_EQ(conjunction.operands.size(), 2U);

  Expression literal;
  literal.kind = Expression::Kind::kBool;
  literal.boolean = true;
  EXPECT_TRUE(literal.boolean);
  EXPECT_EQ(literal.integer, 0);
}

// Ruling F2: the minimal factory is fact-bearing. The next task's validator
// tests dereference `facts.front()` and dangle the primary claim subject, so a
// factless "minimal" case would be undefined behavior.
TEST(EvidenceCaseTest, MinimalCaseIsFactBearing) {
  const EvidenceCase value = MakeValidMinimalEvidenceCase();

  EXPECT_EQ(value.schema_version, "eir.v1");
  EXPECT_EQ(value.level, EvidenceLevel::kL0);
  EXPECT_EQ(value.verification_state, VerificationState::kPossibleDefect);
  EXPECT_FALSE(value.evidence_id.has_value());

  ASSERT_EQ(value.facts.size(), 1U);
  const std::string provenance_id = value.facts.front().provenance_id;
  EXPECT_FALSE(provenance_id.empty());

  bool provenance_found = false;
  for (const auto& record : value.provenance) {
    if (record.id == provenance_id) {
      provenance_found = true;
    }
  }
  EXPECT_TRUE(provenance_found);
  EXPECT_TRUE(IsOverflowComparison(value.primary_claim.predicate))
      << "the minimal case carries the buffer-overflow claim predicate";
}

TEST(EvidenceCaseTest, MinimalCaseDeclaresItsClaimSubject) {
  const EvidenceCase value = MakeValidMinimalEvidenceCase();
  ASSERT_FALSE(value.primary_claim.subject.empty());
  bool subject_declared = false;
  for (const auto& entity : value.entities) {
    if (entity.id == value.primary_claim.subject) {
      subject_declared = true;
    }
  }
  EXPECT_TRUE(subject_declared);
  EXPECT_TRUE(UnresolvedReferences(value).empty());
}

TEST(EvidenceCaseTest, MinimalCaseBindsOneCompleteAnalysisRun) {
  const EvidenceCase value = MakeValidMinimalEvidenceCase();
  ASSERT_TRUE(value.program.analysis_run_id.has_value());
  EXPECT_FALSE(value.program.repository_id.empty());
  EXPECT_FALSE(value.program.revision_id.empty());
  EXPECT_FALSE(value.program.build_variant_id.empty());
  EXPECT_FALSE(value.program.target_triple.empty());
  EXPECT_FALSE(value.program.analysis_configuration_id.empty());
  EXPECT_FALSE(value.program.type_layout_id.empty());

  ASSERT_FALSE(value.provenance.empty());
  for (const auto& record : value.provenance) {
    ASSERT_TRUE(record.analysis_run_id.has_value());
    EXPECT_EQ(*record.analysis_run_id, *value.program.analysis_run_id);
  }
}

TEST(EvidenceCaseTest, OverflowCaseIsFullyPopulated) {
  const EvidenceCase value = MakeOverflowEvidenceCase();

  EXPECT_EQ(value.schema_version, "eir.v1");
  EXPECT_EQ(value.level, EvidenceLevel::kL1);
  EXPECT_EQ(value.verification_state, VerificationState::kPossibleDefect);
  EXPECT_EQ(value.primary_claim.kind, ClaimKind::kBufferOverflow);
  EXPECT_EQ(value.primary_claim.severity, Severity::kHigh);
  EXPECT_TRUE(IsOverflowComparison(value.primary_claim.predicate));

  EXPECT_FALSE(value.entities.empty());
  EXPECT_FALSE(value.edges.empty());
  EXPECT_FALSE(value.paths.empty());
  EXPECT_GE(value.facts.size(), 2U);
  EXPECT_FALSE(value.unknowns.empty());
  EXPECT_FALSE(value.constraints.empty());
  EXPECT_FALSE(value.provenance.empty());
  EXPECT_FALSE(value.summaries.empty());
  EXPECT_FALSE(value.dependencies.empty());
  EXPECT_FALSE(value.omissions.empty());
  ASSERT_EQ(value.proof_obligations.size(), 1U);
  EXPECT_TRUE(UnresolvedReferences(value).empty());
}

// The demo requires a value-flow path from the copied length to the sink; its
// segments are ordered and must resolve to declared entities.
TEST(EvidenceCaseTest, OverflowCaseCarriesAConnectedValueFlowPath) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.paths.empty());
  const Path& path = value.paths.front();
  EXPECT_EQ(path.kind, PathKind::kValueFlow);
  ASSERT_GE(path.entity_ids.size(), 2U);
  EXPECT_EQ(path.feasibility, Feasibility::kSat);

  // Every consecutive segment is realized by a declared edge.
  for (std::size_t i = 1; i < path.entity_ids.size(); ++i) {
    bool connected = false;
    for (const auto& edge : value.edges) {
      if (edge.from == path.entity_ids[i - 1] &&
          edge.to == path.entity_ids[i]) {
        connected = true;
      }
    }
    EXPECT_TRUE(connected) << path.entity_ids[i - 1] << " -> "
                           << path.entity_ids[i];
  }
}

// M10C never strengthens asymmetry into a negative fact, and a truncated
// dominating-check query is visible as an unknown plus an omission.
TEST(EvidenceCaseTest, OverflowCaseRecordsTruncationWithoutNegativeFact) {
  const EvidenceCase value = MakeOverflowEvidenceCase();

  for (const auto& fact : value.facts) {
    EXPECT_NE(fact.epistemic, EpistemicState::kMustNot);
    EXPECT_NE(fact.epistemic, EpistemicState::kUnspecified);
  }

  bool truncated_unknown = false;
  for (const auto& unknown : value.unknowns) {
    if (unknown.reason == "dominating-check query truncated") {
      truncated_unknown = true;
    }
  }
  EXPECT_TRUE(truncated_unknown);

  bool expandable_omission = false;
  for (const auto& omission : value.omissions) {
    if (omission.expandable) {
      expandable_omission = true;
    }
  }
  EXPECT_TRUE(expandable_omission);
}

// The truncated-check unknown keeps both reason ends: the grammar's closed code
// and the exact free-text sentence the demo pins. Neither replaces the other.
TEST(EvidenceCaseTest, OverflowCaseCarriesBothTruncationReasonForms) {
  const EvidenceCase value = MakeOverflowEvidenceCase();

  std::size_t matching = 0;
  for (const auto& unknown : value.unknowns) {
    if (unknown.reason_code == UnknownReasonCode::kAnalysisTimeout) {
      ++matching;
      EXPECT_EQ(unknown.reason, "dominating-check query truncated");
    }
  }
  EXPECT_EQ(matching, 1U);

  bool vendor_code = false;
  for (const auto& unknown : value.unknowns) {
    if (unknown.reason == "EXTERNAL_FUNCTION") {
      vendor_code = unknown.reason_code == UnknownReasonCode::kExternalFunction;
    }
  }
  EXPECT_TRUE(vendor_code);

  for (const auto& unknown : value.unknowns) {
    EXPECT_NE(unknown.reason_code, UnknownReasonCode::kUnspecified);
  }
}

TEST(EvidenceCaseTest, OverflowCaseBindsOneAnalysisRun) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_TRUE(value.program.analysis_run_id.has_value());
  ASSERT_FALSE(value.provenance.empty());
  for (const auto& record : value.provenance) {
    ASSERT_TRUE(record.analysis_run_id.has_value());
    EXPECT_EQ(*record.analysis_run_id, *value.program.analysis_run_id);
  }
}

// M10C constructs the initial obligation only: PENDING, with the requested
// verifier kinds and no result or producer authority.
TEST(EvidenceCaseTest, OverflowCaseProofObligationIsPending) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_EQ(value.proof_obligations.size(), 1U);
  const ProofObligation& obligation = value.proof_obligations.front();
  EXPECT_EQ(obligation.goal_kind, ProofGoalKind::kProve);
  EXPECT_EQ(obligation.status, ProofStatus::kPending);
  EXPECT_TRUE(obligation.result_id.empty());
  EXPECT_TRUE(obligation.verification_producer.empty());
  EXPECT_FALSE(obligation.verifier_kinds.empty());
  EXPECT_EQ(obligation.predicate.kind, Expression::Kind::kForAll);
}

// Hypotheses stay distinguishable from authoritative facts: the case carries no
// hypothesis whose local ID collides with a fact.
TEST(EvidenceCaseTest, OverflowCaseKeepsHypothesesOutOfFacts) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  for (const auto& hypothesis : value.hypotheses) {
    for (const auto& fact : value.facts) {
      EXPECT_NE(hypothesis.id, fact.id);
    }
  }
}

TEST(EvidenceCaseTest, OverflowCaseRetainsSummaryAndDependencyIdentity) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.summaries.empty());
  const SummaryReference& summary = value.summaries.front();
  EXPECT_EQ(summary.summary_id.kind, core::IdKind::kFunctionSummary);
  EXPECT_FALSE(summary.components.empty());

  bool summary_dependency = false;
  for (const auto& dependency : value.dependencies) {
    if (dependency.kind == DependencyKind::kSummary) {
      summary_dependency = true;
      EXPECT_EQ(dependency.stable_id, summary.summary_id);
    }
    EXPECT_NE(dependency.kind, DependencyKind::kUnspecified);
  }
  EXPECT_TRUE(summary_dependency);
}

TEST(EvidenceCaseTest, BuilderRequestCarriesLevelAndTypedHandoff) {
  const EvidenceBuildRequest request =
      EvidenceScenarioBuilder().BuildRequest(EvidenceLevel::kL1);

  EXPECT_EQ(request.level, EvidenceLevel::kL1);
  EXPECT_FALSE(request.context.repository_id.empty());
  EXPECT_FALSE(request.context.revision_id.empty());
  EXPECT_FALSE(request.context.build_variant_id.empty());
  EXPECT_FALSE(request.context.target_triple.empty());
  EXPECT_EQ(request.input.claim_seed.kind, ClaimKind::kBufferOverflow);
  EXPECT_EQ(request.input.claim_seed.severity, Severity::kHigh);
  EXPECT_FALSE(request.input.claim_seed.subject_ref.digest_hex.empty());
  EXPECT_FALSE(request.input.flow_slice.nodes.empty());
  EXPECT_FALSE(request.input.query_completion_facts.empty());
  ASSERT_EQ(request.input.query_completion_facts.size(),
            request.input.query_completion_bindings.size());

  // Every query provenance reference resolves to a completion fact and binding.
  const std::array<const QueryResultMetadata*, 6> metadata{
      {&request.input.flow_slice.metadata, &request.input.ranges.metadata,
       &request.input.capacities.metadata, &request.input.aliases.metadata,
       &request.input.dominating_checks.metadata, &request.input.unknowns.metadata}};
  for (const auto* entry : metadata) {
    EXPECT_TRUE(ValidateQueryResultMetadata(*entry).ok());
    bool fact_found = false;
    bool binding_found = false;
    for (const auto& fact : request.input.query_completion_facts) {
      if (fact.fact_id == entry->query_provenance_id) {
        fact_found = true;
      }
    }
    for (const auto& binding : request.input.query_completion_bindings) {
      if (binding.fact_id == entry->query_provenance_id) {
        binding_found = true;
      }
    }
    EXPECT_TRUE(fact_found);
    EXPECT_TRUE(binding_found);
  }
}

// The truncated-check helper produces matching truncated metadata, completion
// certificate, and case-visible unknown rather than an invalid combination.
TEST(EvidenceCaseTest, TruncatedDominatingChecksStayConsistent) {
  const EvidenceBuildRequest request = EvidenceScenarioBuilder()
                                           .WithTruncatedDominatingChecks(
                                               TruncationReason::kMaxPaths)
                                           .BuildRequest(EvidenceLevel::kL1);

  EXPECT_EQ(request.input.dominating_checks.metadata.completeness,
            QueryCompleteness::kTruncated);
  ASSERT_EQ(request.input.dominating_checks.metadata.truncation_reasons.size(),
            1U);
  EXPECT_EQ(
      request.input.dominating_checks.metadata.truncation_reasons.front(),
      TruncationReason::kMaxPaths);
  EXPECT_TRUE(request.input.dominating_checks.facts.empty());
  EXPECT_TRUE(
      ValidateQueryResultMetadata(request.input.dominating_checks.metadata)
          .ok());

  const EvidenceCase value = MakeOverflowEvidenceCase();
  bool truncated_unknown = false;
  for (const auto& unknown : value.unknowns) {
    if (unknown.reason == "dominating-check query truncated") {
      truncated_unknown = true;
    }
  }
  EXPECT_TRUE(truncated_unknown);
}

TEST(EvidenceCaseTest, CompleteDominatingChecksRemainComplete) {
  const EvidenceBuildRequest request =
      EvidenceScenarioBuilder().BuildRequest(EvidenceLevel::kL1);
  EXPECT_EQ(request.input.dominating_checks.metadata.completeness,
            QueryCompleteness::kComplete);
  EXPECT_TRUE(
      request.input.dominating_checks.metadata.truncation_reasons.empty());
}
