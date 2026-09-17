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

// EvidenceValidator.cpp — the validation passes behind
// `ValidateEvidenceCase`.
//
// The passes run in a fixed order so a case reports its root cause before its
// consequences: header and context, primary claim, local identity, required
// states, provenance and run binding, stable identity, proof authority,
// references, expression typing, path connectivity, hypothesis isolation, and
// omission visibility. References are resolved only after the provenance pass
// has run, so a derived fact with no provenance reports `kMissingProvenance`
// rather than the dangling reference that absence also implies.
//
// Validation is read-only by construction: the pass object holds the case by
// const reference and every index it builds maps identifiers to `const`
// pointers into that case. No pass fetches, rebases, or synthesizes a member,
// a provenance record, or a run binding.

#include "veritas/evidence/EvidenceValidator.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/evidence/EvidenceQueryService.h"

namespace veritas::evidence {

namespace {

// The deepest expression tree validation walks. The model is a finite tree, so
// this only bounds a pathological or adversarial input; the deterministic
// fixtures nest three levels deep.
constexpr std::size_t kMaxExpressionDepth = 64;

// The case-local member families that share one flat identifier space.
enum class MemberKind {
  kClaim,
  kEntity,
  kEdge,
  kPath,
  kFact,
  kAssumption,
  kHypothesis,
  kUnknown,
  kConstraint,
  kProvenance,
  kProofObligation,
  kSummary,
  kDependency,
  kOmission,
};

std::string_view MemberKindName(MemberKind kind) {
  switch (kind) {
    case MemberKind::kClaim:
      return "claim";
    case MemberKind::kEntity:
      return "entity";
    case MemberKind::kEdge:
      return "edge";
    case MemberKind::kPath:
      return "path";
    case MemberKind::kFact:
      return "fact";
    case MemberKind::kAssumption:
      return "assumption";
    case MemberKind::kHypothesis:
      return "hypothesis";
    case MemberKind::kUnknown:
      return "unknown";
    case MemberKind::kConstraint:
      return "constraint";
    case MemberKind::kProvenance:
      return "provenance";
    case MemberKind::kProofObligation:
      return "proof obligation";
    case MemberKind::kSummary:
      return "summary reference";
    case MemberKind::kDependency:
      return "dependency";
    case MemberKind::kOmission:
      return "omission";
  }
  return "member";
}

std::string_view ExpressionKindName(Expression::Kind kind) {
  switch (kind) {
    case Expression::Kind::kUnspecified:
      return "unspecified";
    case Expression::Kind::kBool:
      return "bool";
    case Expression::Kind::kInteger:
      return "integer";
    case Expression::Kind::kString:
      return "string";
    case Expression::Kind::kSymbol:
      return "symbol";
    case Expression::Kind::kReference:
      return "reference";
    case Expression::Kind::kCall:
      return "call";
    case Expression::Kind::kNot:
      return "not";
    case Expression::Kind::kCompare:
      return "compare";
    case Expression::Kind::kAnd:
      return "and";
    case Expression::Kind::kOr:
      return "or";
    case Expression::Kind::kImplies:
      return "implies";
    case Expression::Kind::kForAll:
      return "forall";
    case Expression::Kind::kExists:
      return "exists";
  }
  return "expression";
}

// A formula in a value position is a type error; so is a literal in a formula
// position. These two predicates are the coarse type check the model's
// untyped `Expression` admits without a full type system.
bool IsFormulaKind(Expression::Kind kind) {
  switch (kind) {
    case Expression::Kind::kBool:
    case Expression::Kind::kNot:
    case Expression::Kind::kCompare:
    case Expression::Kind::kAnd:
    case Expression::Kind::kOr:
    case Expression::Kind::kImplies:
    case Expression::Kind::kForAll:
    case Expression::Kind::kExists:
      return true;
    case Expression::Kind::kUnspecified:
    case Expression::Kind::kInteger:
    case Expression::Kind::kString:
    case Expression::Kind::kSymbol:
    case Expression::Kind::kReference:
    case Expression::Kind::kCall:
      return false;
  }
  return false;
}

bool IsLiteralKind(Expression::Kind kind) {
  return kind == Expression::Kind::kInteger || kind == Expression::Kind::kString;
}

bool IsComparisonOperator(std::string_view text) {
  return text == "==" || text == "!=" || text == "<" || text == "<=" ||
         text == ">" || text == ">=";
}

// An interrupted query: the analysis stopped before it could answer, so the
// open question is a withheld result rather than an unresolved property.
bool IsInterruptedQuery(UnknownReasonCode code) {
  return code == UnknownReasonCode::kAnalysisTimeout ||
         code == UnknownReasonCode::kStateExplosion;
}

// A case state only deterministic verification may claim (formal spec §18).
bool IsVerifiedState(VerificationState state) {
  return state == VerificationState::kVerifiedDefect ||
         state == VerificationState::kVerifiedSafe;
}

// A stable ID is usable only if it is canonical: a non-empty digest whose
// serialized form re-parses.
bool IsCanonicalStableId(const core::StableId& id) {
  if (id.digest_hex.empty()) {
    return false;
  }
  return core::ParseStableId(core::ToString(id)).ok();
}

// The relation a path segment must carry, given the path's kind.
std::optional<RelationKind> RelationForPathKind(PathKind kind) {
  switch (kind) {
    case PathKind::kUnspecified:
      return std::nullopt;
    case PathKind::kCall:
      return RelationKind::kCalls;
    case PathKind::kValueFlow:
      return RelationKind::kFlowsTo;
    case PathKind::kControl:
      return RelationKind::kDominates;
  }
  return std::nullopt;
}

std::string Quoted(std::string_view text) {
  std::string out = "'";
  out.append(text);
  out += "'";
  return out;
}

std::string Joined(std::string_view left, std::string_view right) {
  std::string out;
  out.reserve(left.size() + right.size());
  out.append(left);
  out.append(right);
  return out;
}

// The read-only index of one case: which member declares each local
// identifier, and the typed lookups the later passes need. Identifiers are
// kept both in declaration order (for duplicate reporting) and by name (for
// resolution); both map to `const` views of the case.
class CaseValidator {
 public:
  explicit CaseValidator(const EvidenceCase& value) : value_(value) {}

