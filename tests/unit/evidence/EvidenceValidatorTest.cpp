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

// EvidenceValidatorTest.cpp — the evidence well-formedness catalog
// (`VID-001` … `VID-006` of the M10B/M10C API-to-Evidence-IR test design
// spec, section 11), plus the individual failures the milestone plan's Task 3
// enumerates.
//
// Every test is named with the catalog row it implements, so coverage is
// auditable from the source and not from a CTest display name:
//
//   * VID-001 MinimalInitialCaseIsValid — the smallest legal L0 case and the
//     populated L1 demo case both validate, and validation does not mutate
//     what it validates.
//   * VID-002 RejectsDuplicateAndDanglingReferences — duplicate local IDs
//     (including a hypothesis colliding with a fact) and every dangling
//     reference family.
//   * VID-003 RejectsDerivedFactWithoutProvenance — a derived fact with no
//     provenance, and with provenance the case does not declare.
//   * VID-004 RejectsInvalidExpressionAndDisconnectedPath — arity and type
//     failures, a connective that directly nests inside itself, the
//     connective shapes that must stay legal, and a path whose consecutive
//     segments are not adjacent.
//   * VID-005 EnforcesProofAuthority — a decided result without a producer,
//     a pending obligation carrying a result, a promoted case state without
//     proof, and the accepted authority-bearing combination.
//   * VID-006 RequiresVisibleOmissions — a withheld summary with no omission,
//     a non-expandable withheld summary, and a hidden truncated query.
//
// The remaining tests cover the further failures the plan lists: schema
// version, missing program context, a mixed analysis run, a truncation
// unknown with no query-completion provenance, an invalid summary stable ID,
// a missing epistemic state, and a hypothesis used as authoritative input.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/EvidenceValidator.h"

using namespace veritas;
using namespace veritas::evidence;
using namespace veritas::testing;

namespace {

// The handle prefix `veritas_evidence` declares an entity's local identifier
// with: the entity whose analysis label is `copy_length` is declared `E_` plus
// that label. Only the handle is a declaration; the label it wraps is not.
constexpr std::string_view kEntityHandlePrefix = "E_";

bool HasCode(const EvidenceValidationReport& report,
             EvidenceValidationCode code) {
  for (const EvidenceValidationIssue& issue : report.issues) {
    if (issue.code == code) {
      return true;
    }
  }
  return false;
}

std::string FirstIssue(const EvidenceValidationReport& report) {
  if (report.issues.empty()) {
    return "<no issues>";
  }
  const EvidenceValidationIssue& issue = report.issues.front();
  return std::string(ToString(issue.code)) + " at '" + issue.member_id +
         "': " + issue.message;
}

template <typename T, typename Predicate>
void EraseIf(std::vector<T>& values, Predicate predicate) {
  values.erase(std::remove_if(values.begin(), values.end(), predicate),
               values.end());
}

// A connective over `operands`, for shaping the expression the fixture's
// constraint carries.
Expression Connective(Expression::Kind kind, std::vector<Expression> operands) {
  Expression expression;
  expression.kind = kind;
  expression.operands = std::move(operands);
  return expression;
}

// The interrupted dominating-check unknown the truncated demo fixture records.
const Unknown* InterruptedUnknown(const EvidenceCase& value) {
  for (const Unknown& unknown : value.unknowns) {
    if (unknown.reason_code == UnknownReasonCode::kAnalysisTimeout) {
      return &unknown;
    }
  }
  return nullptr;
}

}  // namespace

// --- VID-001: a minimal initial case is valid -------------------------------

TEST(EvidenceValidatorTest, VID001MinimalInitialCaseIsValid) {
  const EvidenceCase value = MakeValidMinimalEvidenceCase();
  const EvidenceValidationReport report = ValidateEvidenceCase(value);
  EXPECT_TRUE(report.ok()) << FirstIssue(report);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
}

TEST(EvidenceValidatorTest, VID001PopulatedOverflowCaseIsValid) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  const EvidenceValidationReport report = ValidateEvidenceCase(value);
  EXPECT_TRUE(report.ok()) << FirstIssue(report);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
}

TEST(EvidenceValidatorTest, VID001ValidationDoesNotMutateTheCase) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  const EvidenceCase before = value;
  (void)ValidateEvidenceCase(value);
  (void)RequireValidEvidenceCase(value);
  EXPECT_TRUE(value == before);
}

// --- VID-002: duplicate and dangling references -----------------------------

TEST(EvidenceValidatorTest, VID002RejectsDanglingClaimSubject) {
  auto value = MakeValidMinimalEvidenceCase();
  value.primary_claim.subject = "missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDanglingReference);
  EXPECT_EQ(report.issues.front().member_id, value.primary_claim.id);
}

