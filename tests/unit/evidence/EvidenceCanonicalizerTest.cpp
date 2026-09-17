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

// EvidenceCanonicalizerTest.cpp — canonical bytes and `EvidenceID`
// (`VID-007` and `VID-008` of the M10B/M10C API-to-Evidence-IR test design
// spec, section 11).
//
//   * VID-007 IdentityIgnoresUnorderedConstructionAndLocalPaths — reversing
//     every semantically unordered collection, and re-deriving the case from
//     the same typed handoff, leaves canonical bytes and `EvidenceID` exactly
//     equal. The checkout-root half of the catalog row is structurally
//     satisfied rather than executed: `ProgramBinding` carries no filesystem
//     path, so no local path can reach the case or its bytes. Task 9 owns the
//     executable path-variation test at the M10B to EIR assembly boundary,
//     where the path actually exists.
//   * VID-008 SemanticOrContextChangeChangesIdentity — one mutation at a time
//     (epistemic state, predicate, ordered path segment, analyzer version,
//     analysis run, program binding, dependency, omission, verification state,
//     proof status) changes both the canonical bytes and the `EvidenceID`.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/core/CanonicalValue.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceValidator.h"

using namespace veritas;
using namespace veritas::evidence;
using namespace veritas::testing;

namespace {

// A deterministic run identity for the mutations that rebind the case.
core::StableId RunId(std::string_view name) {
  return EvidenceScenarioBuilder().Id(core::IdKind::kAnalysisRun, name);
}

// Expression builders for the operand-order tests: the shared fixtures carry
// no commutative node, so the canonicalizer's one expression rule needs its
// own case.
Expression Ref(std::string id) {
  Expression expression;
  expression.kind = Expression::Kind::kReference;
  expression.text = std::move(id);
  return expression;
}

Expression CompareOp(std::string op, Expression left, Expression right) {
  Expression expression;
  expression.kind = Expression::Kind::kCompare;
  expression.text = std::move(op);
  expression.operands = {std::move(left), std::move(right)};
  return expression;
}

Expression AndOp(std::vector<Expression> operands) {
  Expression expression;
  expression.kind = Expression::Kind::kAnd;
  expression.operands = std::move(operands);
  return expression;
}

// Replaces the case's constraint expression with `expression`, which must refer
// only to declared entities.
void SetConstraintExpression(EvidenceCase& value, Expression expression) {
  ASSERT_FALSE(value.constraints.empty());
  value.constraints.front().expression = std::move(expression);
}

// Reverses every collection whose order the canonical contract declares
// semantically unordered, at every nesting level the fixtures use, and leaves
// every semantically ordered sequence (path segments, non-commutative
// expression operands) untouched.
void ReverseUnorderedCollections(EvidenceCase& value) {
  EvidenceScenarioBuilder::Reverse(value.program.analyzer_versions);
  EvidenceScenarioBuilder::Reverse(value.entities);
  EvidenceScenarioBuilder::Reverse(value.edges);
  EvidenceScenarioBuilder::Reverse(value.paths);
  EvidenceScenarioBuilder::Reverse(value.facts);
  EvidenceScenarioBuilder::Reverse(value.assumptions);
  EvidenceScenarioBuilder::Reverse(value.hypotheses);
  EvidenceScenarioBuilder::Reverse(value.unknowns);
  EvidenceScenarioBuilder::Reverse(value.constraints);
  EvidenceScenarioBuilder::Reverse(value.provenance);
  EvidenceScenarioBuilder::Reverse(value.proof_obligations);
  EvidenceScenarioBuilder::Reverse(value.summaries);
  EvidenceScenarioBuilder::Reverse(value.dependencies);
  EvidenceScenarioBuilder::Reverse(value.omissions);

  for (Path& path : value.paths) {
    EvidenceScenarioBuilder::Reverse(path.conditions);
  }
  for (Unknown& unknown : value.unknowns) {
    EvidenceScenarioBuilder::Reverse(unknown.blocking_ids);
  }
  for (Provenance& record : value.provenance) {
    EvidenceScenarioBuilder::Reverse(record.input_fact_ids);
  }
  for (SummaryReference& summary : value.summaries) {
    EvidenceScenarioBuilder::Reverse(summary.components);
  }
  for (ProofObligation& obligation : value.proof_obligations) {
    EvidenceScenarioBuilder::Reverse(obligation.verifier_kinds);
  }
}

// The canonical bytes of two cases are equal, or a failure that says which
// side failed to encode.
::testing::AssertionResult SameCanonicalBytes(const EvidenceCase& left,
                                            const EvidenceCase& right) {
  const auto left_bytes = CanonicalEvidenceBytes(left);
  if (!left_bytes.ok()) {
    return ::testing::AssertionFailure()
           << "left canonicalization failed: " << left_bytes.status().message();
  }
  const auto right_bytes = CanonicalEvidenceBytes(right);
  if (!right_bytes.ok()) {
    return ::testing::AssertionFailure()
           << "right canonicalization failed: "
           << right_bytes.status().message();
  }
  if (*left_bytes == *right_bytes) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "canonical bytes differ: " << left_bytes->size() << " vs "
         << right_bytes->size() << " bytes";
}

// Both cases finalize, and their identities are equal, or a failure naming the
// status that refused.
::testing::AssertionResult SameEvidenceId(EvidenceCase* left,
                                        EvidenceCase* right) {
  const Status left_status = FinalizeEvidenceIdentity(left);
  if (!left_status.ok()) {
    return ::testing::AssertionFailure()
           << "left finalization failed: " << left_status.message();
  }
  const Status right_status = FinalizeEvidenceIdentity(right);
  if (!right_status.ok()) {
    return ::testing::AssertionFailure()
           << "right finalization failed: " << right_status.message();
  }
  if (left->evidence_id == right->evidence_id) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "evidence IDs differ";
}

// One mutation changes the case's canonical bytes and its identity. `mutate`
// runs on the right-hand copy only. Both sides must stay well-formed: a
// mutation that merely broke the case would change the bytes for the wrong
// reason and would prove nothing about identity.
template <typename Mutation>
void ExpectMutationChangesIdentity(EvidenceCase left, Mutation mutate,
                                   const char* what) {
  EvidenceCase right = left;
  mutate(right);

  EXPECT_FALSE(SameCanonicalBytes(left, right)) << what;

  const Status left_status = FinalizeEvidenceIdentity(&left);
  ASSERT_TRUE(left_status.ok()) << what << ": " << left_status.message();
  const Status right_status = FinalizeEvidenceIdentity(&right);
  ASSERT_TRUE(right_status.ok()) << what << ": " << right_status.message();

  ASSERT_TRUE(left.evidence_id.has_value()) << what;
  ASSERT_TRUE(right.evidence_id.has_value()) << what;
  EXPECT_NE(*left.evidence_id, *right.evidence_id) << what;
}

// A mutation that intentionally breaks well-formedness: the case must be
// rejected by the gate, so the identity is computed through the pure
// `ComputeEvidenceId` path and the bytes are compared directly. The point is
// that the bytes follow the case's content, not that every mutation is legal.
template <typename Mutation>
void ExpectMutationChangesBytesAndId(EvidenceCase left, Mutation mutate,
                                     const char* what) {
  EvidenceCase right = left;
  mutate(right);

  EXPECT_FALSE(SameCanonicalBytes(left, right)) << what;
  EXPECT_FALSE(RequireValidEvidenceCase(right).ok())
      << "expected the mutation to break well-formedness: " << what;

  const auto left_id = ComputeEvidenceId(left);
  ASSERT_TRUE(left_id.ok()) << left_id.status().message();
  const auto right_id = ComputeEvidenceId(right);
  ASSERT_TRUE(right_id.ok()) << right_id.status().message();
  EXPECT_NE(*left_id, *right_id) << what;
}

}  // namespace