  EvidenceValidationReport Run() {
    IndexMembers();
    CheckHeader();
    CheckProgramBinding();
    CheckPrimaryClaim();
    CheckDuplicateLocalIds();
    CheckRequiredStates();
    CheckProvenance();
    CheckStableIds();
    CheckProofAuthority();
    CheckReferences();
    CheckExpressions();
    CheckPaths();
    CheckHypothesisIsolation();
    CheckOmissionVisibility();
    return std::move(report_);
  }

 private:
  void Add(EvidenceValidationCode code, std::string member_id,
           std::string message) {
    EvidenceValidationIssue issue;
    issue.code = code;
    issue.member_id = std::move(member_id);
    issue.message = std::move(message);
    report_.issues.push_back(std::move(issue));
  }

  // --- Indexing ------------------------------------------------------------

  void Declare(const std::string& id, MemberKind kind) {
    if (id.empty()) {
      return;
    }
    declarations_.push_back(std::make_pair(id, kind));
    members_.emplace(id, kind);
  }

  void IndexMembers() {
    Declare(value_.primary_claim.id, MemberKind::kClaim);
    for (const Entity& entity : value_.entities) {
      Declare(entity.id, MemberKind::kEntity);
    }
    for (const Edge& edge : value_.edges) {
      Declare(edge.id, MemberKind::kEdge);
    }
    for (const Path& path : value_.paths) {
      Declare(path.id, MemberKind::kPath);
    }
    for (const Fact& fact : value_.facts) {
      Declare(fact.id, MemberKind::kFact);
    }
    for (const Assumption& assumption : value_.assumptions) {
      Declare(assumption.id, MemberKind::kAssumption);
    }
    for (const Hypothesis& hypothesis : value_.hypotheses) {
      Declare(hypothesis.id, MemberKind::kHypothesis);
    }
    for (const Unknown& unknown : value_.unknowns) {
      Declare(unknown.id, MemberKind::kUnknown);
    }
    for (const Constraint& constraint : value_.constraints) {
      Declare(constraint.id, MemberKind::kConstraint);
    }
    for (const Provenance& record : value_.provenance) {
      Declare(record.id, MemberKind::kProvenance);
      provenance_[record.id] = &record;
    }
    for (const ProofObligation& obligation : value_.proof_obligations) {
      Declare(obligation.id, MemberKind::kProofObligation);
    }
    for (const SummaryReference& summary : value_.summaries) {
      Declare(summary.id, MemberKind::kSummary);
    }
    for (const Dependency& dependency : value_.dependencies) {
      Declare(dependency.id, MemberKind::kDependency);
    }
    for (const Omission& omission : value_.omissions) {
      Declare(omission.id, MemberKind::kOmission);
    }
  }

  // --- Passes --------------------------------------------------------------

  void CheckHeader() {
    if (value_.schema_version != kEvidenceSchemaVersion) {
      Add(EvidenceValidationCode::kSchemaVersion, "schema_version",
          Joined("the case declares schema version ",
                 Quoted(value_.schema_version)) +
              Joined(", not ", Quoted(kEvidenceSchemaVersion)));
    }
    if (value_.level == EvidenceLevel::kUnspecified) {
      Add(EvidenceValidationCode::kSchemaVersion, "level",
          "the case declares no abstraction level");
    }
  }

