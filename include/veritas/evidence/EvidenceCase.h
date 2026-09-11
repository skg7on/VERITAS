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

// EvidenceCase.h — the `eir.v1` Evidence IR semantic model.
//
// This is the typed model every M10C representation boundary encodes: the
// EIR-T lexer/parser/writer, the Protobuf codec, the diagnostic JSON emitter,
// the validator, and the canonicalizer all read and write these records and no
// others. It carries no textual syntax, no Protobuf generated types, and no
// analyzer types.
//
// Conventions that the whole model depends on:
//
//   * Two identifier spaces. A case-local identifier is a `std::string` and is
//     a syntax handle, valid only inside one case. A globally resolvable
//     VERITAS identity is a `core::StableId`, kept separately so cross-run and
//     cross-revision identity survives. Only `core::MakeStableId`,
//     `core::ToString`, and `core::ParseStableId` construct and parse IDs.
//   * Local identifiers are bare: the `@` of `@len` belongs to EIR-T syntax,
//     never to a model value. For example `Claim::subject` is "E_len".
//   * An empty string (or a disengaged `std::optional`) means "absent". The
//     grammar's optional attributes are modelled this way, so a writer omits
//     them and a reader leaves them empty.
//   * Every enum family that would otherwise have a plausible-looking "valid"
//     default carries `kUnspecified = 0`, the invalid default. It is
//     domain-only: no serializer emits it, and the Protobuf codec rejects every
//     `*_UNSPECIFIED`. `ToString` is total and renders it as "unspecified",
//     which no parser accepts as a grammar spelling. The families that reuse an
//     M10B type (`ClaimKind`, `Severity`) already own their sentinels.
//   * `Expression::text` carries only the operator, callee, or literal payload;
//     structure lives in `Expression::operands`. The per-kind operand contract
//     is documented on `Expression`.
//   * The textual enum spellings are the EIR-T 1.0 grammar's
//     (`docs/specs/veritas-evidence-ir-formal-specification.md`), not the
//     enumerator names: `EvidenceState` is UPPERCASE, `EvidenceLevel` and
//     `DependencyKind` are lowercase, and so on.
//
// Record declaration order is the canonical field order used by the
// canonicalizer and by both writer modes.

#ifndef VERITAS_EVIDENCE_EVIDENCE_CASE_H_
#define VERITAS_EVIDENCE_EVIDENCE_CASE_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/SliceTypes.h"

