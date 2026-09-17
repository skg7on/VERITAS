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

// EvidenceProtoTest.cpp — the Protobuf representation boundary, and the
// catalog rows that own it (`REP-002`, the Protobuf branch of `REP-003`,
// `REP-006`, and `REP-008` of the M10B/M10C API-to-Evidence-IR test design
// spec, section 11).
//
//   * REP-002 ProtobufRoundTripsSemanticBytes — the DEM-001 overflow case and
//     the minimal L0 case survive encode/decode with equal canonical bytes and
//     one exact `EvidenceID`. Wire bytes are never the identity.
//   * REP-006 RejectsStaleOrMismatchedEvidenceId — an encoded `evidence_id`
//     that disagrees with the identity recomputed from the decoded case is
//     refused, on the wire and in the domain model. An absent `evidence_id` is
//     not a disagreement: it decodes and is finalized.
//   * REP-008 RejectsMalformedProtobufWithoutPartialCase — truncated, garbage,
//     and empty input, unspecified enums, invalid stable IDs, missing header or
//     program context, and references that dangle after decode are each
//     refused, and a refused decode yields no case at all.
//   * The Protobuf branch of REP-003 AllRepresentationsShareEvidenceId — a
//     case that arrives through Protobuf re-canonicalizes to the identity the
//     domain case carries.
//
// Two properties beyond the catalog rows are pinned here because the codec's
// design depends on them and nothing else would catch their loss:
//
//   * A legal case that expresses an absence — the grammar's optional proof
//     budget, and a hypothesis that declares no confidence — round-trips with
//     that absence intact, and its identity differs from the same case with the
//     member present. A sentinel-based encoding would silently collapse the two.
//   * A wire message whose repeated members are in a non-canonical order
//     decodes into a case with the same canonical bytes and the same
//     `EvidenceID` as the canonical encoding. Protobuf preserves repeated-field
//     order, so the reordering genuinely reaches the decoder.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceProto.h"
#include "veritas/evidence/EvidenceValidator.h"
#include "veritas/evidence/v1/evidence.pb.h"

using namespace veritas;
using namespace veritas::evidence;
using namespace veritas::testing;

namespace v1 = veritas::evidence::v1;

namespace {

// The DEM-001 overflow case with its identity finalized, which is the form
// every round-trip assertion starts from.
EvidenceCase FinalizedOverflowCase() {
  EvidenceCase value = MakeOverflowEvidenceCase();
  const Status status = FinalizeEvidenceIdentity(&value);
  EXPECT_TRUE(status.ok()) << status.message();
  return value;
}

// Serializes a wire message, failing the test rather than returning bytes that
// would make the assertion below vacuous.
std::string Serialize(const v1::EvidenceCase& value) {
  std::string bytes;
  const bool serialized = value.SerializeToString(&bytes);
  EXPECT_TRUE(serialized);
  return bytes;
}

// Reverses a repeated field in place. `SwapElements` is used rather than a
// standard algorithm so the mutation is exact for both a repeated message field
// and a repeated scalar one.
template <typename Repeated>
void ReverseRepeated(Repeated* values) {
  for (int left = 0, right = values->size() - 1; left < right;
       ++left, --right) {
    values->SwapElements(left, right);
  }
}

// Asserts a decode failed for the right reason: no case, and an invalid
// argument rather than an internal error.
void ExpectInvalidArgument(const StatusOr<EvidenceCase>& decoded) {
  ASSERT_FALSE(decoded.ok()) << "the decode should have been refused";
  EXPECT_EQ(decoded.status().code(), StatusCode::kInvalidArgument)
      << decoded.status().message();
}

// Canonical bytes are not printable, so a mismatch is reported by the offset
// where the two encodings first differ — which is what a representation bug
// looks like.
::testing::AssertionResult SameCanonicalBytes(const EvidenceCase& left,
                                              const EvidenceCase& right) {
  const auto left_bytes = CanonicalEvidenceBytes(left);
  if (!left_bytes.ok()) {
    return ::testing::AssertionFailure()
           << "canonicalization failed: " << left_bytes.status().message();
  }
  const auto right_bytes = CanonicalEvidenceBytes(right);
  if (!right_bytes.ok()) {
    return ::testing::AssertionFailure()
           << "canonicalization failed: " << right_bytes.status().message();
  }
  if (*left_bytes == *right_bytes) {
    return ::testing::AssertionSuccess();
  }
  std::size_t offset = 0;
  while (offset < left_bytes->size() && offset < right_bytes->size() &&
         (*left_bytes)[offset] == (*right_bytes)[offset]) {
    ++offset;
  }
  return ::testing::AssertionFailure()
         << "canonical bytes differ at offset " << offset << " of ("
         << left_bytes->size() << ", " << right_bytes->size() << ")";
}

}  // namespace

