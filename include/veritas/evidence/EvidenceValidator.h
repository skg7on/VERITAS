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

// EvidenceValidator.h — the `eir.v1` well-formedness gate.
//
// `ValidateEvidenceCase` is the single semantic gate every later M10C stage
// runs through: assembly, canonicalization, both writer modes, the Protobuf
// codec, the diagnostic JSON emitter, and the CLI all refuse a case this
// function rejects. It is pure and read-only — it takes the case by const
// reference, never mutates it, never fetches, and never synthesizes the
// provenance or the members a case is missing. A missing member stays missing
// and is reported.
//
// The checks implement the formal specification's well-formedness constraints
// (docs/specs/veritas-evidence-ir-formal-specification.md §16) and the M10C
// design spec's validation list (§7). Concretely:
//
//   * Header — the schema version is the one accepted version, and the case
//     declares an abstraction level.
//   * Program context — the binding is complete and carries the analysis run
//     the case belongs to.
//   * Primary claim — the case declares exactly one.
//   * Local identity — every case-local identifier is declared once, in one
//     flat identifier space shared by every member kind.
//   * Explicit states — the epistemic, confidence, kind, feasibility, and
//     status families a case depends on are declared, never left at their
//     invalid `kUnspecified` default.
//   * Provenance — every derived fact resolves to a declared provenance
//     record, every provenance record belongs to the case's analysis run, and
//     an interrupted query is traceable to the query-completion provenance
//     that certifies it.
//   * Stable identity — every engaged `core::StableId` is canonical and
//     re-parses, and a summary reference identifies a function summary.
//   * References — every reference a case carries (claim subject, edge
//     endpoints, expression references, path segments, provenance and summary
//     links, blocking facts, provenance inputs, omission subjects) resolves.
//   * Expressions — every expression obeys the per-kind operand contract of
//     `Expression`, with no formula in a value position and no value literal
//     in a formula position.
//   * Paths — a path has at least one segment and every consecutive pair is
//     joined by an edge of the relation kind its path kind implies.
//   * Proof authority — a pending obligation carries no result, a decided
//     result names both its result ID and its verification producer, and an
//     authoritatively verified case state is backed by a decided result.
//   * Hypothesis isolation — a hypothesis never feeds an authoritative
//     derivation or blocks an open question.
//   * Visible omissions — withheld detail is declared, never implied by
//     absence.
//
// An expression reference resolves to a declared member, and also to the bare
// analysis label an entity handle wraps (`copy_length` for `E_copy_length`),
// because `eir.v1` predicates use both spellings.
//
// A note on the model's shape, because it fixes what "declared" can mean: the
// only member that can declare a withheld target is `Omission`, whose
// `subject` names the withheld member or expansion target. An `Unknown`
// declares an open question, not a withheld member, and carries no stable link
// to a query certificate: `Dependency` holds a `core::StableId` while
// `Omission::subject` is a case-local handle, so no member can name a
// dependency by its stable identity. A truncated query is therefore checked
// through the pair the case can actually express — the interrupted `Unknown`
// and the `Omission` that declares it — and the validator never reaches
// outside the case to close that gap.
//
// Diagnostics are stable: `EvidenceValidationCode` names are the diagnostic
// contract (snake_case, as `veritas_evidence` spells every other textual
// enum), each issue carries the member it is about, and `SourceSpan` is
// present only when a syntax tree supplied one. A case with no issues is
// well-formed; `RequireValidEvidenceCase` is the single-status form for
// callers that only need to accept or refuse.

#ifndef VERITAS_EVIDENCE_EVIDENCE_VALIDATOR_H_
#define VERITAS_EVIDENCE_EVIDENCE_VALIDATOR_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

// The stable diagnostic classification. Every rejection is one of these; no
// rejection is reported by message text alone.
enum class EvidenceValidationCode {
  kSchemaVersion,
  kMissingProgramContext,
  kPrimaryClaimCount,
  kDuplicateLocalId,
  kDanglingReference,
  kMissingEpistemic,
  kMissingProvenance,
  kExpressionType,
  kPathDisconnected,
  kInvalidStableId,
  kMixedProgramContext,
  kHypothesisAuthority,
  kVerificationProducer,
  kHiddenOmission,
};

// The stable snake_case spelling of a diagnostic code, as it appears in
// `RequireValidEvidenceCase`'s message and in diagnostic JSON.
std::string_view ToString(EvidenceValidationCode code);

// One rejection: the stable code, the member it is about (empty for a
// case-level issue such as the schema version), a human-readable sentence, and
// the source span when the case was produced by a parser that carried one.
struct EvidenceValidationIssue {
  EvidenceValidationCode code;
  std::string member_id;
  std::string message;
  std::optional<SourceSpan> source_span;
};

// Every issue a case produced, in pass-then-declaration order. `ok()` is the
// only acceptance test; an empty report is a well-formed case.
struct EvidenceValidationReport {
  std::vector<EvidenceValidationIssue> issues;
  bool ok() const { return issues.empty(); }
};

// Validates `value` and returns every issue found. Never mutates `value` and
// never fetches or synthesizes anything it does not declare.
EvidenceValidationReport ValidateEvidenceCase(const EvidenceCase& value);

// The single-status form: `Status::Ok()` for a well-formed case, otherwise
// `Status::InvalidArgument` carrying the first issue's stable code name and
// member ID. Callers that need every failure use `ValidateEvidenceCase`.
Status RequireValidEvidenceCase(const EvidenceCase& value);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_VALIDATOR_H_