namespace veritas::evidence {

// --- Semantic enums ---------------------------------------------------------

// The EIR abstraction level. Textual spellings are lowercase: l0, l1, l2.
enum class EvidenceLevel {
  kUnspecified,
  kL0,
  kL1,
  kL2,
};

// The overall case verification state (EIR architecture §verification). Textual
// spellings are UPPERCASE. M10C emits only the initial states; the later
// authority-bearing transition machinery owns promotion.
enum class VerificationState {
  kUnspecified,
  kUnreviewed,
  kPossibleDefect,
  kLikelyDefect,
  kVerifiedDefect,
  kLikelyFalsePositive,
  kVerifiedSafe,
  kInconclusive,
};

// The initial entity kinds. Textual spellings are lowercase.
enum class EntityKind {
  kUnspecified,
  kFunction,
  kCallSite,
  kValue,
  kMemoryObject,
  kBasicBlock,
};

// The initial relation kinds. Textual spellings are UPPERCASE.
enum class RelationKind {
  kUnspecified,
  kCalls,
  kFlowsTo,
  kReads,
  kWrites,
  kDominates,
  kMayAlias,
};

// The initial path kinds. Textual spellings are lowercase.
enum class PathKind {
  kUnspecified,
  kCall,
  kValueFlow,
  kControl,
};

// The epistemic state of a fact. Independently of confidence, and never
// strengthened during assembly. Textual spellings are lowercase.
enum class EpistemicState {
  kUnspecified,
  kMust,
  kMay,
  kMustNot,
  kInferred,
  kAssumed,
  kUnknown,
};

// Confidence is independent of epistemic state. Textual spellings are
// lowercase.
enum class Confidence {
  kUnspecified,
  kExact,
  kHigh,
  kMedium,
  kLow,
  kUnknown,
};

// The status of a proof obligation. M10C only ever constructs `kPending`.
// Textual spellings are UPPERCASE.
enum class ProofStatus {
  kUnspecified,
  kPending,
  kProved,
  kRefuted,
  kUnknown,
  kTimeout,
  kUnsupported,
};

// One semantic input the case consumed. Textual spellings are lowercase.
enum class DependencyKind {
  kUnspecified,
  kSummary,
  kFact,
  kTypeLayout,
  kConfiguration,
  kSpecification,
};

// Path feasibility. M10C copies what the analysis established; it never
// upgrades `kMaybe` to `kSat`. Textual spellings are UPPERCASE.
enum class Feasibility {
  kUnspecified,
  kProvedFeasible,
  kSat,
  kMaybe,
  kUntested,
  kUnsat,
  kProvedInfeasible,
  kUnknown,
};

// The kind of goal a proof obligation requests. Textual spellings are
// lowercase.
enum class ProofGoalKind {
  kUnspecified,
  kProve,
  kRefute,
  kCheck,
};

// Textual spellings of the semantic enums, and their rejecting parsers. The
// parsers accept only the grammar's closed spelling sets; every unknown
// spelling, including the "unspecified" rendering of the invalid default, is
// rejected with InvalidArgument. `ToString` is total so it stays usable in
// diagnostics.
//
// The M10B textual enums (QueryCompleteness, TruncationReason, ClaimKind,
// Severity) keep their own helpers in SliceTypes.h; they are reused, not
// re-declared.
std::string_view ToString(EvidenceLevel value);
std::string_view ToString(VerificationState value);
std::string_view ToString(EntityKind value);
std::string_view ToString(RelationKind value);
std::string_view ToString(PathKind value);
std::string_view ToString(EpistemicState value);
std::string_view ToString(Confidence value);
std::string_view ToString(ProofStatus value);
std::string_view ToString(DependencyKind value);
std::string_view ToString(Feasibility value);
std::string_view ToString(ProofGoalKind value);

StatusOr<EvidenceLevel> ParseEvidenceLevel(std::string_view text);
StatusOr<VerificationState> ParseVerificationState(std::string_view text);
StatusOr<EntityKind> ParseEntityKind(std::string_view text);
StatusOr<RelationKind> ParseRelationKind(std::string_view text);
StatusOr<PathKind> ParsePathKind(std::string_view text);
StatusOr<EpistemicState> ParseEpistemicState(std::string_view text);
StatusOr<Confidence> ParseConfidence(std::string_view text);
StatusOr<ProofStatus> ParseProofStatus(std::string_view text);
StatusOr<DependencyKind> ParseDependencyKind(std::string_view text);
StatusOr<Feasibility> ParseFeasibility(std::string_view text);
StatusOr<ProofGoalKind> ParseProofGoalKind(std::string_view text);

// A byte offset within a source text plus its 1-based line and column. Used for
// EIR-T parse and validation diagnostics only: a source span never enters
// canonical bytes, so it cannot affect `EvidenceID`.
struct SourceSpan {
  std::size_t offset = 0;
  std::size_t line = 0;
  std::size_t column = 0;

  auto operator<=>(const SourceSpan&) const = default;
};

// --- Expressions ------------------------------------------------------------

// The typed predicate/property abstract syntax tree. One recursively owned
// value type serves every expression position (predicates, constraint
// expressions, entity properties, proof goals, budgets).
//
// Operand contract, per kind:
//
//   kBool       no operands; `boolean`
//   kInteger    no operands; `integer`
//   kString     no operands; `text`
//   kSymbol     no operands; `text` is an identifier (a bare name such as a
//               domain value or a constant symbol)
//   kReference  no operands; `text` is a case-local identifier without `@`
//   kCall       `text` is the callee; operands are the arguments in order
//   kNot        one operand
//   kCompare    `text` is the comparison operator ("==", "!=", "<", "<=", ">",
//               ">="); operands are {left, right}, never chained
//   kAnd, kOr   two or more operands, flattened
//   kImplies    operands are {antecedent, consequent} (right associative)
//   kForAll     `text` is the bound variable; operands are {domain, body}
//   kExists     `text` is the bound variable; operands are {domain, body}
//
// A validator checks arity and operand kinds against this contract; a parser
// rejects syntax it cannot lower into it.
struct Expression {
  enum class Kind {
    kUnspecified,
    kBool,
    kInteger,
    kString,
    kSymbol,
    kReference,
    kCall,
    kNot,
    kCompare,
    kAnd,
    kOr,
    kImplies,
    kForAll,
    kExists,
  };

  Kind kind = Kind::kUnspecified;
  std::string text;
  std::int64_t integer = 0;
  bool boolean = false;
  std::vector<Expression> operands;