  void CheckProgramBinding() {
    const std::pair<std::string_view, const std::string*> fields[] = {
        {"repository_id", &value_.program.repository_id},
        {"revision_id", &value_.program.revision_id},
        {"build_variant_id", &value_.program.build_variant_id},
        {"target_triple", &value_.program.target_triple},
        {"analysis_configuration_id",
         &value_.program.analysis_configuration_id},
        {"type_layout_id", &value_.program.type_layout_id},
    };
    for (const auto& field : fields) {
      if (field.second->empty()) {
        Add(EvidenceValidationCode::kMissingProgramContext, "program",
            Joined("program.", field.first) + " is empty");
      }
    }
    if (!value_.program.analysis_run_id.has_value()) {
      Add(EvidenceValidationCode::kMissingProgramContext, "program",
          "program.analysis_run_id is absent");
    } else if (!IsCanonicalStableId(*value_.program.analysis_run_id)) {
      Add(EvidenceValidationCode::kInvalidStableId, "program",
          "program.analysis_run_id is not a canonical stable ID");
    }
  }

  void CheckPrimaryClaim() {
    if (value_.primary_claim.id.empty()) {
      Add(EvidenceValidationCode::kPrimaryClaimCount, "",
          "the case declares no primary claim");
    }
  }

  void CheckDuplicateLocalIds() {
    std::map<std::string, MemberKind> seen;
    for (const auto& declaration : declarations_) {
      const auto inserted = seen.emplace(declaration.first, declaration.second);
      if (inserted.second) {
        continue;
      }
      Add(EvidenceValidationCode::kDuplicateLocalId, declaration.first,
          Joined("local identifier ", Quoted(declaration.first)) +
              Joined(" is declared more than once (first as a ",
                     MemberKindName(inserted.first->second)) +
              Joined(", then as a ",
                     MemberKindName(declaration.second)) +
              ")");
    }
  }