// --- VID-007: construction order and local paths do not affect identity ------

TEST(EvidenceCanonicalizerTest, VID007IdentityIgnoresUnorderedConstruction) {
  const EvidenceCase original = MakeOverflowEvidenceCase();

  EvidenceCase reversed = original;
  ReverseUnorderedCollections(reversed);

  EXPECT_TRUE(SameCanonicalBytes(original, reversed));

  // The reversals that actually carry the assertion: a one-element collection
  // reverses to itself and would prove nothing.
  ASSERT_EQ(reversed.entities.size(), original.entities.size());
  ASSERT_GE(reversed.entities.size(), 2u);
  ASSERT_GE(reversed.facts.size(), 2u);
  ASSERT_GE(reversed.provenance.size(), 2u);
  ASSERT_GE(reversed.dependencies.size(), 2u);
  ASSERT_GE(reversed.omissions.size(), 2u);
  ASSERT_GE(reversed.unknowns.size(), 2u);
  ASSERT_GE(reversed.program.analyzer_versions.size(), 2u);

  EvidenceCase left = original;
  EvidenceCase right = reversed;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
}

TEST(EvidenceCanonicalizerTest, VID007IdentityIgnoresUnorderedConstructionL0) {
  const EvidenceCase original = MakeValidMinimalEvidenceCase();

  EvidenceCase reversed = original;
  ReverseUnorderedCollections(reversed);

  EXPECT_TRUE(SameCanonicalBytes(original, reversed));

  EvidenceCase left = original;
  EvidenceCase right = reversed;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
  ASSERT_TRUE(left.evidence_id.has_value());
}

