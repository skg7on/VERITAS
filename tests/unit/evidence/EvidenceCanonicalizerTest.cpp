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

// A `kEvidence`-kind identity: what a finalized case's own `evidence_id`
// actually is, and so what a stale value in that field would actually hold.
core::StableId EvidenceIdentity(std::string_view name) {
  return EvidenceScenarioBuilder().Id(core::IdKind::kEvidence, name);
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

// Gives every collection the fixtures leave at a single member a second,
// distinct one. Reversing a one-member collection is a no-op, so without this
// the canonical sort of that collection is never exercised: a canonicalizer
// that emitted it in input order would still pass every test below. A clone
// keeps every reference the original already resolved and changes only its
// local handle — plus, for the reference lists, nothing at all — so the case
// stays well-formed.
void PopulateSecondMembers(EvidenceCase& value) {
  // The path's conditions first, so the path clone inherits two. The second is
  // the first under `not`, not a copy of it: two equal values encode
  // identically, so the canonical sort would have nothing to do and the
  // reversal below would pass against an encoder that emitted conditions in
  // input order. Negating keeps every reference the first already resolved, so
  // the case stays well-formed.
  for (Path& path : value.paths) {
    if (path.conditions.size() == 1) {
      Expression negated;
      negated.kind = Expression::Kind::kNot;
      negated.operands.push_back(path.conditions.front());
      path.conditions.push_back(std::move(negated));
    }
  }
  // Each unknown blocks one fact; the second is a *different* declared fact, so
  // it still resolves. A copy would encode identically and leave the sort
  // untested, for the same reason as the conditions above.
  for (Unknown& unknown : value.unknowns) {
    if (unknown.blocking_ids.size() != 1) {
      continue;
    }
    for (const Fact& fact : value.facts) {
      if (fact.id != unknown.blocking_ids.front()) {
        unknown.blocking_ids.push_back(fact.id);
        break;
      }
    }
  }
  if (value.paths.size() == 1) {
    Path second = value.paths.front();
    second.id = "P_second";
    value.paths.push_back(std::move(second));
  }
  if (value.assumptions.size() == 1) {
    Assumption second = value.assumptions.front();
    second.id = "A_second";
    value.assumptions.push_back(std::move(second));
  }
  if (value.hypotheses.size() == 1) {
    Hypothesis second = value.hypotheses.front();
    second.id = "H_second";
    value.hypotheses.push_back(std::move(second));
  }
  if (value.constraints.size() == 1) {
    Constraint second = value.constraints.front();
    second.id = "C_second";
    value.constraints.push_back(std::move(second));
  }
  if (value.proof_obligations.size() == 1) {
    ProofObligation second = value.proof_obligations.front();
    second.id = "PO_second";
    value.proof_obligations.push_back(std::move(second));
  }
  if (value.summaries.size() == 1) {
    SummaryReference second = value.summaries.front();
    second.id = "S_second";
    value.summaries.push_back(std::move(second));
  }
  if (value.facts.size() == 1) {
    Fact second = value.facts.front();
    second.id = "F_second";
    value.facts.push_back(std::move(second));
  }
  if (value.omissions.size() == 1) {
    Omission second = value.omissions.front();
    second.id = "O_second";
    value.omissions.push_back(std::move(second));
  }
}

// Every collection the reversal touches carries at least two members, so the
// reversal is a real reorder and the canonical sort is exercised. A failure
// here means the fixture lost the population that makes the reversal
// meaningful — the guard is the point, not decoration.
::testing::AssertionResult EveryReversedCollectionIsPopulated(
    const EvidenceCase& value) {
  const std::pair<std::string_view, std::size_t> members[] = {
      {"program.analyzer_versions", value.program.analyzer_versions.size()},
      {"entities", value.entities.size()},
      {"edges", value.edges.size()},
      {"paths", value.paths.size()},
      {"facts", value.facts.size()},
      {"assumptions", value.assumptions.size()},
      {"hypotheses", value.hypotheses.size()},
      {"unknowns", value.unknowns.size()},
      {"constraints", value.constraints.size()},
      {"provenance", value.provenance.size()},
      {"proof_obligations", value.proof_obligations.size()},
      {"summaries", value.summaries.size()},
      {"dependencies", value.dependencies.size()},
      {"omissions", value.omissions.size()},
  };
  for (const auto& member : members) {
    if (member.second < 2) {
      return ::testing::AssertionFailure()
             << member.first << " carries " << member.second
             << " member(s), so reversing it tests nothing";
    }
  }
  for (const Path& path : value.paths) {
    if (path.conditions.size() < 2) {
      return ::testing::AssertionFailure()
             << "path " << path.id << " carries " << path.conditions.size()
             << " condition(s), so reversing them tests nothing";
    }
  }
  for (const Unknown& unknown : value.unknowns) {
    if (unknown.blocking_ids.size() < 2) {
      return ::testing::AssertionFailure()
             << "unknown " << unknown.id << " carries "
             << unknown.blocking_ids.size()
             << " blocking id(s), so reversing them tests nothing";
    }
  }
  for (const SummaryReference& summary : value.summaries) {
    if (summary.components.size() < 2) {
      return ::testing::AssertionFailure()
             << "summary " << summary.id << " carries "
             << summary.components.size()
             << " component(s), so reversing them tests nothing";
    }
  }
  for (const ProofObligation& obligation : value.proof_obligations) {
    if (obligation.verifier_kinds.size() < 2) {
      return ::testing::AssertionFailure()
             << "obligation " << obligation.id << " carries "
             << obligation.verifier_kinds.size()
             << " verifier kind(s), so reversing them tests nothing";
    }
  }
  // The input list is reversed on every provenance record; one record with two
  // inputs is enough to exercise the sort it uses.
  for (const Provenance& record : value.provenance) {
    if (record.input_fact_ids.size() >= 2) {
      return ::testing::AssertionSuccess();
    }
  }
  return ::testing::AssertionFailure()
         << "no provenance record carries two input fact ids, so reversing "
            "them tests nothing";
}

// Size is not the invariant the reversal needs — distinctness is. Two equal
// values encode identically, so reversing them leaves the bytes unchanged:
// an encoder that emitted the collection in input order would pass exactly as
// an encoder that sorted it, and the guard above would still be satisfied.
// Collapsing the second element onto the first must therefore change the
// case's canonical bytes; when it does not, the pair is a duplicate and the
// reversal proves nothing about the sort.
template <typename Collapse>
::testing::AssertionResult CollapsingSecondElementChangesBytes(
    const EvidenceCase& value, Collapse collapse, const char* what) {
  EvidenceCase collapsed = value;
  collapse(collapsed);

  const auto original_bytes = CanonicalEvidenceBytes(value);
  if (!original_bytes.ok()) {
    return ::testing::AssertionFailure()
           << what << ": canonicalization failed: "
           << original_bytes.status().message();
  }
  const auto collapsed_bytes = CanonicalEvidenceBytes(collapsed);
  if (!collapsed_bytes.ok()) {
    return ::testing::AssertionFailure()
           << what << ": canonicalization failed: "
           << collapsed_bytes.status().message();
  }
  if (*original_bytes == *collapsed_bytes) {
    return ::testing::AssertionFailure()
           << what
           << ": its two elements encode identically, so reversing them "
              "reorders nothing and the canonical sort goes untested";
  }
  return ::testing::AssertionSuccess();
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

// The offset of `needle` in `bytes`, or `std::string::npos` when it is absent.
// The canonical order is only observable through the emitted bytes, so the one
// test that pins it reads them directly.
std::size_t OffsetOf(const std::vector<std::byte>& bytes,
                     std::string_view needle) {
  for (std::size_t offset = 0; offset + needle.size() <= bytes.size();
       ++offset) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      const auto expected =
          static_cast<std::byte>(static_cast<unsigned char>(needle[index]));
      if (bytes[offset + index] != expected) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return offset;
    }
  }
  return std::string::npos;
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
  EvidenceCase original = MakeOverflowEvidenceCase();
  PopulateSecondMembers(original);
  // Guard before reversing: a one-member collection reverses to itself, so the
  // sort for it would go untested.
  ASSERT_TRUE(EveryReversedCollectionIsPopulated(original));

  // Population is not enough for the two collections whose second element is
  // derived rather than cloned: the pair must also differ by canonical
  // encoding, or reversing it still reorders nothing.
  EXPECT_TRUE(CollapsingSecondElementChangesBytes(
      original,
      [](EvidenceCase& value) {
        for (Path& path : value.paths) {
          if (path.conditions.size() >= 2) {
            path.conditions[1] = path.conditions[0];
          }
        }
      },
      "path conditions"));
  EXPECT_TRUE(CollapsingSecondElementChangesBytes(
      original,
      [](EvidenceCase& value) {
        for (Unknown& unknown : value.unknowns) {
          if (unknown.blocking_ids.size() >= 2) {
            unknown.blocking_ids[1] = unknown.blocking_ids[0];
          }
        }
      },
      "unknown blocking ids"));

  EvidenceCase reversed = original;
  ReverseUnorderedCollections(reversed);

  EXPECT_TRUE(SameCanonicalBytes(original, reversed));

  EvidenceCase left = original;
  EvidenceCase right = reversed;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
}