  void CheckRequiredStates() {
    if (value_.primary_claim.kind == ClaimKind::kUnspecified) {
      Add(EvidenceValidationCode::kMissingEpistemic, value_.primary_claim.id,
          "the primary claim declares no claim kind");
    }
    if (value_.primary_claim.severity == Severity::kUnspecified) {
      Add(EvidenceValidationCode::kMissingEpistemic, value_.primary_claim.id,
          "the primary claim declares no severity");
    }
    if (value_.verification_state == VerificationState::kUnspecified) {
      Add(EvidenceValidationCode::kMissingEpistemic, "",
          "the case declares no verification state");
    }
    for (const Entity& entity : value_.entities) {
      if (entity.kind == EntityKind::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, entity.id,
            Joined("entity ", Quoted(entity.id)) +
                " declares no entity kind");
      }
    }
    for (const Edge& edge : value_.edges) {
      if (edge.kind == RelationKind::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, edge.id,
            Joined("edge ", Quoted(edge.id)) +
                " declares no relation kind");
      }
      if (edge.epistemic == EpistemicState::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, edge.id,
            Joined("edge ", Quoted(edge.id)) +
                " declares no epistemic state");
      }
    }
    for (const Path& path : value_.paths) {
      if (path.kind == PathKind::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, path.id,
            Joined("path ", Quoted(path.id)) + " declares no path kind");
      }
      if (path.feasibility == Feasibility::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, path.id,
            Joined("path ", Quoted(path.id)) +
                " declares no feasibility");
      }
    }
    for (const Fact& fact : value_.facts) {
      if (fact.epistemic == EpistemicState::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, fact.id,
            Joined("fact ", Quoted(fact.id)) +
                " declares no epistemic state");
      }
      if (fact.confidence == Confidence::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, fact.id,
            Joined("fact ", Quoted(fact.id)) + " declares no confidence");
      }
    }
    for (const Unknown& unknown : value_.unknowns) {
      if (unknown.reason_code == UnknownReasonCode::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, unknown.id,
            Joined("unknown ", Quoted(unknown.id)) +
                " declares no reason code");
      }
    }
    for (const Constraint& constraint : value_.constraints) {
      if (constraint.epistemic == EpistemicState::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, constraint.id,
            Joined("constraint ", Quoted(constraint.id)) +
                " declares no epistemic state");
      }
    }
    for (const ProofObligation& obligation : value_.proof_obligations) {
      if (obligation.goal_kind == ProofGoalKind::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, obligation.id,
            Joined("proof obligation ", Quoted(obligation.id)) +
                " declares no goal kind");
      }
      if (obligation.status == ProofStatus::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, obligation.id,
            Joined("proof obligation ", Quoted(obligation.id)) +
                " declares no status");
      }
    }
    for (const Dependency& dependency : value_.dependencies) {
      if (dependency.kind == DependencyKind::kUnspecified) {
        Add(EvidenceValidationCode::kMissingEpistemic, dependency.id,
            Joined("dependency ", Quoted(dependency.id)) +
                " declares no dependency kind");
      }
    }
  }

  void CheckProvenance() {
    // A derived fact without resolvable provenance has hidden its derivation.
    // A non-derived fact may carry provenance, and when it does the reference
    // pass resolves it.
    for (const Fact& fact : value_.facts) {
      if (!fact.derived) {
        continue;
      }
      if (fact.provenance_id.empty()) {
        Add(EvidenceValidationCode::kMissingProvenance, fact.id,
            Joined("derived fact ", Quoted(fact.id)) +
                " carries no provenance");
        continue;
      }
      if (provenance_.find(fact.provenance_id) == provenance_.end()) {
        Add(EvidenceValidationCode::kMissingProvenance, fact.id,
            Joined("derived fact ", Quoted(fact.id)) +
                " names provenance " + Quoted(fact.provenance_id) +
                ", which the case does not declare");
      }
    }

    // Every provenance record belongs to the one run the case is bound to, and
    // says which run that is.
    const Provenance* query_provenance = nullptr;
    for (const Provenance& record : value_.provenance) {
      if (!record.analysis_run_id.has_value()) {
        Add(EvidenceValidationCode::kMissingProvenance, record.id,
            Joined("provenance ", Quoted(record.id)) +
                " declares no analysis run");
      } else if (value_.program.analysis_run_id.has_value() &&
                 *record.analysis_run_id != *value_.program.analysis_run_id) {
        Add(EvidenceValidationCode::kMixedProgramContext, record.id,
            Joined("provenance ", Quoted(record.id)) +
                " belongs to another analysis run than the case binding");
      }
      if (record.producer == kQueryCompletionProducerId &&
          record.rule == kQueryCompletionRuleId) {
        query_provenance = &record;
      }
    }

    // An interrupted query is a withheld result: the case must be able to name
    // the completion provenance that certifies it, in the case's own run.
    for (const Unknown& unknown : value_.unknowns) {
      if (!IsInterruptedQuery(unknown.reason_code)) {
        continue;
      }
      if (query_provenance == nullptr) {
        Add(EvidenceValidationCode::kMissingProvenance, unknown.id,
            Joined("interrupted query ", Quoted(unknown.id)) +
                " has no query-completion provenance in the case");
        continue;
      }
      if (!query_provenance->analysis_run_id.has_value()) {
        Add(EvidenceValidationCode::kMissingProvenance, unknown.id,
            Joined("the query-completion provenance for ", Quoted(unknown.id)) +
                " declares no analysis run");
      } else if (value_.program.analysis_run_id.has_value() &&
                 *query_provenance->analysis_run_id !=
                     *value_.program.analysis_run_id) {
        Add(EvidenceValidationCode::kMixedProgramContext, unknown.id,
            Joined("the query-completion provenance for ", Quoted(unknown.id)) +
                " belongs to another analysis run than the case binding");
      }
    }
  }

  void CheckStableIds() {
    for (const Entity& entity : value_.entities) {
      if (entity.stable_id.has_value() &&
          !IsCanonicalStableId(*entity.stable_id)) {
        Add(EvidenceValidationCode::kInvalidStableId, entity.id,
            Joined("entity ", Quoted(entity.id)) +
                " carries a non-canonical stable ID");
      }
    }
    for (const Fact& fact : value_.facts) {
      if (fact.stable_id.has_value() && !IsCanonicalStableId(*fact.stable_id)) {
        Add(EvidenceValidationCode::kInvalidStableId, fact.id,
            Joined("fact ", Quoted(fact.id)) +
                " carries a non-canonical stable ID");
      }
    }
    for (const SummaryReference& summary : value_.summaries) {
      if (!IsCanonicalStableId(summary.summary_id)) {
        Add(EvidenceValidationCode::kInvalidStableId, summary.id,
            Joined("summary reference ", Quoted(summary.id)) +
                " carries a non-canonical summary ID");
        continue;
      }
      if (summary.summary_id.kind != core::IdKind::kFunctionSummary) {
        Add(EvidenceValidationCode::kInvalidStableId, summary.id,
            Joined("summary reference ", Quoted(summary.id)) +
                " does not identify a function summary");
      }
    }
    for (const Dependency& dependency : value_.dependencies) {
      if (!IsCanonicalStableId(dependency.stable_id)) {
        Add(EvidenceValidationCode::kInvalidStableId, dependency.id,
            Joined("dependency ", Quoted(dependency.id)) +
                " carries a non-canonical stable ID");
      }
    }
  }

  void CheckProofAuthority() {
    std::size_t decided_results = 0;
    for (const ProofObligation& obligation : value_.proof_obligations) {
      const bool carries_result = !obligation.result_id.empty() ||
                                  !obligation.verification_producer.empty();
      switch (obligation.status) {
        case ProofStatus::kUnspecified:
          // Reported as a missing state.
          break;
        case ProofStatus::kPending:
          if (carries_result) {
            Add(EvidenceValidationCode::kVerificationProducer, obligation.id,
                Joined("pending obligation ", Quoted(obligation.id)) +
                    " carries a result that no verifier produced");
          }
          break;
        case ProofStatus::kUnknown:
          // An undecided result records no outcome; whether it names a
          // producer is the verifier's business.
          break;
        case ProofStatus::kProved:
        case ProofStatus::kRefuted:
        case ProofStatus::kTimeout:
        case ProofStatus::kUnsupported:
          if (obligation.result_id.empty() ||
              obligation.verification_producer.empty()) {
            Add(EvidenceValidationCode::kVerificationProducer, obligation.id,
                Joined("obligation ", Quoted(obligation.id)) +
                    " carries a decided result without both a result ID and a "
                    "verification producer");
          } else {
            ++decided_results;
          }
          break;
      }
    }
    // Only deterministic verification may claim a verified case state.
    if (IsVerifiedState(value_.verification_state) && decided_results == 0) {
      Add(EvidenceValidationCode::kVerificationProducer, "",
          Joined("the case claims verification state ",
                 ToString(value_.verification_state)) +
              " with no decided proof result");
    }
  }

  void ResolveEntity(const std::string& id, const std::string& owner,
                     std::string_view what) {
    if (id.empty()) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " is empty"));
      return;
    }
    const auto member = members_.find(id);
    if (member == members_.end()) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " ") + Quoted(id) + " is declared by no member");
      return;
    }
    if (member->second != MemberKind::kEntity) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " ") + Quoted(id) + " names a " +
              std::string(MemberKindName(member->second)) +
              ", not an entity");
    }
  }

  void ResolveProvenance(const std::string& id, const std::string& owner,
                         std::string_view what) {
    if (id.empty()) {
      return;
    }
    const auto member = members_.find(id);
    if (member == members_.end()) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " names provenance ") + Quoted(id) +
              ", which the case does not declare");
      return;
    }
    if (member->second != MemberKind::kProvenance) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " names ") + Quoted(id) + ", a " +
              std::string(MemberKindName(member->second)) +
              ", not a provenance record");
    }
  }

  void ResolveMember(const std::string& id, const std::string& owner,
                     std::string_view what) {
    if (members_.find(id) == members_.end()) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          Joined(what, " ") + Quoted(id) + " is declared by no member");
    }
  }

  // An expression reference resolves against the declared member set alone: a
  // reference names the identifier a member is declared under, and an entity
  // is addressable only under the handle it is declared with (`E_copy_length`),
  // never under the bare analysis label that handle wraps (`copy_length`).
  void ResolveReference(const std::string& text, const std::string& owner) {
    if (text.empty()) {
      Add(EvidenceValidationCode::kDanglingReference, owner,
          "an expression reference is empty");
      return;
    }
    if (members_.find(text) != members_.end()) {
      return;
    }
    Add(EvidenceValidationCode::kDanglingReference, owner,
        Joined("reference ", Quoted(text)) + " is declared by no member");
  }

  void ResolveExpression(const Expression& expression,
                         const std::string& owner) {
    if (expression.kind == Expression::Kind::kReference) {
      ResolveReference(expression.text, owner);
    }
    for (const Expression& operand : expression.operands) {
      ResolveExpression(operand, owner);
    }
  }

  void CheckReferences() {
    // The claim is about an entity, and the entity must exist.
    ResolveEntity(value_.primary_claim.subject, value_.primary_claim.id,
                  "the primary claim's subject");

    for (const Entity& entity : value_.entities) {
      for (const auto& property : entity.properties) {
        ResolveExpression(property.second, entity.id);
      }
    }
    for (const Edge& edge : value_.edges) {
      ResolveEntity(edge.from, edge.id, "the edge's source");
      ResolveEntity(edge.to, edge.id, "the edge's target");
      ResolveProvenance(edge.provenance_id, edge.id, "the edge");
      // An expandable edge's withheld expansion is checked by the omission
      // pass; a closed edge must still resolve the summary it names. An empty
      // attribute is absent, not dangling.
      if (!edge.expandable && !edge.summarized_by.empty()) {
        ResolveMember(edge.summarized_by, edge.id,
                      "the edge's summary reference");
      }
    }
    for (const Path& path : value_.paths) {
      for (const std::string& entity_id : path.entity_ids) {
        ResolveEntity(entity_id, path.id, "a path segment");
      }
      for (const Expression& condition : path.conditions) {
        ResolveExpression(condition, path.id);
      }
      ResolveProvenance(path.provenance_id, path.id, "the path");
    }
    for (const Fact& fact : value_.facts) {
      ResolveExpression(fact.predicate, fact.id);
      ResolveProvenance(fact.provenance_id, fact.id, "the fact");
    }
    for (const Assumption& assumption : value_.assumptions) {
      ResolveExpression(assumption.predicate, assumption.id);
    }
    for (const Hypothesis& hypothesis : value_.hypotheses) {
      ResolveExpression(hypothesis.predicate, hypothesis.id);
    }
    for (const Unknown& unknown : value_.unknowns) {
      ResolveExpression(unknown.property, unknown.id);
      for (const std::string& blocked : unknown.blocking_ids) {
        ResolveMember(blocked, unknown.id, "the blocked fact");
      }
    }
    for (const Constraint& constraint : value_.constraints) {
      ResolveExpression(constraint.expression, constraint.id);
      ResolveProvenance(constraint.provenance_id, constraint.id,
                        "the constraint");
    }
    for (const Provenance& record : value_.provenance) {
      for (const std::string& input : record.input_fact_ids) {
        ResolveMember(input, record.id, "the provenance input");
      }
    }
    for (const ProofObligation& obligation : value_.proof_obligations) {
      ResolveExpression(obligation.predicate, obligation.id);
      ResolveExpression(obligation.budget, obligation.id);
    }
    for (const SummaryReference& summary : value_.summaries) {
      ResolveEntity(summary.function_id, summary.id,
                    "the summarized function");
    }
    for (const Omission& omission : value_.omissions) {
      ResolveMember(omission.subject, omission.id, "the omission's subject");
    }
  }

  // --- Expression typing ---------------------------------------------------

  void CheckOperand(const Expression& expression, std::size_t index,
                    const std::string& owner, std::string_view position,
                    bool require_formula) {
    const Expression& operand = expression.operands[index];
    if (require_formula) {
      if (IsLiteralKind(operand.kind)) {
        Add(EvidenceValidationCode::kExpressionType, owner,
            Joined(position, ": a ") +
                Joined(ExpressionKindName(operand.kind),
                       " literal is not a predicate"));
      }
      return;
    }
    if (IsFormulaKind(operand.kind)) {
      Add(EvidenceValidationCode::kExpressionType, owner,
          Joined(position, ": a ") +
              Joined(ExpressionKindName(operand.kind),
                     " formula appears in a value position"));
    }
  }

  void CheckArity(const Expression& expression, const std::string& owner,
                  std::string_view position, std::size_t expected) {
    if (expression.operands.size() != expected) {
      Add(EvidenceValidationCode::kExpressionType, owner,
          Joined(position, ": a ") +
              Joined(ExpressionKindName(expression.kind), " expression takes ") +
              std::to_string(expected) + " operand(s) but carries " +
              std::to_string(expression.operands.size()));
    }
  }

  void CheckExpression(const Expression& expression,
                       const std::string& owner, std::string_view position,
                       bool optional, std::size_t depth) {
    if (depth > kMaxExpressionDepth) {
      Add(EvidenceValidationCode::kExpressionType, owner,
          Joined(position, ": nests deeper than validation walks"));
      return;
    }
    if (expression.kind == Expression::Kind::kUnspecified) {
      if (!optional) {
        Add(EvidenceValidationCode::kExpressionType, owner,
            Joined(position, ": declares no expression"));
      }
      return;
    }
    const std::size_t arity = expression.operands.size();
    switch (expression.kind) {
      case Expression::Kind::kUnspecified:
        break;
      case Expression::Kind::kBool:
      case Expression::Kind::kInteger:
      case Expression::Kind::kString:
      case Expression::Kind::kSymbol:
      case Expression::Kind::kReference:
        if (arity != 0) {
          Add(EvidenceValidationCode::kExpressionType, owner,
              Joined(position, ": a ") +
                  Joined(ExpressionKindName(expression.kind),
                         " expression takes no operands but carries ") +
                  std::to_string(arity));
        }
        break;
      case Expression::Kind::kCall:
        if (expression.text.empty()) {
          Add(EvidenceValidationCode::kExpressionType, owner,
              Joined(position, ": a call names no callee"));
        }
        break;
      case Expression::Kind::kNot:
        CheckArity(expression, owner, position, 1);
        if (arity == 1) {
          CheckOperand(expression, 0, owner, position, /*require_formula=*/true);
        }
        break;
      case Expression::Kind::kCompare:
        if (!IsComparisonOperator(expression.text)) {
          Add(EvidenceValidationCode::kExpressionType, owner,
              Joined(position, ": ") + Quoted(expression.text) +
                  " is not a comparison operator");
        }
        CheckArity(expression, owner, position, 2);
        if (arity == 2) {
          CheckOperand(expression, 0, owner, position, /*require_formula=*/false);
          CheckOperand(expression, 1, owner, position, /*require_formula=*/false);
        }
        break;
      case Expression::Kind::kAnd:
      case Expression::Kind::kOr:
        if (arity < 2) {
          Add(EvidenceValidationCode::kExpressionType, owner,
              Joined(position, ": a ") +
                  Joined(ExpressionKindName(expression.kind),
                         " expression takes two or more operands but carries ") +
                  std::to_string(arity));
        }
        for (std::size_t index = 0; index < arity; ++index) {
          CheckOperand(expression, index, owner, position,
                       /*require_formula=*/true);
        }
        break;
      case Expression::Kind::kImplies:
        CheckArity(expression, owner, position, 2);
        for (std::size_t index = 0; index < arity; ++index) {
          CheckOperand(expression, index, owner, position,
                       /*require_formula=*/true);
        }
        break;
      case Expression::Kind::kForAll:
      case Expression::Kind::kExists:
        if (expression.text.empty()) {
          Add(EvidenceValidationCode::kExpressionType, owner,
              Joined(position, ": a quantified expression binds no variable"));
        }
        CheckArity(expression, owner, position, 2);
        break;
    }
    for (const Expression& operand : expression.operands) {
      CheckExpression(operand, owner, position, /*optional=*/false, depth + 1);
    }
  }

  void CheckExpressions() {
    CheckExpression(value_.primary_claim.predicate, value_.primary_claim.id,
                    "the primary claim's predicate", /*optional=*/true, 0);
    for (const Entity& entity : value_.entities) {
      for (const auto& property : entity.properties) {
        CheckExpression(property.second, entity.id,
                        Joined("entity property ", Quoted(property.first)),
                        /*optional=*/false, 0);
      }
    }
    for (const Path& path : value_.paths) {
      for (const Expression& condition : path.conditions) {
        CheckExpression(condition, path.id, "a path condition",
                        /*optional=*/false, 0);
      }
    }
    for (const Fact& fact : value_.facts) {
      CheckExpression(fact.predicate, fact.id, "the fact's predicate",
                      /*optional=*/false, 0);
    }
    for (const Assumption& assumption : value_.assumptions) {
      CheckExpression(assumption.predicate, assumption.id,
                      "the assumption's predicate", /*optional=*/false, 0);
    }
    for (const Hypothesis& hypothesis : value_.hypotheses) {
      CheckExpression(hypothesis.predicate, hypothesis.id,
                      "the hypothesis's predicate", /*optional=*/false, 0);
    }
    for (const Unknown& unknown : value_.unknowns) {
      CheckExpression(unknown.property, unknown.id, "the unknown's property",
                      /*optional=*/false, 0);
    }
    for (const Constraint& constraint : value_.constraints) {
      CheckExpression(constraint.expression, constraint.id,
                      "the constraint's expression", /*optional=*/false, 0);
    }
    for (const ProofObligation& obligation : value_.proof_obligations) {
      CheckExpression(obligation.predicate, obligation.id,
                      "the proof goal", /*optional=*/false, 0);
      // The grammar's budget is optional: an absent one is `kUnspecified`.
      CheckExpression(obligation.budget, obligation.id, "the proof budget",
                      /*optional=*/true, 0);
    }
  }

  // --- Path connectivity ---------------------------------------------------

  bool HasSegment(const Path& path, const std::string& from,
                  const std::string& to) const {
    const std::optional<RelationKind> required =
        RelationForPathKind(path.kind);
    for (const Edge& edge : value_.edges) {
      if (edge.from != from || edge.to != to) {
        continue;
      }
      if (!required.has_value() || edge.kind == *required) {
        return true;
      }
    }
    return false;
  }

  void CheckPaths() {
    for (const Path& path : value_.paths) {
      if (path.entity_ids.size() < 2) {
        Add(EvidenceValidationCode::kPathDisconnected, path.id,
            Joined("path ", Quoted(path.id)) +
                " carries fewer than two segments");
        continue;
      }
      for (std::size_t index = 0; index + 1 < path.entity_ids.size();
           ++index) {
        const std::string& from = path.entity_ids[index];
        const std::string& to = path.entity_ids[index + 1];
        if (HasSegment(path, from, to)) {
          continue;
        }
        Add(EvidenceValidationCode::kPathDisconnected, path.id,
            Joined("path ", Quoted(path.id)) + " jumps from " + Quoted(from) +
                " to " + Quoted(to) + " with no connecting edge");
      }
    }
  }

  // --- Hypothesis isolation ------------------------------------------------

  void CheckHypothesisIsolation() {
    std::set<std::string> hypothesis_ids;
    for (const Hypothesis& hypothesis : value_.hypotheses) {
      if (!hypothesis.id.empty()) {
        hypothesis_ids.insert(hypothesis.id);
      }
    }
    if (hypothesis_ids.empty()) {
      return;
    }
    // A hypothesis never becomes an input to an authoritative derivation and
    // never blocks the resolution of an open question.
    for (const Provenance& record : value_.provenance) {
      for (const std::string& input : record.input_fact_ids) {
        if (hypothesis_ids.count(input) == 0) {
          continue;
        }
        Add(EvidenceValidationCode::kHypothesisAuthority, record.id,
            Joined("provenance ", Quoted(record.id)) +
                " derives from hypothesis " + Quoted(input));
      }
    }
    for (const Unknown& unknown : value_.unknowns) {
      for (const std::string& blocked : unknown.blocking_ids) {
        if (hypothesis_ids.count(blocked) == 0) {
          continue;
        }
        Add(EvidenceValidationCode::kHypothesisAuthority, unknown.id,
            Joined("unknown ", Quoted(unknown.id)) +
                " treats hypothesis " + Quoted(blocked) +
                " as blocking evidence");
      }
    }
  }

  // --- Omission visibility -------------------------------------------------

  const Omission* OmissionDeclaring(const std::string& local_id) const {
    if (local_id.empty()) {
      return nullptr;
    }
    for (const Omission& omission : value_.omissions) {
      if (omission.subject == local_id) {
        return &omission;
      }
    }
    return nullptr;
  }

  void CheckOmissionVisibility() {
    // A withheld expansion is declared, never implied by absence: an edge that
    // says it can be expanded must name a summary the case declares withheld.
    for (const Edge& edge : value_.edges) {
      if (!edge.expandable) {
        continue;
      }
      if (edge.summarized_by.empty()) {
        Add(EvidenceValidationCode::kHiddenOmission, edge.id,
            Joined("expandable edge ", Quoted(edge.id)) +
                " names no expansion target");
        continue;
      }
      const auto member = members_.find(edge.summarized_by);
      if (member == members_.end()) {
        Add(EvidenceValidationCode::kHiddenOmission, edge.id,
            Joined("expandable edge ", Quoted(edge.id)) +
                " names expansion target " + Quoted(edge.summarized_by) +
                ", which no member declares");
        continue;
      }
      if (member->second != MemberKind::kSummary) {
        Add(EvidenceValidationCode::kHiddenOmission, edge.id,
            Joined("expandable edge ", Quoted(edge.id)) +
                " names expansion target " + Quoted(edge.summarized_by) +
                ", a " + std::string(MemberKindName(member->second)) +
                ", not a summary reference");
        continue;
      }
      if (OmissionDeclaring(edge.summarized_by) == nullptr) {
        Add(EvidenceValidationCode::kHiddenOmission, edge.id,
            Joined("expandable edge ", Quoted(edge.id)) +
                " leaves summary " + Quoted(edge.summarized_by) +
                " withheld with no omission declaring it");
      }
    }

    // A withheld summary is recoverable at a higher level; declaring it
    // non-expandable loses the audit trail.
    for (const Omission& omission : value_.omissions) {
      const auto member = members_.find(omission.subject);
      if (member == members_.end() || member->second != MemberKind::kSummary) {
        continue;
      }
      if (!omission.expandable) {
        Add(EvidenceValidationCode::kHiddenOmission, omission.id,
            Joined("omission ", Quoted(omission.id)) + " withholds summary " +
                Quoted(omission.subject) + " as non-expandable");
      }
    }

    // An interrupted query is a withheld result: the case must declare it as
    // an expandable omission, not leave it as an open question alone.
    for (const Unknown& unknown : value_.unknowns) {
      if (!IsInterruptedQuery(unknown.reason_code)) {
        continue;
      }
      const Omission* declaring = OmissionDeclaring(unknown.id);
      if (declaring == nullptr) {
        Add(EvidenceValidationCode::kHiddenOmission, unknown.id,
            Joined("interrupted query ", Quoted(unknown.id)) +
                " is declared by no omission");
        continue;
      }
      if (!declaring->expandable) {
        Add(EvidenceValidationCode::kHiddenOmission, unknown.id,
            Joined("omission ", Quoted(declaring->id)) +
                " withholds interrupted query " + Quoted(unknown.id) +
                " as non-expandable");
      }
    }
  }

  const EvidenceCase& value_;
  EvidenceValidationReport report_;
  std::vector<std::pair<std::string, MemberKind>> declarations_;
  std::map<std::string, MemberKind> members_;
  std::map<std::string, const Provenance*> provenance_;
};

}  // namespace