TEST(EvidenceProtoTest, RoundTripPreservesCanonicalIdentity) {
  auto input = MakeOverflowEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());
  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto output = DecodeEvidenceProto(*bytes);
  ASSERT_TRUE(output.ok()) << output.status().message();
  auto output_id = ComputeEvidenceId(*output);
  ASSERT_TRUE(output_id.ok()) << output_id.status().message();
  EXPECT_EQ(*output_id, input.evidence_id);
}

// Every member of every fixture case survives the round trip, whole: the same
// records, in the same order, with the same field values — not merely the same
// identity. A dropped or defaulted field would change the canonical bytes in
// most cases, but a substitution that happens to canonicalize identically (an
// absent optional member, an ordering the canonicalizer would repair anyway)
// is caught here and nowhere else.
TEST(EvidenceProtoTest, RoundTripReproducesEveryFixtureCase) {
  std::vector<EvidenceCase> cases = {MakeValidMinimalEvidenceCase(),
                                     MakeOverflowEvidenceCase()};
  for (EvidenceCase& value : cases) {
    ASSERT_TRUE(FinalizeEvidenceIdentity(&value).ok());
    auto bytes = EncodeEvidenceProto(value);
    ASSERT_TRUE(bytes.ok()) << bytes.status().message();
    auto decoded = DecodeEvidenceProto(*bytes);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();

    EXPECT_TRUE(*decoded == value);
    EXPECT_TRUE(SameCanonicalBytes(value, *decoded));
  }
}

// The grammar's proof budget is optional. A case without one must stay
// representable: absence is carried as field absence and decodes back to an
// absent budget, not to an invalid enum or a rejection.
TEST(EvidenceProtoTest, AbsentProofBudgetSurvivesRoundTrip) {
  EvidenceCase input = MakeOverflowEvidenceCase();
  ASSERT_EQ(input.proof_obligations.size(), 1u);
  input.proof_obligations[0].budget = Expression{};
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());

  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto decoded = DecodeEvidenceProto(*bytes);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();

  ASSERT_EQ(decoded->proof_obligations.size(), 1u);
  EXPECT_EQ(decoded->proof_obligations[0].budget.kind,
            Expression::Kind::kUnspecified);
  EXPECT_TRUE(decoded->proof_obligations[0].budget.text.empty());
  EXPECT_EQ(decoded->proof_obligations[0].budget.integer, 0);
  EXPECT_FALSE(decoded->proof_obligations[0].budget.boolean);
  EXPECT_TRUE(decoded->proof_obligations[0].budget.operands.empty());
  EXPECT_TRUE(*decoded == input);

  // An absent budget is a semantic difference, so the round trip above is not
  // vacuous: the same case with its budget is a different identity.
  const EvidenceCase with_budget = FinalizedOverflowCase();
  EXPECT_FALSE(SameCanonicalBytes(with_budget, input));
  ASSERT_TRUE(with_budget.evidence_id.has_value());
  auto input_id = ComputeEvidenceId(input);
  ASSERT_TRUE(input_id.ok()) << input_id.status().message();
  EXPECT_FALSE(*input_id == *with_budget.evidence_id);
}