TEST(EvidenceCanonicalizerTest, VID007IdentityIgnoresUnorderedConstructionL0) {
  EvidenceCase original = MakeValidMinimalEvidenceCase();
  PopulateSecondMembers(original);

  // The minimal case deliberately carries one member of some collections and
  // none of the others: the guard covers what it does carry, because an empty
  // collection has nothing to reverse while a one-member one would make the
  // reversal a no-op. Its causal slice is a single edge, so reversing edges
  // here is vacuous by construction and is guarded by the L1 test instead.
  ASSERT_GE(original.program.analyzer_versions.size(), 2u);
  ASSERT_GE(original.entities.size(), 2u);
  ASSERT_GE(original.provenance.size(), 2u);
  ASSERT_GE(original.paths.size(), 2u);
  ASSERT_GE(original.facts.size(), 2u);
  ASSERT_GE(original.omissions.size(), 2u);

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
  with_stale_id.evidence_id = EvidenceIdentity("stale_evidence_identity");

  EXPECT_TRUE(SameCanonicalBytes(original, with_stale_id));

  EvidenceCase left = original;
  EvidenceCase right = with_stale_id;
  EXPECT_TRUE(SameEvidenceId(&left, &right));
  EXPECT_NE(left.evidence_id, std::optional<core::StableId>(
                                  EvidenceIdentity("stale_evidence_identity")));
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

// --- The sort key is a spelling, not an enumerator value --------------------

// `EntityKind` declares `kFunction` (1) before `kBasicBlock` (5), while the
// spellings run the other way ("basic_block" < "function"), so a group keyed
// on the enumerator value emits these two entities in the opposite order from
// one keyed on the spelling. Enumerators cannot be renumbered at runtime, so
// the property is pinned where it is observable: the emitted bytes of a case
// whose only two entities are of those kinds.
TEST(EvidenceCanonicalizerTest, SortOrderFollowsKindSpellingNotEnumeratorValue) {
  const auto entity_of_kind = [](EntityKind kind, std::string id) {
    Entity entity;
    entity.id = std::move(id);
    entity.kind = kind;
    return entity;
  };

  EvidenceCase value;
  value.entities.push_back(entity_of_kind(EntityKind::kFunction, "E_one"));
  value.entities.push_back(entity_of_kind(EntityKind::kBasicBlock, "E_two"));

  const auto bytes = CanonicalEvidenceBytes(value);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  // Each spelling occurs exactly once in this case, so the two offsets are the
  // positions of the two entity records and nothing else.
  const std::size_t basic_block = OffsetOf(*bytes, "basic_block");
  const std::size_t function = OffsetOf(*bytes, "function");
  ASSERT_NE(basic_block, std::string::npos);
  ASSERT_NE(function, std::string::npos);
  EXPECT_LT(basic_block, function)
      << "the entity group is ordered by enumerator value, not by spelling";

  // The order follows the key alone, never the input: the same two entities
  // declared the other way round produce the same bytes.
  EvidenceCase reversed;
  reversed.entities.push_back(entity_of_kind(EntityKind::kBasicBlock, "E_two"));
  reversed.entities.push_back(entity_of_kind(EntityKind::kFunction, "E_one"));
  EXPECT_TRUE(SameCanonicalBytes(value, reversed));
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