TEST(EvidenceValidatorTest, VID002RejectsDanglingPathSegment) {
  auto value = MakeValidMinimalEvidenceCase();
  value.paths.front().entity_ids[1] = "missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDanglingReference);
  EXPECT_EQ(report.issues.front().member_id, value.paths.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsDanglingProvenanceReference) {
  auto value = MakeValidMinimalEvidenceCase();
  value.facts.front().provenance_id = "PR_missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kDanglingReference));
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsDanglingSummaryFunction) {
  auto value = MakeOverflowEvidenceCase();
  value.summaries.front().function_id = "E_missing";
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kDanglingReference));
  EXPECT_EQ(report.issues.front().member_id, value.summaries.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsDanglingOmissionSubject) {
  auto value = MakeOverflowEvidenceCase();
  value.omissions.front().subject = "U_missing";
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kDanglingReference));
  EXPECT_EQ(report.issues.front().member_id, value.omissions.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsDanglingExpressionReference) {
  auto value = MakeValidMinimalEvidenceCase();
  // The range predicate addresses its value by name; a name no member declares
  // resolves to nothing.
  value.facts.front().predicate.operands.front().text = "missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDanglingReference);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsBareAnalysisLabelAsReference) {
  auto value = MakeValidMinimalEvidenceCase();
  ASSERT_FALSE(value.entities.empty());
  const std::string handle = value.entities.front().id;
  // Derive the bare label from the declared handle rather than naming it, so
  // this tracks whatever the fixture declares.
  ASSERT_GT(handle.size(), kEntityHandlePrefix.size()) << handle;
  ASSERT_EQ(handle.compare(0, kEntityHandlePrefix.size(), kEntityHandlePrefix),
            0)
      << handle;
  // An entity is addressable only under the handle it is declared with. The
  // bare analysis label the handle wraps names no declaration, so a reference
  // spelled that way is dangling — never resolved by stripping the prefix.
  value.facts.front().predicate.operands.front().text =
      handle.substr(kEntityHandlePrefix.size());
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDanglingReference);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsDuplicateEntityLocalIds) {
  auto value = MakeValidMinimalEvidenceCase();
  value.entities.push_back(value.entities.front());
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDuplicateLocalId);
  EXPECT_EQ(report.issues.front().member_id, value.entities.front().id);
}

TEST(EvidenceValidatorTest, VID002RejectsHypothesisFactIdCollision) {
  auto value = MakeOverflowEvidenceCase();
  // A hypothesis and an authoritative fact share one case-local identifier
  // space: colliding on an identifier would let one stand in for the other.
  value.hypotheses.front().id = value.facts.front().id;
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDuplicateLocalId);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

// --- VID-003: derived facts carry provenance --------------------------------

TEST(EvidenceValidatorTest, VID003RejectsDerivedFactWithoutProvenance) {
  auto value = MakeValidMinimalEvidenceCase();
  value.facts.front().derived = true;
  value.facts.front().provenance_id.clear();
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingProvenance);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

TEST(EvidenceValidatorTest, VID003RejectsDerivedFactWithUndeclaredProvenance) {
  auto value = MakeValidMinimalEvidenceCase();
  value.facts.front().derived = true;
  value.facts.front().provenance_id = "PR_missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingProvenance);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

// --- VID-004: expressions and path connectivity -----------------------------

TEST(EvidenceValidatorTest, VID004RejectsInvalidExpressionArity) {
  auto value = MakeOverflowEvidenceCase();
  // A comparison is a binary operator; a third operand is not a predicate.
  Expression& expression = value.constraints.front().expression;
  expression.operands.push_back(expression.operands.front());
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kExpressionType));
  EXPECT_EQ(report.issues.front().member_id, value.constraints.front().id);
}

TEST(EvidenceValidatorTest, VID004RejectsInvalidExpressionType) {
  auto value = MakeOverflowEvidenceCase();
  // A formula in a value position: the comparison's left operand becomes a
  // negation instead of the value it compares.
  value.constraints.front().expression.operands[0].kind = Expression::Kind::kNot;
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kExpressionType));
  EXPECT_EQ(report.issues.front().member_id, value.constraints.front().id);
}

// `EvidenceCase.h` declares `kAnd`/`kOr` "two or more operands, flattened",
// and the grammar reaches that shape directly: `OrExpr ::= AndExpr { "or"
// AndExpr }` builds one n-ary node, and parentheses around a conjunction group
// nothing, so no legal EIR-T parses into a directly nested connective.
// Enforcing it here — and only here — keeps one formula to one canonical
// encoding, and so to one identity.
TEST(EvidenceValidatorTest, VID004RejectsDirectlyNestedConnective) {
  auto value = MakeOverflowEvidenceCase();
  const Expression predicate = value.constraints.front().expression;
  value.constraints.front().expression = Connective(
      Expression::Kind::kAnd,
      {Connective(Expression::Kind::kAnd, {predicate, predicate}), predicate});

  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kExpressionType);
  EXPECT_EQ(report.issues.front().member_id, value.constraints.front().id);
}

