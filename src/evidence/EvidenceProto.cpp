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

// EvidenceProto.cpp — the explicit `eir.v1` model/Protobuf conversion.
//
// One function per record and per enum, in both directions, plus the four
// public entry points. The conversion is written out by hand.
//
// Exhaustiveness over the semantic model is maintained by hand, not by the
// compiler: C++20 has no reflection, so there is no static check that every
// field of every record in `EvidenceCase.h` is carried here. It is enforced by
// review and by the round-trip tests. Adding a field to a record in
// `EvidenceCase.h` therefore requires a matching change in this file plus a
// fixture or test that populates the new field; without both, the field is
// dropped silently and `EvidenceID` changes with no failing test to say so.
//
// Between the two directions, four rules hold together:
//
//   * Presence. An empty string is an absent identifier, and an absent message
//     field is an absent optional expression; both directions agree, so a case
//     with no proof budget and a case with a budget through `Expression` both
//     round-trip byte for byte. `Hypothesis::confidence` declares explicit
//     presence because the domain distinguishes an unspecified confidence from
//     an absent one; the model's own `kUnspecified` is carried as absence.
//   * Rejection. A zero-valued enum in a position the domain requires is
//     `InvalidArgument`, named by the field path that carried it. Protobuf's
//     unknown-field set is not consulted: an enum value the schema does not
//     declare arrives as the zero default and is rejected here.
//   * Stable IDs. Every canonical stable-ID string is re-parsed with
//     `core::ParseStableId`, so a wire value that is not a canonical ID never
//     reaches the model.
//   * Order. Wire order is preserved into the model, and the model's canonical
//     order is applied afterwards by the canonicalizer. Nothing here sorts by
//     an enum number or a Protobuf value, so enum numbering cannot reach
//     `EvidenceID`.

#include "veritas/evidence/EvidenceProto.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "veritas/core/Ids.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceValidator.h"