TEST(EvidenceCanonicalizerTest, VID007CanonicalBytesIgnoreExistingEvidenceId) {
  const EvidenceCase original = MakeOverflowEvidenceCase();

  EvidenceCase with_stale_id = original;
  with_stale_id.evidence_id = RunId("stale_evidence_identity");

  EXPECT_TRUE(SameCanonicalBytes(original, with_stale_id));

  EvidenceCase left = original;
  EvidenceCase right = with_stale_id;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
  EXPECT_NE(left.evidence_id, std::optional<core::StableId>(
                                  RunId("stale_evidence_identity")));
}

// The case carries no top-level display name: `EvidenceCase`'s members are the
// semantic record set alone, so a presentation-only case label has nothing to
// vary and cannot reach the bytes. The EIR-T writer derives the case's
// top-level identifier from this digest instead.
TEST(EvidenceCanonicalizerTest, VID007IdentityIsStableAcrossConstructions) {
  const EvidenceCase first = MakeOverflowEvidenceCase();
  const EvidenceCase second = MakeOverflowEvidenceCase();

  EXPECT_TRUE(SameCanonicalBytes(first, second));

  EvidenceCase left = first;
  EvidenceCase right = second;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
}

// --- VID-008: one semantic change at a time changes bytes and identity -------

TEST(EvidenceCanonicalizerTest, VID008EpistemicChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        value.facts.front().epistemic = EpistemicState::kMay;
      },
      "epistemic state");
}

TEST(EvidenceCanonicalizerTest, VID008PredicateChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        // The range fact's upper bound: `range(E_copy_length, min, max)`.
        ASSERT_EQ(value.facts.front().predicate.operands.size(), 3u);
        value.facts.front().predicate.operands[2].integer += 1;
      },
      "predicate literal");
}

// A path's segment sequence is the path, so reordering it is a different
// claim. The reversal here deliberately breaks the path's connectivity (the
// fixture's value-flow edges run one way), so it is compared through the pure
// bytes and identity functions: the point is that the sequence is hashed, not
// that a disconnected path is well-formed.
TEST(EvidenceCanonicalizerTest, VID008PathSegmentOrderChangesIdentity) {
  ExpectMutationChangesBytesAndId(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        ASSERT_GE(value.paths.front().entity_ids.size(), 2u);
        std::reverse(value.paths.front().entity_ids.begin(),
                     value.paths.front().entity_ids.end());
      },
      "ordered path segments");
}

TEST(EvidenceCanonicalizerTest, VID008ProgramBindingChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        value.program.analysis_configuration_id = "cfg:other";
      },
      "program binding");
}

TEST(EvidenceCanonicalizerTest, VID008AnalyzerVersionChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        ASSERT_FALSE(value.program.analyzer_versions.empty());
        value.program.analyzer_versions.front().version = "0.2";
      },
      "analyzer version");
}

TEST(EvidenceCanonicalizerTest, VID008DependencyChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        ASSERT_FALSE(value.dependencies.empty());
        value.dependencies.front().stable_id =
            EvidenceScenarioBuilder().Id(core::IdKind::kFact,
                                         "other_dependency");
      },
      "dependency");
}

TEST(EvidenceCanonicalizerTest, VID008OmissionChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        // The fixture's non-expandable omission. The truncated-query omission
        // must stay expandable to remain well-formed (`hidden_omission`), so
        // flipping that one would change the bytes for the wrong reason.
        const auto omission = std::find_if(
            value.omissions.begin(), value.omissions.end(),
            [](const Omission& candidate) { return !candidate.expandable; });
        ASSERT_NE(omission, value.omissions.end());
        omission->expandable = true;
      },
      "omission");
}