// The negative control, and the half that matters: over-rejection is the risk
// this rule carries, so the shapes that must stay legal are asserted
// explicitly. Only direct same-kind nesting is rejected; a connective under
// `not`, or under the other connective, is untouched.
TEST(EvidenceValidatorTest, VID004AcceptsConnectivesNestedInOtherShapes) {
  const EvidenceCase fixture = MakeOverflowEvidenceCase();
  const Expression predicate = fixture.constraints.front().expression;
  const Expression conjunction = Connective(
      Expression::Kind::kAnd, {predicate, predicate});

  auto negated = fixture;
  negated.constraints.front().expression =
      Connective(Expression::Kind::kNot, {conjunction});
  {
    const auto report = ValidateEvidenceCase(negated);
    EXPECT_TRUE(report.ok()) << "a conjunction under a negation: "
                             << FirstIssue(report);
  }

  auto disjoined = fixture;
  disjoined.constraints.front().expression =
      Connective(Expression::Kind::kOr, {conjunction, predicate});
  {
    const auto report = ValidateEvidenceCase(disjoined);
    EXPECT_TRUE(report.ok()) << "a conjunction under a disjunction: "
                             << FirstIssue(report);
  }
}

TEST(EvidenceValidatorTest, VID004RejectsDisconnectedPath) {
  auto value = MakeValidMinimalEvidenceCase();
  // The only edge runs from the length to the sink call; the reversed path
  // has no connecting segment.
  std::swap(value.paths.front().entity_ids[0], value.paths.front().entity_ids[1]);
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kPathDisconnected);
  EXPECT_EQ(report.issues.front().member_id, value.paths.front().id);
}

TEST(EvidenceValidatorTest, VID004RejectsPathWithoutSegments) {
  auto value = MakeValidMinimalEvidenceCase();
  value.paths.front().entity_ids.clear();
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kPathDisconnected);
}

// --- VID-005: proof authority -----------------------------------------------

TEST(EvidenceValidatorTest, VID005EnforcesProofAuthority) {
  auto value = MakeOverflowEvidenceCase();
  value.proof_obligations.front().status = ProofStatus::kProved;
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kVerificationProducer));
  EXPECT_EQ(report.issues.front().member_id,
            value.proof_obligations.front().id);
}

TEST(EvidenceValidatorTest, VID005RejectsPendingObligationCarryingAResult) {
  auto value = MakeOverflowEvidenceCase();
  value.proof_obligations.front().result_id = "R_unproduced";
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kVerificationProducer));
}

TEST(EvidenceValidatorTest, VID005RejectsPromotedCaseState) {
  auto value = MakeValidMinimalEvidenceCase();
  // Only deterministic verification promotes a case state; the builder never
  // promotes on its own.
  value.verification_state = VerificationState::kVerifiedDefect;
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kVerificationProducer);
}

TEST(EvidenceValidatorTest, VID005AcceptsDecidedResultWithProducer) {
  auto value = MakeOverflowEvidenceCase();
  ProofObligation& obligation = value.proof_obligations.front();
  obligation.status = ProofStatus::kProved;
  obligation.result_id = "R_smt_1";
  obligation.verification_producer = "verifier.smt";
  value.verification_state = VerificationState::kVerifiedDefect;
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(report.ok()) << FirstIssue(report);
}

// --- VID-006: visible omissions ---------------------------------------------

TEST(EvidenceValidatorTest, VID006RequiresVisibleOmissionForWithheldSummary) {
  auto value = MakeOverflowEvidenceCase();
  // The expandable edge says the vendor summary can be expanded; dropping the
  // omission that declares it leaves the withheld detail implied by absence.
  const std::string summary_id = value.summaries.front().id;
  EraseIf(value.omissions, [&](const Omission& omission) {
    return omission.subject == summary_id;
  });
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kHiddenOmission);
}

TEST(EvidenceValidatorTest, VID006RejectsNonExpandableWithheldSummary) {
  auto value = MakeOverflowEvidenceCase();
  const std::string summary_id = value.summaries.front().id;
  bool found = false;
  for (Omission& omission : value.omissions) {
    if (omission.subject == summary_id) {
      omission.expandable = false;
      found = true;
    }
  }
  ASSERT_TRUE(found);
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kHiddenOmission);
}