std::string_view ToString(EvidenceValidationCode code) {
  switch (code) {
    case EvidenceValidationCode::kSchemaVersion:
      return "schema_version";
    case EvidenceValidationCode::kMissingProgramContext:
      return "missing_program_context";
    case EvidenceValidationCode::kPrimaryClaimCount:
      return "primary_claim_count";
    case EvidenceValidationCode::kDuplicateLocalId:
      return "duplicate_local_id";
    case EvidenceValidationCode::kDanglingReference:
      return "dangling_reference";
    case EvidenceValidationCode::kMissingEpistemic:
      return "missing_epistemic";
    case EvidenceValidationCode::kMissingProvenance:
      return "missing_provenance";
    case EvidenceValidationCode::kExpressionType:
      return "expression_type";
    case EvidenceValidationCode::kPathDisconnected:
      return "path_disconnected";
    case EvidenceValidationCode::kInvalidStableId:
      return "invalid_stable_id";
    case EvidenceValidationCode::kMixedProgramContext:
      return "mixed_program_context";
    case EvidenceValidationCode::kHypothesisAuthority:
      return "hypothesis_authority";
    case EvidenceValidationCode::kVerificationProducer:
      return "verification_producer";
    case EvidenceValidationCode::kHiddenOmission:
      return "hidden_omission";
  }
  return {};
}

EvidenceValidationReport ValidateEvidenceCase(const EvidenceCase& value) {
  CaseValidator validator(value);
  return validator.Run();
}

Status RequireValidEvidenceCase(const EvidenceCase& value) {
  const EvidenceValidationReport report = ValidateEvidenceCase(value);
  if (report.ok()) {
    return Status::Ok();
  }
  const EvidenceValidationIssue& issue = report.issues.front();
  std::string message = "invalid evidence case: ";
  message += ToString(issue.code);
  message += " (";
  message += issue.member_id;
  message += "): ";
  message += issue.message;
  return Status::InvalidArgument(std::move(message));
}

}  // namespace veritas::evidence