TEST(EvidenceCanonicalizerTest, VID008VerificationStateChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        value.verification_state = VerificationState::kInconclusive;
      },
      "verification state");
}

TEST(EvidenceCanonicalizerTest, VID008ProofStatusChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        ASSERT_FALSE(value.proof_obligations.empty());
        ProofObligation& obligation = value.proof_obligations.front();
        obligation.status = ProofStatus::kProved;
        obligation.result_id = "R_proof";
        obligation.verification_producer = "smt.z3";
      },
      "proof status");
}

// The analysis run is part of the program binding, and every provenance record
// must belong to it. Rebinding the case to another run therefore moves the
// program binding and the provenance records together: still well-formed, and a
// different case.
TEST(EvidenceCanonicalizerTest, VID008AnalysisRunChangeChangesIdentity) {
  ExpectMutationChangesIdentity(
      MakeOverflowEvidenceCase(),
      [](EvidenceCase& value) {
        const core::StableId run_id = RunId("other_run");
        value.program.analysis_run_id = run_id;
        for (Provenance& record : value.provenance) {
          record.analysis_run_id = run_id;
        }
      },
      "analysis run");
}

// --- Commutative and non-commutative expression operands ---------------------

// `and` is commutative: the operand order is not part of the case.
TEST(EvidenceCanonicalizerTest, CommutativeOperandsAreOrderFreeInIdentity) {
  EvidenceCase left = MakeOverflowEvidenceCase();
  ASSERT_GE(left.entities.size(), 3u);
  const std::string first = left.entities[0].id;
  const std::string second = left.entities[1].id;
  const std::string third = left.entities[2].id;

  SetConstraintExpression(
      left, AndOp({CompareOp("==", Ref(first), Ref(second)),
                   CompareOp("!=", Ref(first), Ref(third))}));

  EvidenceCase right = left;
  Expression& operands = right.constraints.front().expression;
  std::reverse(operands.operands.begin(), operands.operands.end());

  EXPECT_TRUE(SameCanonicalBytes(left, right));
  EXPECT_TRUE(SameEvidenceId(&left, &right));
}

// A comparison's operands are ordered: its left and right sides are part of the
// claim, so the canonicalizer never swaps them.
TEST(EvidenceCanonicalizerTest, SwappedComparisonOperandsChangeIdentity) {
  EvidenceCase left = MakeOverflowEvidenceCase();
  ASSERT_GE(left.entities.size(), 2u);
  const std::string first = left.entities[0].id;
  const std::string second = left.entities[1].id;
  SetConstraintExpression(left, CompareOp("==", Ref(first), Ref(second)));

  EvidenceCase right = left;
  right.constraints.front().expression =
      CompareOp("==", Ref(second), Ref(first));

  EXPECT_FALSE(SameCanonicalBytes(left, right));

  const Status left_status = FinalizeEvidenceIdentity(&left);
  ASSERT_TRUE(left_status.ok()) << left_status.message();
  const Status right_status = FinalizeEvidenceIdentity(&right);
  ASSERT_TRUE(right_status.ok()) << right_status.message();
  ASSERT_TRUE(left.evidence_id.has_value());
  ASSERT_TRUE(right.evidence_id.has_value());
  EXPECT_NE(*left.evidence_id, *right.evidence_id);
}

// The finalize contract itself: a mutation that breaks well-formedness is
// refused, and the refused call leaves `evidence_id` exactly as it was.
TEST(EvidenceCanonicalizerTest, FinalizeRefusesInvalidCaseAndLeavesIdUnset) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.entities.empty());
  value.entities.front().kind = EntityKind::kUnspecified;

  const Status status = FinalizeEvidenceIdentity(&value);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(value.evidence_id.has_value());
}

TEST(EvidenceCanonicalizerTest,
     FinalizePreservesPreviouslyAssignedIdOnFailure) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  const core::StableId assigned = RunId("previously_assigned");
  value.evidence_id = assigned;
  ASSERT_FALSE(value.facts.empty());
  value.facts.front().epistemic = EpistemicState::kUnspecified;

  const Status status = FinalizeEvidenceIdentity(&value);
  EXPECT_FALSE(status.ok());
  ASSERT_TRUE(value.evidence_id.has_value());
  EXPECT_EQ(*value.evidence_id, assigned);
}

TEST(EvidenceCanonicalizerTest, FinalizeRejectsNullCase) {
  const Status status = FinalizeEvidenceIdentity(nullptr);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
}