TEST(EvidenceValidatorTest, VID006RejectsHiddenTruncation) {
  auto value = MakeOverflowEvidenceCase();
  const Unknown* interrupted = InterruptedUnknown(value);
  ASSERT_NE(interrupted, nullptr);
  const std::string unknown_id = interrupted->id;
  // The unknown records the open question; without the omission that declares
  // the withheld result, the truncation is hidden from progressive disclosure.
  EraseIf(value.omissions, [&](const Omission& omission) {
    return omission.subject == unknown_id;
  });
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kHiddenOmission);
  EXPECT_EQ(report.issues.front().member_id, unknown_id);
}

TEST(EvidenceValidatorTest, VID006RejectsOmissionlessCase) {
  auto value = MakeOverflowEvidenceCase();
  value.omissions.clear();
  EXPECT_TRUE(HasCode(ValidateEvidenceCase(value),
                      EvidenceValidationCode::kHiddenOmission));
}

// --- The further failures the plan enumerates -------------------------------

TEST(EvidenceValidatorTest, RejectsCaseWithoutAPrimaryClaim) {
  auto value = MakeValidMinimalEvidenceCase();
  value.primary_claim.id.clear();
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kPrimaryClaimCount);
  EXPECT_TRUE(report.issues.front().member_id.empty());
}

TEST(EvidenceValidatorTest, RejectsUnsupportedSchemaVersion) {
  auto value = MakeValidMinimalEvidenceCase();
  value.schema_version = "eir.v2";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kSchemaVersion);
  EXPECT_EQ(report.issues.front().member_id, "schema_version");
}

TEST(EvidenceValidatorTest, RejectsMissingProgramContextFields) {
  auto value = MakeValidMinimalEvidenceCase();
  value.program.revision_id.clear();
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingProgramContext);
  EXPECT_EQ(report.issues.front().member_id, "program");
}

TEST(EvidenceValidatorTest, RejectsCaseWithoutAnAnalysisRun) {
  auto value = MakeValidMinimalEvidenceCase();
  value.program.analysis_run_id = std::nullopt;
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingProgramContext);
}

TEST(EvidenceValidatorTest, RejectsProvenanceFromAnotherAnalysisRun) {
  auto value = MakeOverflowEvidenceCase();
  value.provenance.front().analysis_run_id =
      EvidenceScenarioBuilder().Id(core::IdKind::kAnalysisRun, "other-run");
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMixedProgramContext);
  EXPECT_EQ(report.issues.front().member_id, value.provenance.front().id);
}

TEST(EvidenceValidatorTest, RejectsInterruptedQueryWithoutCompletionProvenance) {
  auto value = MakeOverflowEvidenceCase();
  EraseIf(value.provenance, [](const Provenance& record) {
    return record.producer == kQueryCompletionProducerId;
  });
  const Unknown* interrupted = InterruptedUnknown(value);
  ASSERT_NE(interrupted, nullptr);
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingProvenance);
  EXPECT_EQ(report.issues.front().member_id, interrupted->id);
}

TEST(EvidenceValidatorTest, RejectsInvalidSummaryStableId) {
  auto value = MakeOverflowEvidenceCase();
  value.summaries.front().summary_id.digest_hex.clear();
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kInvalidStableId);
  EXPECT_EQ(report.issues.front().member_id, value.summaries.front().id);
}

TEST(EvidenceValidatorTest, RejectsSummaryThatIsNotAFunctionSummary) {
  auto value = MakeOverflowEvidenceCase();
  value.summaries.front().summary_id.kind = core::IdKind::kFact;
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kInvalidStableId));
  EXPECT_EQ(report.issues.front().member_id, value.summaries.front().id);
}

TEST(EvidenceValidatorTest, RejectsFactWithoutAnEpistemicState) {
  auto value = MakeOverflowEvidenceCase();
  value.facts.front().epistemic = EpistemicState::kUnspecified;
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kMissingEpistemic);
  EXPECT_EQ(report.issues.front().member_id, value.facts.front().id);
}

TEST(EvidenceValidatorTest, RejectsHypothesisAsAuthoritativeInput) {
  auto value = MakeOverflowEvidenceCase();
  // A hypothesis never feeds an authoritative derivation.
  value.provenance.front().input_fact_ids.push_back(
      value.hypotheses.front().id);
  const auto report = ValidateEvidenceCase(value);
  EXPECT_TRUE(HasCode(report, EvidenceValidationCode::kHypothesisAuthority));
  EXPECT_EQ(report.issues.front().member_id, value.provenance.front().id);
}

TEST(EvidenceValidatorTest, RequireValidEvidenceCaseNamesTheFirstIssue) {
  auto value = MakeValidMinimalEvidenceCase();
  value.primary_claim.subject = "missing";
  const Status status = RequireValidEvidenceCase(value);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("dangling_reference"), std::string::npos)
      << status.message();
  EXPECT_NE(status.message().find(value.primary_claim.id), std::string::npos)
      << status.message();
}