  // `Expression` is the one recursively owned record in the model, and libc++
  // cannot synthesize the defaulted three-way comparison through a
  // `std::vector<Expression>` member: the element's operator is not yet viable
  // where the default is defined, so the default is implicitly deleted and with
  // it every containing record's. Both operators are therefore compared
  // field by field in the implementation, in this declaration's field order.
  bool operator==(const Expression& other) const;
  std::strong_ordering operator<=>(const Expression& other) const;
};

// --- Program identity -------------------------------------------------------

// One analyzer that contributed to the case.
struct AnalyzerVersion {
  std::string producer;
  std::string version;
  std::string configuration;

  auto operator<=>(const AnalyzerVersion&) const = default;
};

// The program context every case is bound to. It is a projection of the M10B
// `build::ProgramContext`; the checkout path never enters it. The textual
// fields mirror the EIR-T `ContextDecl` string properties, and follow the M10B
// `SnapshotDescriptor`/`ProgramContext` value types: a global identity that the
// analysis already carries as a `core::StableId` (the run) is typed as one, and
// the rest stay textual. A mixed-run input is rejected, never rebased.
struct ProgramBinding {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::string target_triple;
  std::string analysis_configuration_id;
  std::string type_layout_id;
  std::optional<core::StableId> analysis_run_id;
  std::vector<AnalyzerVersion> analyzer_versions;

  auto operator<=>(const ProgramBinding&) const = default;
};

// --- Graph members ----------------------------------------------------------

// An entity is a node of the evidence graph. `id` is the case-local handle;
// `stable_id` is the underlying VERITAS identity when one exists (synthetic
// entities have none). `properties` is the open, key-sorted property bag the
// EIR-T `EntityDecl` admits.
struct Entity {
  std::string id;
  EntityKind kind = EntityKind::kUnspecified;
  std::optional<core::StableId> stable_id;
  std::map<std::string, Expression> properties;

  auto operator<=>(const Entity&) const = default;
};

// A typed relation between two entities. `summarized_by` names the
// `SummaryReference` that summarizes this edge; an unexpanded summary edge is
// never a concrete path segment without its omission marker.
struct Edge {
  std::string id;
  std::string from;
  std::string to;
  RelationKind kind = RelationKind::kUnspecified;
  EpistemicState epistemic = EpistemicState::kUnspecified;
  std::string provenance_id;
  std::string summarized_by;
  bool expandable = false;

  auto operator<=>(const Edge&) const = default;
};

// An ordered path. `entity_ids` is a semantically ordered list: its sequence is
// the path and is never reordered by canonicalization. `conditions` are the
// per-path predicates that constrain it.
struct Path {
  std::string id;
  PathKind kind = PathKind::kUnspecified;
  std::vector<std::string> entity_ids;
  std::vector<Expression> conditions;
  Feasibility feasibility = Feasibility::kUnspecified;
  std::string provenance_id;

  auto operator<=>(const Path&) const = default;
};

// --- Claims and evidence members --------------------------------------------

// The single primary claim. `kind` and `severity` reuse the M10B-owned enums
// that cross the M10B/M10C boundary. `subject` names the entity the claim is
// about.
struct Claim {
  std::string id;
  ClaimKind kind = ClaimKind::kUnspecified;
  std::string subject;
  Expression predicate;
  Severity severity = Severity::kUnspecified;
  std::string description;

  auto operator<=>(const Claim&) const = default;
};

// One evidence fact. `derived` marks a fact the analysis derived rather than
// observed; every derived fact must carry resolvable provenance. `producer` is
// the stable producer identity (`analysis.value_range`).
struct Fact {
  std::string id;
  std::optional<core::StableId> stable_id;
  Expression predicate;
  EpistemicState epistemic = EpistemicState::kUnspecified;
  Confidence confidence = Confidence::kUnspecified;
  std::string producer;
  std::string provenance_id;
  bool derived = false;

  auto operator<=>(const Fact&) const = default;
};

// An explicitly assumed premise and where it came from.
struct Assumption {
  std::string id;
  Expression predicate;
  std::string source;
  std::string scope;

  auto operator<=>(const Assumption&) const = default;
};

// A candidate explanation that is not evidence: it stays outside the
// authoritative fact collection until a verifier promotes it.
struct Hypothesis {
  std::string id;
  Expression predicate;
  std::string producer;
  std::string reason;
  Confidence confidence = Confidence::kUnspecified;

  auto operator<=>(const Hypothesis&) const = default;
};

// A property the analysis could not establish. Absence of a fact is never
// converted into this member's negation: an unknown records that the question
// is open, and `blocking_ids` names the facts that block resolution.
struct Unknown {
  std::string id;
  Expression property;
  std::string reason;
  std::vector<std::string> blocking_ids;
  std::string suggested_resolution;