namespace veritas::evidence {
namespace {

namespace v1 = veritas::evidence::v1;

// --- Diagnostic paths -------------------------------------------------------

// "owner.member", so a rejection names where in the case it happened. The owner
// is the member's path through the message ("entity 'E_memcpy'", "entities[3]").
std::string At(std::string_view owner, std::string_view member) {
  std::string out;
  out.reserve(owner.size() + member.size() + 1);
  out.append(owner);
  if (!owner.empty()) {
    out.push_back('.');
  }
  out.append(member);
  return out;
}

// A named member's own owner string: "entity 'E_memcpy'".
std::string Owned(std::string_view kind, const std::string& id) {
  std::string out;
  out.reserve(kind.size() + id.size() + 3);
  out.append(kind);
  out.append(" '");
  out.append(id);
  out.push_back('\'');
  return out;
}

// A repeated member's owner string: "entities[3]".
std::string Indexed(std::string_view collection, int index) {
  std::string out;
  out.append(collection);
  out.push_back('[');
  out.append(std::to_string(index));
  out.push_back(']');
  return out;
}

// --- Enum mapping -----------------------------------------------------------

// One domain enumerator and the wire value it is carried as. The pairs are
// written out rather than derived, so a domain enumerator inserted into the
// middle of a family cannot shift a wire value, and a wire value cannot be
// renumbered without an edit here.
template <typename DomainEnum, typename ProtoEnum>
struct EnumMapping {
  DomainEnum domain;
  ProtoEnum proto;
};

// Maps a wire value onto its domain enumerator. Zero is the sentinel proto3
// forces on every enum and is never a semantic value for the families this
// codec carries positionally, so it is rejected with the field path that
// carried it; an unrecognized non-zero value is rejected too rather than
// coerced to the invalid default.
template <typename DomainEnum, typename ProtoEnum, std::size_t N>
StatusOr<DomainEnum> DecodeEnum(
    ProtoEnum value, const EnumMapping<DomainEnum, ProtoEnum> (&table)[N],
    const std::string& field) {
  if (static_cast<int>(value) == 0) {
    return Status::InvalidArgument(field + " is unspecified");
  }
  for (std::size_t index = 0; index < N; ++index) {
    if (table[index].proto == value) {
      return table[index].domain;
    }
  }
  return Status::InvalidArgument(field + " carries an unrecognized enum value");
}

// Maps a domain enumerator onto its wire value. `kUnspecified` has no entry and
// no wire value: a serializer never emits it, so reaching this with one is a
// contract violation rather than an encoding decision.
template <typename DomainEnum, typename ProtoEnum, std::size_t N>
StatusOr<ProtoEnum> EncodeEnum(
    DomainEnum value, const EnumMapping<DomainEnum, ProtoEnum> (&table)[N],
    const std::string& field) {
  for (std::size_t index = 0; index < N; ++index) {
    if (table[index].domain == value) {
      return table[index].proto;
    }
  }
  return Status::InvalidArgument(field + " has no valid wire value");
}

constexpr EnumMapping<EvidenceLevel, v1::EvidenceLevel> kEvidenceLevels[] = {
    {EvidenceLevel::kL0, v1::EVIDENCE_LEVEL_L0},
    {EvidenceLevel::kL1, v1::EVIDENCE_LEVEL_L1},
    {EvidenceLevel::kL2, v1::EVIDENCE_LEVEL_L2},
};

constexpr EnumMapping<VerificationState, v1::VerificationState>
    kVerificationStates[] = {
        {VerificationState::kUnreviewed, v1::VERIFICATION_STATE_UNREVIEWED},
        {VerificationState::kPossibleDefect,
         v1::VERIFICATION_STATE_POSSIBLE_DEFECT},
        {VerificationState::kLikelyDefect, v1::VERIFICATION_STATE_LIKELY_DEFECT},
        {VerificationState::kVerifiedDefect,
         v1::VERIFICATION_STATE_VERIFIED_DEFECT},
        {VerificationState::kLikelyFalsePositive,
         v1::VERIFICATION_STATE_LIKELY_FALSE_POSITIVE},
        {VerificationState::kVerifiedSafe, v1::VERIFICATION_STATE_VERIFIED_SAFE},
        {VerificationState::kInconclusive, v1::VERIFICATION_STATE_INCONCLUSIVE},
};

constexpr EnumMapping<EntityKind, v1::EntityKind> kEntityKinds[] = {
    {EntityKind::kFunction, v1::ENTITY_KIND_FUNCTION},
    {EntityKind::kCallSite, v1::ENTITY_KIND_CALL_SITE},
    {EntityKind::kValue, v1::ENTITY_KIND_VALUE},
    {EntityKind::kMemoryObject, v1::ENTITY_KIND_MEMORY_OBJECT},
    {EntityKind::kBasicBlock, v1::ENTITY_KIND_BASIC_BLOCK},
};

constexpr EnumMapping<RelationKind, v1::RelationKind> kRelationKinds[] = {
    {RelationKind::kCalls, v1::RELATION_KIND_CALLS},
    {RelationKind::kFlowsTo, v1::RELATION_KIND_FLOWS_TO},
    {RelationKind::kReads, v1::RELATION_KIND_READS},
    {RelationKind::kWrites, v1::RELATION_KIND_WRITES},
    {RelationKind::kDominates, v1::RELATION_KIND_DOMINATES},
    {RelationKind::kMayAlias, v1::RELATION_KIND_MAY_ALIAS},
};

constexpr EnumMapping<PathKind, v1::PathKind> kPathKinds[] = {
    {PathKind::kCall, v1::PATH_KIND_CALL},
    {PathKind::kValueFlow, v1::PATH_KIND_VALUE_FLOW},
    {PathKind::kControl, v1::PATH_KIND_CONTROL},
};

constexpr EnumMapping<EpistemicState, v1::EpistemicState> kEpistemicStates[] = {
    {EpistemicState::kMust, v1::EPISTEMIC_STATE_MUST},
    {EpistemicState::kMay, v1::EPISTEMIC_STATE_MAY},
    {EpistemicState::kMustNot, v1::EPISTEMIC_STATE_MUST_NOT},
    {EpistemicState::kInferred, v1::EPISTEMIC_STATE_INFERRED},
    {EpistemicState::kAssumed, v1::EPISTEMIC_STATE_ASSUMED},
    {EpistemicState::kUnknown, v1::EPISTEMIC_STATE_UNKNOWN},
};

constexpr EnumMapping<Confidence, v1::Confidence> kConfidences[] = {
    {Confidence::kExact, v1::CONFIDENCE_EXACT},
    {Confidence::kHigh, v1::CONFIDENCE_HIGH},
    {Confidence::kMedium, v1::CONFIDENCE_MEDIUM},
    {Confidence::kLow, v1::CONFIDENCE_LOW},
    {Confidence::kUnknown, v1::CONFIDENCE_UNKNOWN},
};

constexpr EnumMapping<ProofStatus, v1::ProofStatus> kProofStatuses[] = {
    {ProofStatus::kPending, v1::PROOF_STATUS_PENDING},
    {ProofStatus::kProved, v1::PROOF_STATUS_PROVED},
    {ProofStatus::kRefuted, v1::PROOF_STATUS_REFUTED},
    {ProofStatus::kUnknown, v1::PROOF_STATUS_UNKNOWN},
    {ProofStatus::kTimeout, v1::PROOF_STATUS_TIMEOUT},
    {ProofStatus::kUnsupported, v1::PROOF_STATUS_UNSUPPORTED},
};

constexpr EnumMapping<DependencyKind, v1::DependencyKind> kDependencyKinds[] = {
    {DependencyKind::kSummary, v1::DEPENDENCY_KIND_SUMMARY},
    {DependencyKind::kFact, v1::DEPENDENCY_KIND_FACT},
    {DependencyKind::kTypeLayout, v1::DEPENDENCY_KIND_TYPE_LAYOUT},
    {DependencyKind::kConfiguration, v1::DEPENDENCY_KIND_CONFIGURATION},
    {DependencyKind::kSpecification, v1::DEPENDENCY_KIND_SPECIFICATION},
};

constexpr EnumMapping<Feasibility, v1::Feasibility> kFeasibilities[] = {
    {Feasibility::kProvedFeasible, v1::FEASIBILITY_PROVED_FEASIBLE},
    {Feasibility::kSat, v1::FEASIBILITY_SAT},
    {Feasibility::kMaybe, v1::FEASIBILITY_MAYBE},
    {Feasibility::kUntested, v1::FEASIBILITY_UNTESTED},
    {Feasibility::kUnsat, v1::FEASIBILITY_UNSAT},
    {Feasibility::kProvedInfeasible, v1::FEASIBILITY_PROVED_INFEASIBLE},
    {Feasibility::kUnknown, v1::FEASIBILITY_UNKNOWN},
};

constexpr EnumMapping<ProofGoalKind, v1::ProofGoalKind> kProofGoalKinds[] = {
    {ProofGoalKind::kProve, v1::PROOF_GOAL_KIND_PROVE},
    {ProofGoalKind::kRefute, v1::PROOF_GOAL_KIND_REFUTE},
    {ProofGoalKind::kCheck, v1::PROOF_GOAL_KIND_CHECK},
};

constexpr EnumMapping<UnknownReasonCode, v1::UnknownReasonCode>
    kUnknownReasonCodes[] = {
        {UnknownReasonCode::kUnresolvedCall,
         v1::UNKNOWN_REASON_CODE_UNRESOLVED_CALL},
        {UnknownReasonCode::kUnknownAlias,
         v1::UNKNOWN_REASON_CODE_UNKNOWN_ALIAS},
        {UnknownReasonCode::kExternalFunction,
         v1::UNKNOWN_REASON_CODE_EXTERNAL_FUNCTION},
        {UnknownReasonCode::kMissingSpecification,
         v1::UNKNOWN_REASON_CODE_MISSING_SPECIFICATION},
        {UnknownReasonCode::kAnalysisTimeout,
         v1::UNKNOWN_REASON_CODE_ANALYSIS_TIMEOUT},
        {UnknownReasonCode::kStateExplosion,
         v1::UNKNOWN_REASON_CODE_STATE_EXPLOSION},
        {UnknownReasonCode::kUnsupportedLanguageFeature,
         v1::UNKNOWN_REASON_CODE_UNSUPPORTED_LANGUAGE_FEATURE},
        {UnknownReasonCode::kInlineAssembly,
         v1::UNKNOWN_REASON_CODE_INLINE_ASSEMBLY},
        {UnknownReasonCode::kDynamicLoading,
         v1::UNKNOWN_REASON_CODE_DYNAMIC_LOADING},
        {UnknownReasonCode::kUnknownBuildConfiguration,
         v1::UNKNOWN_REASON_CODE_UNKNOWN_BUILD_CONFIGURATION},
};

constexpr EnumMapping<ClaimKind, v1::ClaimKind> kClaimKinds[] = {
    {ClaimKind::kBufferOverflow, v1::CLAIM_KIND_BUFFER_OVERFLOW},
};

constexpr EnumMapping<Severity, v1::Severity> kSeverities[] = {
    {Severity::kCritical, v1::SEVERITY_CRITICAL},
    {Severity::kHigh, v1::SEVERITY_HIGH},
    {Severity::kMedium, v1::SEVERITY_MEDIUM},
    {Severity::kLow, v1::SEVERITY_LOW},
    {Severity::kInfo, v1::SEVERITY_INFO},
};

constexpr EnumMapping<Expression::Kind, v1::ExpressionKind> kExpressionKinds[] =
    {
        {Expression::Kind::kBool, v1::EXPRESSION_KIND_BOOL},
        {Expression::Kind::kInteger, v1::EXPRESSION_KIND_INTEGER},
        {Expression::Kind::kString, v1::EXPRESSION_KIND_STRING},
        {Expression::Kind::kSymbol, v1::EXPRESSION_KIND_SYMBOL},
        {Expression::Kind::kReference, v1::EXPRESSION_KIND_REFERENCE},
        {Expression::Kind::kCall, v1::EXPRESSION_KIND_CALL},
        {Expression::Kind::kNot, v1::EXPRESSION_KIND_NOT},
        {Expression::Kind::kCompare, v1::EXPRESSION_KIND_COMPARE},
        {Expression::Kind::kAnd, v1::EXPRESSION_KIND_AND},
        {Expression::Kind::kOr, v1::EXPRESSION_KIND_OR},
        {Expression::Kind::kImplies, v1::EXPRESSION_KIND_IMPLIES},
        {Expression::Kind::kForAll, v1::EXPRESSION_KIND_FOR_ALL},
        {Expression::Kind::kExists, v1::EXPRESSION_KIND_EXISTS},
};

// --- Stable IDs -------------------------------------------------------------

// Parses a canonical stable-ID string that must be present and canonical.
StatusOr<core::StableId> RequiredStableId(const std::string& field,
                                          const std::string& text) {
  if (text.empty()) {
    return Status::InvalidArgument(field + " is absent");
  }
  auto id = core::ParseStableId(text);
  if (!id.ok()) {
    return Status::InvalidArgument(
        field + " is not a canonical stable ID: '" + text + "'");
  }
  return *id;
}

// Parses a canonical stable-ID string where the empty string is absence.
StatusOr<std::optional<core::StableId>> OptionalStableId(
    const std::string& field, const std::string& text) {
  if (text.empty()) {
    return std::optional<core::StableId>();
  }
  auto id = core::ParseStableId(text);
  if (!id.ok()) {
    return Status::InvalidArgument(
        field + " is not a canonical stable ID: '" + text + "'");
  }
  return std::optional<core::StableId>(*id);
}

// --- Expressions ------------------------------------------------------------

// The model's only recursive record. Wire nesting is bounded by the parser's
// recursion limit, and a domain expression is bounded by the validator's depth
// limit, which every encode path runs before this one.
Expression DecodeExpression(const v1::Expression& value) {
  Expression out;
  // The zero value and any value the schema does not declare both arrive as
  // `EXPRESSION_KIND_UNSPECIFIED`: a proto3 enum is open, so an unrecognized
  // number is kept in the unknown-field set and the field reads back as its
  // default. An absent expression is therefore "unspecified" here, and the
  // validator decides whether the position that carried it required one.
  out.kind = Expression::Kind::kUnspecified;
  for (const auto& entry : kExpressionKinds) {
    if (entry.proto == value.kind()) {
      out.kind = entry.domain;
      break;
    }
  }
  out.text = value.text();
  out.integer = value.integer();
  out.boolean = value.boolean();
  for (const v1::Expression& operand : value.operands()) {
    out.operands.push_back(DecodeExpression(operand));
  }
  return out;
}

v1::Expression EncodeExpression(const Expression& value) {
  v1::Expression out;
  for (const auto& entry : kExpressionKinds) {
    if (entry.domain == value.kind) {
      out.set_kind(entry.proto);
      break;
    }
  }
  out.set_text(value.text);
  out.set_integer(value.integer);
  out.set_boolean(value.boolean);
  for (const Expression& operand : value.operands) {
    *out.add_operands() = EncodeExpression(operand);
  }
  return out;
}

// True when an optional expression carries anything at all. Only a wholly
// default expression is encoded as an absent field; an expression that carries
// residual content with an unspecified kind is still carried, because the
// canonical encoding distinguishes the two and identity must not depend on
// which representation a case travelled through.
bool IsPresent(const Expression& value) {
  return value.kind != Expression::Kind::kUnspecified || !value.text.empty() ||
         value.integer != 0 || value.boolean || !value.operands.empty();
}

// --- Program identity -------------------------------------------------------

StatusOr<ProgramBinding> DecodeProgramBinding(const v1::ProgramBinding& value) {
  ProgramBinding out;
  out.repository_id = value.repository_id();
  out.revision_id = value.revision_id();
  out.build_variant_id = value.build_variant_id();
  out.target_triple = value.target_triple();
  out.analysis_configuration_id = value.analysis_configuration_id();
  out.type_layout_id = value.type_layout_id();
  auto run_id =
      OptionalStableId("program.analysis_run_id", value.analysis_run_id());
  if (!run_id.ok()) {
    return run_id.status();
  }
  out.analysis_run_id = std::move(*run_id);
  for (const v1::AnalyzerVersion& version : value.analyzer_versions()) {
    AnalyzerVersion analyzer;
    analyzer.producer = version.producer();
    analyzer.version = version.version();
    analyzer.configuration = version.configuration();
    out.analyzer_versions.push_back(std::move(analyzer));
  }
  return out;
}

Status EncodeProgramBinding(const ProgramBinding& value,
                            v1::ProgramBinding* out) {
  out->set_repository_id(value.repository_id);
  out->set_revision_id(value.revision_id);
  out->set_build_variant_id(value.build_variant_id);
  out->set_target_triple(value.target_triple);
  out->set_analysis_configuration_id(value.analysis_configuration_id);
  out->set_type_layout_id(value.type_layout_id);
  if (value.analysis_run_id.has_value()) {
    out->set_analysis_run_id(core::ToString(*value.analysis_run_id));
  }
  for (const AnalyzerVersion& version : value.analyzer_versions) {
    v1::AnalyzerVersion* analyzer = out->add_analyzer_versions();
    analyzer->set_producer(version.producer);
    analyzer->set_version(version.version);
    analyzer->set_configuration(version.configuration);
  }
  return Status::Ok();
}

// --- Graph members ----------------------------------------------------------

StatusOr<Entity> DecodeEntity(const v1::Entity& value,
                              const std::string& owner) {
  Entity out;
  out.id = value.id();
  auto kind = DecodeEnum(value.kind(), kEntityKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out.kind = *kind;
  auto stable_id = OptionalStableId(At(owner, "stable_id"), value.stable_id());
  if (!stable_id.ok()) {
    return stable_id.status();
  }
  out.stable_id = std::move(*stable_id);
  for (const auto& property : value.properties()) {
    out.properties.emplace(property.first, DecodeExpression(property.second));
  }
  return out;
}

Status EncodeEntity(const Entity& value, v1::Entity* out) {
  const std::string owner = Owned("entity", value.id);
  out->set_id(value.id);
  auto kind = EncodeEnum(value.kind, kEntityKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out->set_kind(*kind);
  if (value.stable_id.has_value()) {
    out->set_stable_id(core::ToString(*value.stable_id));
  }
  for (const auto& property : value.properties) {
    (*out->mutable_properties())[property.first] = EncodeExpression(property.second);
  }
  return Status::Ok();
}

StatusOr<Edge> DecodeEdge(const v1::Edge& value, const std::string& owner) {
  Edge out;
  out.id = value.id();
  auto kind = DecodeEnum(value.kind(), kRelationKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out.kind = *kind;
  auto epistemic =
      DecodeEnum(value.epistemic(), kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out.epistemic = *epistemic;
  out.from = value.from();
  out.to = value.to();
  out.provenance_id = value.provenance_id();
  out.summarized_by = value.summarized_by();
  out.expandable = value.expandable();
  return out;
}

Status EncodeEdge(const Edge& value, v1::Edge* out) {
  const std::string owner = Owned("edge", value.id);
  out->set_id(value.id);
  out->set_from(value.from);
  out->set_to(value.to);
  auto kind = EncodeEnum(value.kind, kRelationKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out->set_kind(*kind);
  auto epistemic =
      EncodeEnum(value.epistemic, kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out->set_epistemic(*epistemic);
  out->set_provenance_id(value.provenance_id);
  out->set_summarized_by(value.summarized_by);
  out->set_expandable(value.expandable);
  return Status::Ok();
}

StatusOr<Path> DecodePath(const v1::Path& value, const std::string& owner) {
  Path out;
  out.id = value.id();
  auto kind = DecodeEnum(value.kind(), kPathKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out.kind = *kind;
  auto feasibility =
      DecodeEnum(value.feasibility(), kFeasibilities, At(owner, "feasibility"));
  if (!feasibility.ok()) {
    return feasibility.status();
  }
  out.feasibility = *feasibility;
  for (const std::string& segment : value.entity_ids()) {
    out.entity_ids.push_back(segment);
  }
  for (const v1::Expression& condition : value.conditions()) {
    out.conditions.push_back(DecodeExpression(condition));
  }
  out.provenance_id = value.provenance_id();
  return out;
}

Status EncodePath(const Path& value, v1::Path* out) {
  const std::string owner = Owned("path", value.id);
  out->set_id(value.id);
  auto kind = EncodeEnum(value.kind, kPathKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out->set_kind(*kind);
  for (const std::string& segment : value.entity_ids) {
    out->add_entity_ids(segment);
  }
  for (const Expression& condition : value.conditions) {
    *out->add_conditions() = EncodeExpression(condition);
  }
  auto feasibility =
      EncodeEnum(value.feasibility, kFeasibilities, At(owner, "feasibility"));
  if (!feasibility.ok()) {
    return feasibility.status();
  }
  out->set_feasibility(*feasibility);
  out->set_provenance_id(value.provenance_id);
  return Status::Ok();
}

// --- Claims and evidence members --------------------------------------------

StatusOr<Claim> DecodeClaim(const v1::Claim& value, const std::string& owner) {
  Claim out;
  out.id = value.id();
  auto kind = DecodeEnum(value.kind(), kClaimKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out.kind = *kind;
  auto severity = DecodeEnum(value.severity(), kSeverities, At(owner, "severity"));
  if (!severity.ok()) {
    return severity.status();
  }
  out.severity = *severity;
  out.subject = value.subject();
  out.description = value.description();
  out.predicate = DecodeExpression(value.predicate());
  return out;
}

Status EncodeClaim(const Claim& value, v1::Claim* out) {
  const std::string owner = Owned("claim", value.id);
  out->set_id(value.id);
  auto kind = EncodeEnum(value.kind, kClaimKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out->set_kind(*kind);
  auto severity = EncodeEnum(value.severity, kSeverities, At(owner, "severity"));
  if (!severity.ok()) {
    return severity.status();
  }
  out->set_severity(*severity);
  out->set_subject(value.subject);
  out->set_description(value.description);
  // The predicate is optional in the model: an absent one is an absence here.
  if (IsPresent(value.predicate)) {
    *out->mutable_predicate() = EncodeExpression(value.predicate);
  }
  return Status::Ok();
}

StatusOr<Fact> DecodeFact(const v1::Fact& value, const std::string& owner) {
  Fact out;
  out.id = value.id();
  auto stable_id = OptionalStableId(At(owner, "stable_id"), value.stable_id());
  if (!stable_id.ok()) {
    return stable_id.status();
  }
  out.stable_id = std::move(*stable_id);
  auto epistemic =
      DecodeEnum(value.epistemic(), kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out.epistemic = *epistemic;
  auto confidence =
      DecodeEnum(value.confidence(), kConfidences, At(owner, "confidence"));
  if (!confidence.ok()) {
    return confidence.status();
  }
  out.confidence = *confidence;
  out.predicate = DecodeExpression(value.predicate());
  out.producer = value.producer();
  out.provenance_id = value.provenance_id();
  out.derived = value.derived();
  return out;
}

Status EncodeFact(const Fact& value, v1::Fact* out) {
  const std::string owner = Owned("fact", value.id);
  out->set_id(value.id);
  if (value.stable_id.has_value()) {
    out->set_stable_id(core::ToString(*value.stable_id));
  }
  *out->mutable_predicate() = EncodeExpression(value.predicate);
  auto epistemic =
      EncodeEnum(value.epistemic, kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out->set_epistemic(*epistemic);
  auto confidence =
      EncodeEnum(value.confidence, kConfidences, At(owner, "confidence"));
  if (!confidence.ok()) {
    return confidence.status();
  }
  out->set_confidence(*confidence);
  out->set_producer(value.producer);
  out->set_provenance_id(value.provenance_id);
  out->set_derived(value.derived);
  return Status::Ok();
}

StatusOr<Assumption> DecodeAssumption(const v1::Assumption& value) {
  Assumption out;
  out.id = value.id();
  out.predicate = DecodeExpression(value.predicate());
  out.source = value.source();
  out.scope = value.scope();
  return out;
}

Status EncodeAssumption(const Assumption& value, v1::Assumption* out) {
  out->set_id(value.id);
  *out->mutable_predicate() = EncodeExpression(value.predicate);
  out->set_source(value.source);
  out->set_scope(value.scope);
  return Status::Ok();
}

StatusOr<Hypothesis> DecodeHypothesis(const v1::Hypothesis& value,
                                      const std::string& owner) {
  Hypothesis out;
  out.id = value.id();
  out.predicate = DecodeExpression(value.predicate());
  out.producer = value.producer();
  out.reason = value.reason();
  // Explicit presence: an absent confidence is the model's `kUnspecified`, and
  // an explicitly encoded zero is not a value this codec accepts.
  if (value.has_confidence()) {
    auto confidence =
        DecodeEnum(value.confidence(), kConfidences, At(owner, "confidence"));
    if (!confidence.ok()) {
      return confidence.status();
    }
    out.confidence = *confidence;
  }
  return out;
}

Status EncodeHypothesis(const Hypothesis& value, v1::Hypothesis* out) {
  const std::string owner = Owned("hypothesis", value.id);
  out->set_id(value.id);
  *out->mutable_predicate() = EncodeExpression(value.predicate);
  out->set_producer(value.producer);
  out->set_reason(value.reason);
  if (value.confidence != Confidence::kUnspecified) {
    auto confidence =
        EncodeEnum(value.confidence, kConfidences, At(owner, "confidence"));
    if (!confidence.ok()) {
      return confidence.status();
    }
    out->set_confidence(*confidence);
  }
  return Status::Ok();
}

StatusOr<Unknown> DecodeUnknown(const v1::Unknown& value,
                                const std::string& owner) {
  Unknown out;
  out.id = value.id();
  out.property = DecodeExpression(value.property());
  auto reason_code = DecodeEnum(value.reason_code(), kUnknownReasonCodes,
                                At(owner, "reason_code"));
  if (!reason_code.ok()) {
    return reason_code.status();
  }
  out.reason_code = *reason_code;
  out.reason = value.reason();
  for (const std::string& blocker : value.blocking_ids()) {
    out.blocking_ids.push_back(blocker);
  }
  out.suggested_resolution = value.suggested_resolution();
  return out;
}

Status EncodeUnknown(const Unknown& value, v1::Unknown* out) {
  const std::string owner = Owned("unknown", value.id);
  out->set_id(value.id);
  *out->mutable_property() = EncodeExpression(value.property);
  auto reason_code = EncodeEnum(value.reason_code, kUnknownReasonCodes,
                                At(owner, "reason_code"));
  if (!reason_code.ok()) {
    return reason_code.status();
  }
  out->set_reason_code(*reason_code);
  out->set_reason(value.reason);
  for (const std::string& blocker : value.blocking_ids) {
    out->add_blocking_ids(blocker);
  }
  out->set_suggested_resolution(value.suggested_resolution);
  return Status::Ok();
}

StatusOr<Constraint> DecodeConstraint(const v1::Constraint& value,
                                      const std::string& owner) {
  Constraint out;
  out.id = value.id();
  out.expression = DecodeExpression(value.expression());
  out.scope = value.scope();
  auto epistemic =
      DecodeEnum(value.epistemic(), kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out.epistemic = *epistemic;
  out.provenance_id = value.provenance_id();
  return out;
}

Status EncodeConstraint(const Constraint& value, v1::Constraint* out) {
  const std::string owner = Owned("constraint", value.id);
  out->set_id(value.id);
  *out->mutable_expression() = EncodeExpression(value.expression);
  out->set_scope(value.scope);
  auto epistemic =
      EncodeEnum(value.epistemic, kEpistemicStates, At(owner, "epistemic"));
  if (!epistemic.ok()) {
    return epistemic.status();
  }
  out->set_epistemic(*epistemic);
  out->set_provenance_id(value.provenance_id);
  return Status::Ok();
}

StatusOr<Provenance> DecodeProvenance(const v1::Provenance& value,
                                      const std::string& owner) {
  Provenance out;
  out.id = value.id();
  out.producer = value.producer();
  out.rule = value.rule();
  for (const std::string& input : value.input_fact_ids()) {
    out.input_fact_ids.push_back(input);
  }
  out.source_anchor_id = value.source_anchor_id();
  auto run_id = OptionalStableId(At(owner, "analysis_run_id"),
                                 value.analysis_run_id());
  if (!run_id.ok()) {
    return run_id.status();
  }
  out.analysis_run_id = std::move(*run_id);
  out.version = value.version();
  out.configuration = value.configuration();
  return out;
}

Status EncodeProvenance(const Provenance& value, v1::Provenance* out) {
  out->set_id(value.id);
  out->set_producer(value.producer);
  out->set_rule(value.rule);
  for (const std::string& input : value.input_fact_ids) {
    out->add_input_fact_ids(input);
  }
  out->set_source_anchor_id(value.source_anchor_id);
  if (value.analysis_run_id.has_value()) {
    out->set_analysis_run_id(core::ToString(*value.analysis_run_id));
  }
  out->set_version(value.version);
  out->set_configuration(value.configuration);
  return Status::Ok();
}

StatusOr<ProofObligation> DecodeProofObligation(const v1::ProofObligation& value,
                                                const std::string& owner) {
  ProofObligation out;
  out.id = value.id();
  auto goal_kind =
      DecodeEnum(value.goal_kind(), kProofGoalKinds, At(owner, "goal_kind"));
  if (!goal_kind.ok()) {
    return goal_kind.status();
  }
  out.goal_kind = *goal_kind;
  out.predicate = DecodeExpression(value.predicate());
  for (const std::string& verifier : value.verifier_kinds()) {
    out.verifier_kinds.push_back(verifier);
  }
  // An absent budget decodes to the model's absent-budget expression.
  out.budget = DecodeExpression(value.budget());
  auto status =
      DecodeEnum(value.status(), kProofStatuses, At(owner, "status"));
  if (!status.ok()) {
    return status.status();
  }
  out.status = *status;
  out.result_id = value.result_id();
  out.verification_producer = value.verification_producer();
  return out;
}

Status EncodeProofObligation(const ProofObligation& value,
                             v1::ProofObligation* out) {
  const std::string owner = Owned("proof obligation", value.id);
  out->set_id(value.id);
  auto goal_kind =
      EncodeEnum(value.goal_kind, kProofGoalKinds, At(owner, "goal_kind"));
  if (!goal_kind.ok()) {
    return goal_kind.status();
  }
  out->set_goal_kind(*goal_kind);
  *out->mutable_predicate() = EncodeExpression(value.predicate);
  for (const std::string& verifier : value.verifier_kinds) {
    out->add_verifier_kinds(verifier);
  }
  // The grammar's budget is optional; an absent one stays absent on the wire.
  if (IsPresent(value.budget)) {
    *out->mutable_budget() = EncodeExpression(value.budget);
  }
  auto status = EncodeEnum(value.status, kProofStatuses, At(owner, "status"));
  if (!status.ok()) {
    return status.status();
  }
  out->set_status(*status);
  out->set_result_id(value.result_id);
  out->set_verification_producer(value.verification_producer);
  return Status::Ok();
}

StatusOr<SummaryReference> DecodeSummaryReference(
    const v1::SummaryReference& value, const std::string& owner) {
  SummaryReference out;
  out.id = value.id();
  out.function_id = value.function_id();
  auto summary_id =
      RequiredStableId(At(owner, "summary_id"), value.summary_id());
  if (!summary_id.ok()) {
    return summary_id.status();
  }
  out.summary_id = *summary_id;
  for (const std::string& component : value.components()) {
    out.components.push_back(component);
  }
  return out;
}

Status EncodeSummaryReference(const SummaryReference& value,
                              v1::SummaryReference* out) {
  out->set_id(value.id);
  out->set_function_id(value.function_id);
  out->set_summary_id(core::ToString(value.summary_id));
  for (const std::string& component : value.components) {
    out->add_components(component);
  }
  return Status::Ok();
}

StatusOr<Dependency> DecodeDependency(const v1::Dependency& value,
                                      const std::string& owner) {
  Dependency out;
  out.id = value.id();
  auto kind = DecodeEnum(value.kind(), kDependencyKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out.kind = *kind;
  auto stable_id = RequiredStableId(At(owner, "stable_id"), value.stable_id());
  if (!stable_id.ok()) {
    return stable_id.status();
  }
  out.stable_id = *stable_id;
  return out;
}

Status EncodeDependency(const Dependency& value, v1::Dependency* out) {
  const std::string owner = Owned("dependency", value.id);
  out->set_id(value.id);
  auto kind = EncodeEnum(value.kind, kDependencyKinds, At(owner, "kind"));
  if (!kind.ok()) {
    return kind.status();
  }
  out->set_kind(*kind);
  out->set_stable_id(core::ToString(value.stable_id));
  return Status::Ok();
}

StatusOr<Omission> DecodeOmission(const v1::Omission& value) {
  Omission out;
  out.id = value.id();
  out.kind = value.kind();
  out.subject = value.subject();
  out.reason = value.reason();
  out.expandable = value.expandable();
  return out;
}

Status EncodeOmission(const Omission& value, v1::Omission* out) {
  out->set_id(value.id);
  out->set_kind(value.kind);
  out->set_subject(value.subject);
  out->set_reason(value.reason);
  out->set_expandable(value.expandable);
  return Status::Ok();
}

// --- The case ---------------------------------------------------------------

Status EncodeCase(const EvidenceCase& value, v1::EvidenceCase* out) {
  if (value.evidence_id.has_value()) {
    out->set_evidence_id(core::ToString(*value.evidence_id));
  }
  out->set_schema_version(value.schema_version);
  auto level = EncodeEnum(value.level, kEvidenceLevels, "level");
  if (!level.ok()) {
    return level.status();
  }
  out->set_level(*level);
  Status status = EncodeProgramBinding(value.program, out->mutable_program());
  if (!status.ok()) {
    return status;
  }
  status = EncodeClaim(value.primary_claim, out->mutable_primary_claim());
  if (!status.ok()) {
    return status;
  }
  for (const Entity& entity : value.entities) {
    status = EncodeEntity(entity, out->add_entities());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Edge& edge : value.edges) {
    status = EncodeEdge(edge, out->add_edges());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Path& path : value.paths) {
    status = EncodePath(path, out->add_paths());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Fact& fact : value.facts) {
    status = EncodeFact(fact, out->add_facts());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Assumption& assumption : value.assumptions) {
    status = EncodeAssumption(assumption, out->add_assumptions());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Hypothesis& hypothesis : value.hypotheses) {
    status = EncodeHypothesis(hypothesis, out->add_hypotheses());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Unknown& unknown : value.unknowns) {
    status = EncodeUnknown(unknown, out->add_unknowns());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Constraint& constraint : value.constraints) {
    status = EncodeConstraint(constraint, out->add_constraints());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Provenance& record : value.provenance) {
    status = EncodeProvenance(record, out->add_provenance());
    if (!status.ok()) {
      return status;
    }
  }
  for (const ProofObligation& obligation : value.proof_obligations) {
    status = EncodeProofObligation(obligation, out->add_proof_obligations());
    if (!status.ok()) {
      return status;
    }
  }
  for (const SummaryReference& summary : value.summaries) {
    status = EncodeSummaryReference(summary, out->add_summaries());
    if (!status.ok()) {
      return status;
    }
  }
  for (const Dependency& dependency : value.dependencies) {
    status = EncodeDependency(dependency, out->add_dependencies());
    if (!status.ok()) {
      return status;
    }
  }
  auto verification_state = EncodeEnum(value.verification_state,
                                       kVerificationStates,
                                       "verification_state");
  if (!verification_state.ok()) {
    return verification_state.status();
  }
  out->set_verification_state(*verification_state);
  for (const Omission& omission : value.omissions) {
    status = EncodeOmission(omission, out->add_omissions());
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

StatusOr<EvidenceCase> DecodeCase(const v1::EvidenceCase& value) {
  EvidenceCase out;
  out.schema_version = value.schema_version();
  auto level = DecodeEnum(value.level(), kEvidenceLevels, "level");
  if (!level.ok()) {
    return level.status();
  }
  out.level = *level;
  auto program = DecodeProgramBinding(value.program());
  if (!program.ok()) {
    return program.status();
  }
  out.program = std::move(*program);
  auto claim = DecodeClaim(value.primary_claim(), "primary_claim");
  if (!claim.ok()) {
    return claim.status();
  }
  out.primary_claim = std::move(*claim);
  for (int index = 0; index < value.entities_size(); ++index) {
    auto entity = DecodeEntity(value.entities(index), Indexed("entities", index));
    if (!entity.ok()) {
      return entity.status();
    }
    out.entities.push_back(std::move(*entity));
  }
  for (int index = 0; index < value.edges_size(); ++index) {
    auto edge = DecodeEdge(value.edges(index), Indexed("edges", index));
    if (!edge.ok()) {
      return edge.status();
    }
    out.edges.push_back(std::move(*edge));
  }
  for (int index = 0; index < value.paths_size(); ++index) {
    auto path = DecodePath(value.paths(index), Indexed("paths", index));
    if (!path.ok()) {
      return path.status();
    }
    out.paths.push_back(std::move(*path));
  }
  for (int index = 0; index < value.facts_size(); ++index) {
    auto fact = DecodeFact(value.facts(index), Indexed("facts", index));
    if (!fact.ok()) {
      return fact.status();
    }
    out.facts.push_back(std::move(*fact));
  }
  for (int index = 0; index < value.assumptions_size(); ++index) {
    auto assumption = DecodeAssumption(value.assumptions(index));
    if (!assumption.ok()) {
      return assumption.status();
    }
    out.assumptions.push_back(std::move(*assumption));
  }
  for (int index = 0; index < value.hypotheses_size(); ++index) {
    auto hypothesis =
        DecodeHypothesis(value.hypotheses(index), Indexed("hypotheses", index));
    if (!hypothesis.ok()) {
      return hypothesis.status();
    }
    out.hypotheses.push_back(std::move(*hypothesis));
  }
  for (int index = 0; index < value.unknowns_size(); ++index) {
    auto unknown = DecodeUnknown(value.unknowns(index), Indexed("unknowns", index));
    if (!unknown.ok()) {
      return unknown.status();
    }
    out.unknowns.push_back(std::move(*unknown));
  }
  for (int index = 0; index < value.constraints_size(); ++index) {
    auto constraint =
        DecodeConstraint(value.constraints(index), Indexed("constraints", index));
    if (!constraint.ok()) {
      return constraint.status();
    }
    out.constraints.push_back(std::move(*constraint));
  }
  for (int index = 0; index < value.provenance_size(); ++index) {
    auto record =
        DecodeProvenance(value.provenance(index), Indexed("provenance", index));
    if (!record.ok()) {
      return record.status();
    }
    out.provenance.push_back(std::move(*record));
  }
  for (int index = 0; index < value.proof_obligations_size(); ++index) {
    auto obligation = DecodeProofObligation(value.proof_obligations(index),
                                            Indexed("proof_obligations", index));
    if (!obligation.ok()) {
      return obligation.status();
    }
    out.proof_obligations.push_back(std::move(*obligation));
  }
  for (int index = 0; index < value.summaries_size(); ++index) {
    auto summary =
        DecodeSummaryReference(value.summaries(index), Indexed("summaries", index));
    if (!summary.ok()) {
      return summary.status();
    }
    out.summaries.push_back(std::move(*summary));
  }
  for (int index = 0; index < value.dependencies_size(); ++index) {
    auto dependency =
        DecodeDependency(value.dependencies(index), Indexed("dependencies", index));
    if (!dependency.ok()) {
      return dependency.status();
    }
    out.dependencies.push_back(std::move(*dependency));
  }
  auto verification_state = DecodeEnum(value.verification_state(),
                                       kVerificationStates,
                                       "verification_state");
  if (!verification_state.ok()) {
    return verification_state.status();
  }
  out.verification_state = *verification_state;
  for (int index = 0; index < value.omissions_size(); ++index) {
    auto omission = DecodeOmission(value.omissions(index));
    if (!omission.ok()) {
      return omission.status();
    }
    out.omissions.push_back(std::move(*omission));
  }
  if (!value.evidence_id().empty()) {
    auto evidence_id = core::ParseStableId(value.evidence_id());
    if (!evidence_id.ok()) {
      return Status::InvalidArgument(
          "evidence_id is not a canonical stable ID: '" + value.evidence_id() +
          "'");
    }
    out.evidence_id = *evidence_id;
  }
  return out;
}

}  // namespace

// --- Public entry points ----------------------------------------------------

StatusOr<veritas::evidence::v1::EvidenceCase> ToEvidenceProto(
    const EvidenceCase& value) {
  // A case that is not well-formed has no wire form: serializing one would
  // manufacture a representation the validator refuses to accept back.
  Status status = RequireValidEvidenceCase(value);
  if (!status.ok()) {
    return status;
  }
  // A carried identity is checked rather than trusted, so a stale one is
  // refused at the boundary that would have published it.
  if (value.evidence_id.has_value()) {
    auto computed = ComputeEvidenceId(value);
    if (!computed.ok()) {
      return computed.status();
    }
    if (!(*computed == *value.evidence_id)) {
      return Status::InvalidArgument(
          "the case's evidence_id " + core::ToString(*value.evidence_id) +
          " disagrees with the identity recomputed from its canonical bytes, " +
          core::ToString(*computed));
    }
  }
  v1::EvidenceCase out;
  status = EncodeCase(value, &out);
  if (!status.ok()) {
    return status;
  }
  return out;
}

StatusOr<EvidenceCase> FromEvidenceProto(
    const veritas::evidence::v1::EvidenceCase& value) {
  auto decoded = DecodeCase(value);
  if (!decoded.ok()) {
    return decoded.status();
  }
  EvidenceCase out = std::move(*decoded);
  // The decoded case is a case like any other: it is validated, and its
  // identity is recomputed from its canonical bytes rather than taken from the
  // wire. A carried identity that disagrees is a rejection, never a silent
  // rewrite.
  Status status = RequireValidEvidenceCase(out);
  if (!status.ok()) {
    return status;
  }
  auto computed = ComputeEvidenceId(out);
  if (!computed.ok()) {
    return computed.status();
  }
  if (out.evidence_id.has_value() && !(*out.evidence_id == *computed)) {
    return Status::InvalidArgument(
        "the encoded evidence_id " + core::ToString(*out.evidence_id) +
        " disagrees with the identity recomputed from the decoded case, " +
        core::ToString(*computed));
  }
  out.evidence_id = *computed;
  return out;
}

StatusOr<std::string> EncodeEvidenceProto(const EvidenceCase& value) {
  auto proto = ToEvidenceProto(value);
  if (!proto.ok()) {
    return proto.status();
  }
  std::string bytes;
  if (!proto->SerializeToString(&bytes)) {
    return Status::Internal("the Evidence Case Protobuf message did not serialize");
  }
  return bytes;
}

StatusOr<EvidenceCase> DecodeEvidenceProto(std::string_view bytes) {
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Status::InvalidArgument(
        "the encoded Evidence Case is too large to parse");
  }
  // An empty input is a parse failure, not an empty case: `ParseFromArray`
  // accepts a zero-length buffer, so the empty literal keeps that path free of
  // a null pointer, and the conversion then refuses the case the empty buffer
  // decodes to rather than returning it.
  const char* data = bytes.empty() ? "" : bytes.data();
  v1::EvidenceCase value;
  if (!value.ParseFromArray(data, static_cast<int>(bytes.size()))) {
    return Status::InvalidArgument(
        "the input is not a parsable Evidence Case Protobuf message");
  }
  return FromEvidenceProto(value);
}

}  // namespace veritas::evidence