// `Confidence::kUnspecified` is a legal state for a hypothesis and is not a
// value any serializer emits. The codec carries it as an absent field, so the
// model's unspecified confidence and an explicitly encoded zero are told apart:
// one round-trips, the other is rejected.
TEST(EvidenceProtoTest, AbsentHypothesisConfidenceSurvivesRoundTrip) {
  EvidenceCase input = MakeOverflowEvidenceCase();
  ASSERT_EQ(input.hypotheses.size(), 1u);
  input.hypotheses[0].confidence = Confidence::kUnspecified;
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());

  auto proto = ToEvidenceProto(input);
  ASSERT_TRUE(proto.ok()) << proto.status().message();
  EXPECT_FALSE(proto->hypotheses(0).has_confidence());

  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto decoded = DecodeEvidenceProto(*bytes);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  ASSERT_EQ(decoded->hypotheses.size(), 1u);
  EXPECT_EQ(decoded->hypotheses[0].confidence, Confidence::kUnspecified);
  EXPECT_TRUE(*decoded == input);
}

// Every enum position the model declares must carry a value: zero is the
// invalid default proto3 forces on the schema, and a decoder that accepted it
// would hand the validator a case no serializer could have produced.
TEST(EvidenceProtoTest, RejectsUnspecifiedEnums) {
  auto proto = ToEvidenceProto(FinalizedOverflowCase());
  ASSERT_TRUE(proto.ok()) << proto.status().message();

  {
    v1::EvidenceCase mutated = *proto;
    mutated.set_level(v1::EVIDENCE_LEVEL_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.set_verification_state(v1::VERIFICATION_STATE_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_entities(0)->set_kind(v1::ENTITY_KIND_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_edges(0)->set_epistemic(v1::EPISTEMIC_STATE_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_facts(0)->set_confidence(v1::CONFIDENCE_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_paths(0)->set_feasibility(v1::FEASIBILITY_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_unknowns(0)->set_reason_code(
        v1::UNKNOWN_REASON_CODE_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_proof_obligations(0)->set_goal_kind(
        v1::PROOF_GOAL_KIND_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_dependencies(0)->set_kind(
        v1::DEPENDENCY_KIND_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_primary_claim()->set_kind(v1::CLAIM_KIND_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  // The explicit-presence field is the interesting one: clearing it is legal
  // absence, explicitly setting it to zero is not.
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_hypotheses(0)->set_confidence(v1::CONFIDENCE_UNSPECIFIED);
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
}

// REP-008's malformed input: a decoder that returned a partially initialized
// case, or that accepted a truncated buffer, would be able to publish a case
// no encoder produced.
TEST(EvidenceProtoTest, RejectsMalformedBytes) {
  const EvidenceCase input = FinalizedOverflowCase();
  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  ASSERT_GT(bytes->size(), 16u);

  ExpectInvalidArgument(
      DecodeEvidenceProto(std::string_view(*bytes).substr(0, bytes->size() / 2)));
  ExpectInvalidArgument(
      DecodeEvidenceProto(std::string_view(*bytes).substr(0, 1)));
  ExpectInvalidArgument(DecodeEvidenceProto(std::string_view()));
  ExpectInvalidArgument(DecodeEvidenceProto(std::string_view("\xff\xff\xff\xff", 4)));
}

// A case is bound to a schema and a program context; neither may be absent, and
// a decoded case missing either is refused rather than repaired.
TEST(EvidenceProtoTest, RejectsMissingSchemaAndProgramContext) {
  auto proto = ToEvidenceProto(FinalizedOverflowCase());
  ASSERT_TRUE(proto.ok()) << proto.status().message();

  {
    v1::EvidenceCase mutated = *proto;
    mutated.clear_schema_version();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.set_schema_version("eir.v2");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.clear_program();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_program()->clear_analysis_run_id();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_program()->clear_analysis_configuration_id();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
}

// A wire message can name a member that does not exist. Validation runs on the
// decoded case, so the reference is caught after the conversion rather than
// silently resolved to nothing.
TEST(EvidenceProtoTest, RejectsDanglingReferencesAfterDecode) {
  auto proto = ToEvidenceProto(FinalizedOverflowCase());
  ASSERT_TRUE(proto.ok()) << proto.status().message();
  ASSERT_GT(proto->entities_size(), 0);

  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_primary_claim()->set_subject("E_never_declared");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_edges(0)->set_to("E_never_declared");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    // A derived fact that loses its provenance has hidden its derivation.
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_facts(0)->set_derived(true);
    mutated.mutable_facts(0)->clear_provenance_id();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
}

// A globally resolvable identity is a canonical stable-ID string. Anything else
// on the wire is a rejection, never a repaired or ignored value.
TEST(EvidenceProtoTest, RejectsInvalidStableIds) {
  auto proto = ToEvidenceProto(FinalizedOverflowCase());
  ASSERT_TRUE(proto.ok()) << proto.status().message();

  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_dependencies(0)->set_stable_id("not-a-stable-id");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_dependencies(0)->clear_stable_id();
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_entities(0)->set_stable_id("step:md5:00");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_summaries(0)->set_summary_id("funcbody:sha256:");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.mutable_program()->set_analysis_run_id("analysisrun:nope:00");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
}

// REP-006: an encoded identity that disagrees with the content it claims to
// address is refused, in both directions, and never silently rewritten into the
// correct one.
TEST(EvidenceProtoTest, RejectsStaleOrMismatchedEvidenceId) {
  const EvidenceCase input = FinalizedOverflowCase();
  auto proto = ToEvidenceProto(input);
  ASSERT_TRUE(proto.ok()) << proto.status().message();
  ASSERT_TRUE(input.evidence_id.has_value());

  // A different case, so the stale identity is a real, well-formed
  // `evidence:sha256:...` value and not merely a malformed string.
  EvidenceCase other = MakeOverflowEvidenceCase();
  other.primary_claim.description += " (a different case)";
  ASSERT_TRUE(FinalizeEvidenceIdentity(&other).ok());
  ASSERT_TRUE(other.evidence_id.has_value());
  ASSERT_FALSE(*other.evidence_id == *input.evidence_id);

  {
    v1::EvidenceCase mutated = *proto;
    mutated.set_evidence_id(core::ToString(*other.evidence_id));
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    v1::EvidenceCase mutated = *proto;
    mutated.set_evidence_id("evidence:sha256:not-a-digest");
    ExpectInvalidArgument(DecodeEvidenceProto(Serialize(mutated)));
  }
  {
    // The domain-side half of the same rule: a case carrying another case's
    // identity is not serialized either.
    EvidenceCase tampered = input;
    tampered.evidence_id = other.evidence_id;
    auto encoded = EncodeEvidenceProto(tampered);
    ASSERT_FALSE(encoded.ok());
    EXPECT_EQ(encoded.status().code(), StatusCode::kInvalidArgument);
    auto converted = ToEvidenceProto(tampered);
    ASSERT_FALSE(converted.ok());
    EXPECT_EQ(converted.status().code(), StatusCode::kInvalidArgument);
  }
}

// An empty `evidence_id` is an absent identity, not a disagreement: the case
// decodes, and the recomputation fills the identity in.
TEST(EvidenceProtoTest, AbsentEvidenceIdDecodesAndFinalizes) {
  const EvidenceCase input = FinalizedOverflowCase();
  auto proto = ToEvidenceProto(input);
  ASSERT_TRUE(proto.ok()) << proto.status().message();

  v1::EvidenceCase mutated = *proto;
  mutated.clear_evidence_id();
  auto decoded = DecodeEvidenceProto(Serialize(mutated));
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  ASSERT_TRUE(decoded->evidence_id.has_value());
  EXPECT_EQ(*decoded->evidence_id, *input.evidence_id);
}

// The Protobuf branch of REP-003: a case that arrives through Protobuf reports
// the identity the domain case carries, because identity is recomputed from the
// canonical encoding rather than taken from the representation.
TEST(EvidenceProtoTest, DecodedCaseSharesTheCanonicalIdentity) {
  const EvidenceCase input = FinalizedOverflowCase();
  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto decoded = DecodeEvidenceProto(*bytes);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();

  ASSERT_TRUE(decoded->evidence_id.has_value());
  ASSERT_TRUE(input.evidence_id.has_value());
  EXPECT_EQ(*decoded->evidence_id, *input.evidence_id);
  auto recomputed = ComputeEvidenceId(*decoded);
  ASSERT_TRUE(recomputed.ok()) << recomputed.status().message();
  EXPECT_EQ(*recomputed, *input.evidence_id);
  // The wire bytes are not the identity: they are not even stable across the
  // two representations of the same case.
  EXPECT_NE(*bytes, core::ToString(*input.evidence_id));
}

// A legal wire message in a non-canonical member order decodes into a case with
// the canonical case's bytes and identity. The canonicalizer owns ordering;
// the codec must not, or two encodings of one case would have two identities.
TEST(EvidenceProtoTest, NonCanonicalWireOrderDecodesToCanonicalIdentity) {
  const EvidenceCase input = FinalizedOverflowCase();
  auto proto = ToEvidenceProto(input);
  ASSERT_TRUE(proto.ok()) << proto.status().message();

  v1::EvidenceCase reordered = *proto;
  ReverseRepeated(reordered.mutable_entities());
  ReverseRepeated(reordered.mutable_edges());
  ReverseRepeated(reordered.mutable_paths());
  ReverseRepeated(reordered.mutable_facts());
  ReverseRepeated(reordered.mutable_assumptions());
  ReverseRepeated(reordered.mutable_hypotheses());
  ReverseRepeated(reordered.mutable_unknowns());
  ReverseRepeated(reordered.mutable_constraints());
  ReverseRepeated(reordered.mutable_provenance());
  ReverseRepeated(reordered.mutable_proof_obligations());
  ReverseRepeated(reordered.mutable_summaries());
  ReverseRepeated(reordered.mutable_dependencies());
  ReverseRepeated(reordered.mutable_omissions());
  ReverseRepeated(reordered.mutable_program()->mutable_analyzer_versions());
  ReverseRepeated(reordered.mutable_paths(0)->mutable_conditions());
  ReverseRepeated(
      reordered.mutable_proof_obligations(0)->mutable_verifier_kinds());
  ReverseRepeated(reordered.mutable_provenance(0)->mutable_input_fact_ids());
  ReverseRepeated(reordered.mutable_summaries(0)->mutable_components());
  ReverseRepeated(reordered.mutable_unknowns(0)->mutable_blocking_ids());

  const std::string canonical_wire = Serialize(*proto);
  const std::string reordered_wire = Serialize(reordered);
  // The reordering reaches the decoder; Protobuf preserves repeated-field
  // order on the wire and the codec preserves it into the model.
  EXPECT_NE(canonical_wire, reordered_wire);

  auto decoded = DecodeEvidenceProto(reordered_wire);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();

  ASSERT_FALSE(input.entities.empty());
  ASSERT_FALSE(decoded->entities.empty());
  EXPECT_NE(input.entities[0].id, decoded->entities[0].id);

  EXPECT_TRUE(SameCanonicalBytes(input, *decoded));

  auto decoded_id = ComputeEvidenceId(*decoded);
  ASSERT_TRUE(decoded_id.ok()) << decoded_id.status().message();
  EXPECT_EQ(*decoded_id, *input.evidence_id);
}