  auto operator<=>(const Unknown&) const = default;
};

// A predicate the case requires to hold over its scope.
struct Constraint {
  std::string id;
  Expression expression;
  std::string scope;
  EpistemicState epistemic = EpistemicState::kUnspecified;
  std::string provenance_id;

  auto operator<=>(const Constraint&) const = default;
};

// How one member came to be. `input_fact_ids` names the case-local facts that
// fed the derivation (empty for a root), `source_anchor_id` the M9 source
// anchor, and `analysis_run_id` the run that produced it — which must match the
// program binding of the case.
struct Provenance {
  std::string id;
  std::string producer;
  std::string rule;
  std::vector<std::string> input_fact_ids;
  std::string source_anchor_id;
  std::optional<core::StableId> analysis_run_id;
  std::string version;
  std::string configuration;

  auto operator<=>(const Provenance&) const = default;
};

// A typed goal requested of a verifier. `budget` is an `Expression`: kInteger
// for an integer resource budget, kCall for a `resource_budget(...)` call, and
// kUnspecified when the grammar's optional budget is absent. M10C constructs
// only PENDING obligations; a non-unknown result requires both `result_id` and
// `verification_producer`.
struct ProofObligation {
  std::string id;
  ProofGoalKind goal_kind = ProofGoalKind::kUnspecified;
  Expression predicate;
  std::vector<std::string> verifier_kinds;
  Expression budget;
  ProofStatus status = ProofStatus::kUnspecified;
  std::string result_id;
  std::string verification_producer;

  auto operator<=>(const ProofObligation&) const = default;
};

// A reference to the immutable `FunctionSummaryID` of `function_id`, plus the
// summary components the case consumed.
struct SummaryReference {
  std::string id;
  std::string function_id;
  core::StableId summary_id;
  std::vector<std::string> components;

  auto operator<=>(const SummaryReference&) const = default;
};

// One semantic input the case consumed, identified by kind and stable ID.
struct Dependency {
  std::string id;
  DependencyKind kind = DependencyKind::kUnspecified;
  core::StableId stable_id;

  auto operator<=>(const Dependency&) const = default;
};

// A member deliberately withheld at the declared level. A withheld member is
// never represented by its absence alone: `subject` names the referenced member
// or expansion target and `expandable` states whether a higher level can
// recover it.
struct Omission {
  std::string id;
  std::string kind;
  std::string subject;
  std::string reason;
  bool expandable = false;

  auto operator<=>(const Omission&) const = default;
};

// --- The case ---------------------------------------------------------------

// One `eir.v1` Evidence Case. `evidence_id` is the computed content address and
// is excluded from canonical bytes; it stays disengaged until
// `FinalizeEvidenceIdentity` succeeds.
struct EvidenceCase {
  std::optional<core::StableId> evidence_id;
  std::string schema_version;
  EvidenceLevel level = EvidenceLevel::kUnspecified;
  ProgramBinding program;
  Claim primary_claim;
  std::vector<Entity> entities;
  std::vector<Edge> edges;
  std::vector<Path> paths;
  std::vector<Fact> facts;
  std::vector<Assumption> assumptions;
  std::vector<Hypothesis> hypotheses;
  std::vector<Unknown> unknowns;
  std::vector<Constraint> constraints;
  std::vector<Provenance> provenance;
  std::vector<ProofObligation> proof_obligations;
  std::vector<SummaryReference> summaries;
  std::vector<Dependency> dependencies;
  VerificationState verification_state = VerificationState::kUnspecified;
  std::vector<Omission> omissions;

  auto operator<=>(const EvidenceCase&) const = default;
};

// The only accepted semantic schema version.
inline constexpr std::string_view kEvidenceSchemaVersion = "eir.v1";

// The M10C assembly request: one M10B handoff plus the program context it was
// taken from and the projection level to build. It lives here, with the model
// it produces and the level it selects, so the M10B handoff wrapper and the
// target model share one header; `EvidenceCaseBuilder` (M10C) consumes it.
struct EvidenceBuildRequest {
  build::ProgramContext context;
  EvidenceBuildInput input;
  EvidenceLevel level = EvidenceLevel::kUnspecified;

  // Deliberately no comparison operator: the request is a handoff, not a
  // canonical artifact. Neither `build::ProgramContext` (a checkout path) nor
  // the M10B `EvidenceBuildInput` declares ordering, and the case built from the
  // request is what comparisons are taken on.
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_CASE_H_
