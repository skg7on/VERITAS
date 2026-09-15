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

// EvidenceCase.cpp — textual spellings for the `eir.v1` semantic enums, plus
// the structural comparison of the one recursively owned record.
//
// The spellings are the EIR-T 1.0 grammar's
// (docs/specs/veritas-evidence-ir-formal-specification.md), which is the
// frozen syntax contract. That grammar's terminal sets are the full EIR-T 1.0
// language surface and are deliberately wider than this model: each parser
// accepts the EIR V0.1 subset the M10C design spec selects (§1 and §2.2), as
// fixed by the implementation plan's enum lists, and rejects everything else —
// including a grammar-valid spelling outside the subset, and the "unspecified"
// rendering of the invalid default, which is never a grammar terminal.
// Rejection is diagnosed, never coerced or dropped: silently accepting an
// unrepresentable terminal would break REP-001 losslessness.

#include "veritas/evidence/EvidenceCase.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <string_view>

namespace veritas::evidence {

namespace {

// Field-by-field structural order over `Expression`, in declaration order. The
// operands are compared lexicographically, length breaking a common prefix.
// This is the order the defaulted operator would have produced; it is written
// out because a defaulted one cannot recurse through `std::vector<Expression>`.
std::strong_ordering CompareExpressions(const Expression& left,
                                        const Expression& right) {
  if (auto cmp = left.kind <=> right.kind; cmp != 0) return cmp;
  if (auto cmp = left.text <=> right.text; cmp != 0) return cmp;
  if (auto cmp = left.integer <=> right.integer; cmp != 0) return cmp;
  if (auto cmp = left.boolean <=> right.boolean; cmp != 0) return cmp;
  const std::size_t common =
      std::min(left.operands.size(), right.operands.size());
  for (std::size_t index = 0; index < common; ++index) {
    if (auto cmp = CompareExpressions(left.operands[index],
                                      right.operands[index]);
        cmp != 0) {
      return cmp;
    }
  }
  return left.operands.size() <=> right.operands.size();
}


// The invalid default is never a grammar spelling, so every parser rejects it.
constexpr std::string_view kUnspecifiedSpelling = "unspecified";

Status Unrecognized(std::string_view family, std::string_view text) {
  std::string message = "unrecognized ";
  message += family;
  message += " spelling: ";
  message.append(text);
  return Status::InvalidArgument(std::move(message));
}

}  // namespace

bool Expression::operator==(const Expression& other) const {
  return CompareExpressions(*this, other) == 0;
}

std::strong_ordering Expression::operator<=>(const Expression& other) const {
  return CompareExpressions(*this, other);
}

std::string_view ToString(EvidenceLevel value) {
  switch (value) {
    case EvidenceLevel::kUnspecified:
      return kUnspecifiedSpelling;
    case EvidenceLevel::kL0:
      return "l0";
    case EvidenceLevel::kL1:
      return "l1";
    case EvidenceLevel::kL2:
      return "l2";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(VerificationState value) {
  switch (value) {
    case VerificationState::kUnspecified:
      return kUnspecifiedSpelling;
    case VerificationState::kUnreviewed:
      return "UNREVIEWED";
    case VerificationState::kPossibleDefect:
      return "POSSIBLE_DEFECT";
    case VerificationState::kLikelyDefect:
      return "LIKELY_DEFECT";
    case VerificationState::kVerifiedDefect:
      return "VERIFIED_DEFECT";
    case VerificationState::kLikelyFalsePositive:
      return "LIKELY_FALSE_POSITIVE";
    case VerificationState::kVerifiedSafe:
      return "VERIFIED_SAFE";
    case VerificationState::kInconclusive:
      return "INCONCLUSIVE";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(EntityKind value) {
  switch (value) {
    case EntityKind::kUnspecified:
      return kUnspecifiedSpelling;
    case EntityKind::kFunction:
      return "function";
    case EntityKind::kCallSite:
      return "callsite";
    case EntityKind::kValue:
      return "value";
    case EntityKind::kMemoryObject:
      return "memory_object";
    case EntityKind::kBasicBlock:
      return "basic_block";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(RelationKind value) {
  switch (value) {
    case RelationKind::kUnspecified:
      return kUnspecifiedSpelling;
    case RelationKind::kCalls:
      return "CALLS";
    case RelationKind::kFlowsTo:
      return "FLOWS_TO";
    case RelationKind::kReads:
      return "READS";
    case RelationKind::kWrites:
      return "WRITES";
    case RelationKind::kDominates:
      return "DOMINATES";
    case RelationKind::kMayAlias:
      return "MAY_ALIAS";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(PathKind value) {
  switch (value) {
    case PathKind::kUnspecified:
      return kUnspecifiedSpelling;
    case PathKind::kCall:
      return "call";
    case PathKind::kValueFlow:
      return "value_flow";
    case PathKind::kControl:
      return "control";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(EpistemicState value) {
  switch (value) {
    case EpistemicState::kUnspecified:
      return kUnspecifiedSpelling;
    case EpistemicState::kMust:
      return "must";
    case EpistemicState::kMay:
      return "may";
    case EpistemicState::kMustNot:
      return "must_not";
    case EpistemicState::kInferred:
      return "inferred";
    case EpistemicState::kAssumed:
      return "assumed";
    case EpistemicState::kUnknown:
      return "unknown";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(Confidence value) {
  switch (value) {
    case Confidence::kUnspecified:
      return kUnspecifiedSpelling;
    case Confidence::kExact:
      return "exact";
    case Confidence::kHigh:
      return "high";
    case Confidence::kMedium:
      return "medium";
    case Confidence::kLow:
      return "low";
    case Confidence::kUnknown:
      return "unknown";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(ProofStatus value) {
  switch (value) {
    case ProofStatus::kUnspecified:
      return kUnspecifiedSpelling;
    case ProofStatus::kPending:
      return "PENDING";
    case ProofStatus::kProved:
      return "PROVED";
    case ProofStatus::kRefuted:
      return "REFUTED";
    case ProofStatus::kUnknown:
      return "UNKNOWN";
    case ProofStatus::kTimeout:
      return "TIMEOUT";
    case ProofStatus::kUnsupported:
      return "UNSUPPORTED";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(DependencyKind value) {
  switch (value) {
    case DependencyKind::kUnspecified:
      return kUnspecifiedSpelling;
    case DependencyKind::kSummary:
      return "summary";
    case DependencyKind::kFact:
      return "fact";
    case DependencyKind::kTypeLayout:
      return "type_layout";
    case DependencyKind::kConfiguration:
      return "configuration";
    case DependencyKind::kSpecification:
      return "specification";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(Feasibility value) {
  switch (value) {
    case Feasibility::kUnspecified:
      return kUnspecifiedSpelling;
    case Feasibility::kProvedFeasible:
      return "PROVED_FEASIBLE";
    case Feasibility::kSat:
      return "SAT";
    case Feasibility::kMaybe:
      return "MAYBE";
    case Feasibility::kUntested:
      return "UNTESTED";
    case Feasibility::kUnsat:
      return "UNSAT";
    case Feasibility::kProvedInfeasible:
      return "PROVED_INFEASIBLE";
    case Feasibility::kUnknown:
      return "UNKNOWN";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(ProofGoalKind value) {
  switch (value) {
    case ProofGoalKind::kUnspecified:
      return kUnspecifiedSpelling;
    case ProofGoalKind::kProve:
      return "prove";
    case ProofGoalKind::kRefute:
      return "refute";
    case ProofGoalKind::kCheck:
      return "check";
  }
  return kUnspecifiedSpelling;
}

std::string_view ToString(UnknownReasonCode value) {
  switch (value) {
    case UnknownReasonCode::kUnspecified:
      return kUnspecifiedSpelling;
    case UnknownReasonCode::kUnresolvedCall:
      return "UNRESOLVED_CALL";
    case UnknownReasonCode::kUnknownAlias:
      return "UNKNOWN_ALIAS";
    case UnknownReasonCode::kExternalFunction:
      return "EXTERNAL_FUNCTION";
    case UnknownReasonCode::kMissingSpecification:
      return "MISSING_SPECIFICATION";
    case UnknownReasonCode::kAnalysisTimeout:
      return "ANALYSIS_TIMEOUT";
    case UnknownReasonCode::kStateExplosion:
      return "STATE_EXPLOSION";
    case UnknownReasonCode::kUnsupportedLanguageFeature:
      return "UNSUPPORTED_LANGUAGE_FEATURE";
    case UnknownReasonCode::kInlineAssembly:
      return "INLINE_ASSEMBLY";
    case UnknownReasonCode::kDynamicLoading:
      return "DYNAMIC_LOADING";
    case UnknownReasonCode::kUnknownBuildConfiguration:
      return "UNKNOWN_BUILD_CONFIGURATION";
  }
  return kUnspecifiedSpelling;
}

StatusOr<EvidenceLevel> ParseEvidenceLevel(std::string_view text) {
  if (text == "l0") return EvidenceLevel::kL0;
  if (text == "l1") return EvidenceLevel::kL1;
  if (text == "l2") return EvidenceLevel::kL2;
  return Unrecognized("evidence level", text);
}

StatusOr<VerificationState> ParseVerificationState(std::string_view text) {
  if (text == "UNREVIEWED") return VerificationState::kUnreviewed;
  if (text == "POSSIBLE_DEFECT") return VerificationState::kPossibleDefect;
  if (text == "LIKELY_DEFECT") return VerificationState::kLikelyDefect;
  if (text == "VERIFIED_DEFECT") return VerificationState::kVerifiedDefect;
  if (text == "LIKELY_FALSE_POSITIVE") {
    return VerificationState::kLikelyFalsePositive;
  }
  if (text == "VERIFIED_SAFE") return VerificationState::kVerifiedSafe;
  if (text == "INCONCLUSIVE") return VerificationState::kInconclusive;
  return Unrecognized("verification state", text);
}

StatusOr<EntityKind> ParseEntityKind(std::string_view text) {
  if (text == "function") return EntityKind::kFunction;
  if (text == "callsite") return EntityKind::kCallSite;
  if (text == "value") return EntityKind::kValue;
  if (text == "memory_object") return EntityKind::kMemoryObject;
  if (text == "basic_block") return EntityKind::kBasicBlock;
  return Unrecognized("entity kind", text);
}

StatusOr<RelationKind> ParseRelationKind(std::string_view text) {
  if (text == "CALLS") return RelationKind::kCalls;
  if (text == "FLOWS_TO") return RelationKind::kFlowsTo;
  if (text == "READS") return RelationKind::kReads;
  if (text == "WRITES") return RelationKind::kWrites;
  if (text == "DOMINATES") return RelationKind::kDominates;
  if (text == "MAY_ALIAS") return RelationKind::kMayAlias;
  return Unrecognized("relation kind", text);
}

StatusOr<PathKind> ParsePathKind(std::string_view text) {
  if (text == "call") return PathKind::kCall;
  if (text == "value_flow") return PathKind::kValueFlow;
  if (text == "control") return PathKind::kControl;
  return Unrecognized("path kind", text);
}

StatusOr<EpistemicState> ParseEpistemicState(std::string_view text) {
  if (text == "must") return EpistemicState::kMust;
  if (text == "may") return EpistemicState::kMay;
  if (text == "must_not") return EpistemicState::kMustNot;
  if (text == "inferred") return EpistemicState::kInferred;
  if (text == "assumed") return EpistemicState::kAssumed;
  if (text == "unknown") return EpistemicState::kUnknown;
  return Unrecognized("epistemic state", text);
}

StatusOr<Confidence> ParseConfidence(std::string_view text) {
  if (text == "exact") return Confidence::kExact;
  if (text == "high") return Confidence::kHigh;
  if (text == "medium") return Confidence::kMedium;
  if (text == "low") return Confidence::kLow;
  if (text == "unknown") return Confidence::kUnknown;
  return Unrecognized("confidence", text);
}

StatusOr<ProofStatus> ParseProofStatus(std::string_view text) {
  if (text == "PENDING") return ProofStatus::kPending;
  if (text == "PROVED") return ProofStatus::kProved;
  if (text == "REFUTED") return ProofStatus::kRefuted;
  if (text == "UNKNOWN") return ProofStatus::kUnknown;
  if (text == "TIMEOUT") return ProofStatus::kTimeout;
  if (text == "UNSUPPORTED") return ProofStatus::kUnsupported;
  return Unrecognized("proof status", text);
}

StatusOr<DependencyKind> ParseDependencyKind(std::string_view text) {
  if (text == "summary") return DependencyKind::kSummary;
  if (text == "fact") return DependencyKind::kFact;
  if (text == "type_layout") return DependencyKind::kTypeLayout;
  if (text == "configuration") return DependencyKind::kConfiguration;
  if (text == "specification") return DependencyKind::kSpecification;
  return Unrecognized("dependency kind", text);
}

StatusOr<Feasibility> ParseFeasibility(std::string_view text) {
  if (text == "PROVED_FEASIBLE") return Feasibility::kProvedFeasible;
  if (text == "SAT") return Feasibility::kSat;
  if (text == "MAYBE") return Feasibility::kMaybe;
  if (text == "UNTESTED") return Feasibility::kUntested;
  if (text == "UNSAT") return Feasibility::kUnsat;
  if (text == "PROVED_INFEASIBLE") return Feasibility::kProvedInfeasible;
  if (text == "UNKNOWN") return Feasibility::kUnknown;
  return Unrecognized("feasibility", text);
}

StatusOr<ProofGoalKind> ParseProofGoalKind(std::string_view text) {
  if (text == "prove") return ProofGoalKind::kProve;
  if (text == "refute") return ProofGoalKind::kRefute;
  if (text == "check") return ProofGoalKind::kCheck;
  return Unrecognized("proof goal kind", text);
}

StatusOr<UnknownReasonCode> ParseUnknownReasonCode(std::string_view text) {
  if (text == "UNRESOLVED_CALL") return UnknownReasonCode::kUnresolvedCall;
  if (text == "UNKNOWN_ALIAS") return UnknownReasonCode::kUnknownAlias;
  if (text == "EXTERNAL_FUNCTION") {
    return UnknownReasonCode::kExternalFunction;
  }
  if (text == "MISSING_SPECIFICATION") {
    return UnknownReasonCode::kMissingSpecification;
  }
  if (text == "ANALYSIS_TIMEOUT") return UnknownReasonCode::kAnalysisTimeout;
  if (text == "STATE_EXPLOSION") return UnknownReasonCode::kStateExplosion;
  if (text == "UNSUPPORTED_LANGUAGE_FEATURE") {
    return UnknownReasonCode::kUnsupportedLanguageFeature;
  }
  if (text == "INLINE_ASSEMBLY") return UnknownReasonCode::kInlineAssembly;
  if (text == "DYNAMIC_LOADING") return UnknownReasonCode::kDynamicLoading;
  if (text == "UNKNOWN_BUILD_CONFIGURATION") {
    return UnknownReasonCode::kUnknownBuildConfiguration;
  }
  return Unrecognized("unknown reason code", text);
}

}  // namespace veritas::evidence
