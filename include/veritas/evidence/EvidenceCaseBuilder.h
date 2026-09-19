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

// EvidenceCaseBuilder.h — assemble one M10B `EvidenceBuildInput` into a
// validated L0/L1/L2 `EvidenceCase`.
//
// The builder is the M9→EIR boundary. It reads the typed handoff, lowers the
// relations the mapper knows, and refuses the ones it does not; it never calls
// the FactStore, the ProvenanceStore, the CPG, or any analyzer, and it never
// queries a backend (the spec's "no M10C code path may query the FactStore").
// Everything it emits is a function of the request alone, so two builds of the
// same request produce byte-identical canonical output and reversing the
// insertion order of any input vector changes nothing.
//
// WHAT THE REQUEST CANNOT SUPPLY, THE CASE DOES NOT CLAIM.
//
// The handoff carries entities, edges, facts, completion certificates, run
// bindings, and selected witnesses. It does **not** carry analyzer rule IDs,
// M9 source anchors, summary IDs, or threat-model assumptions; those are
// hand-authored in the DEM fixtures and have no derivation here. The builder
// therefore emits no `Assumption`, no `Hypothesis`, no `SummaryReference`, and
// provenance whose rule and anchor come from the witness it holds — never from
// a literal. Inventing them would make the case assert a derivation nobody
// performed.
//
// THE BUILDER NEVER RESOLVES AN OPEN QUESTION BY ASSERTING ITS NEGATION.
//
// A complete, empty, correctly-scoped closed-world query may derive
// `MUST_NOT dominates_bounds_check(...)`. Everything weaker — a truncated
// result, a mismatched scope or run, a certificate the handoff does not carry,
// a missing selected witness — leaves the question open and becomes an explicit
// `Unknown` plus an `Omission`, never a negative fact (BLD-006, BLD-007).
//
// One input is a malformation rather than an open question. A certificate whose
// cells no longer re-derive their own content-addressed identity was patched
// after publication, and no case may rest on one; that is refused outright for
// every certificate the handoff carries, before any of them is used.

#ifndef VERITAS_EVIDENCE_EVIDENCE_CASE_BUILDER_H_
#define VERITAS_EVIDENCE_EVIDENCE_CASE_BUILDER_H_

#include <string>
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

// The EIR producer a query-completion witness is named under.
//
// The M9 witness carries `producer_id = "evidence-query"` (M10B's
// `kQueryCompletionProducerId`), and a hyphen is outside the EIR-T
// `QualifiedId` alphabet. `Producer` is deliberately not widened (ruling L43),
// so that string has no EIR spelling at all; the builder is the layer that
// names things in EIR's vocabulary, and it names this one `evidence.query` —
// the spelling its sibling rule `evidence.query_completion.v1` already uses
// (ruling L49).
//
// This is a translation, not a drop: a mapper that copied the M9 string through
// would produce a case that cannot be serialized as EIR-T at all.
inline constexpr std::string_view kEvidenceQueryProducerId = "evidence.query";

// The producer and rule of the one derivation the builder performs itself: the
// closed-world absence of a dominating bounds check.
inline constexpr std::string_view kClosedWorldProducerId =
    "evidence.closed_world";
inline constexpr std::string_view kClosedWorldAbsenceRuleId =
    "evidence.closed_world.dominating_check_absence.v1";

// Translates an M9 witness producer identifier into its EIR spelling.
// `"evidence-query"` becomes `"evidence.query"`; every identifier that already
// has a legal `QualifiedId` spelling is returned unchanged, so the translation
// is total and idempotent and never silently drops a producer.
std::string TranslateProducer(std::string_view m9_producer_id);

class EvidenceCaseBuilder {
 public:
  // Assembles `request` into a validated case at `request.level`.
  //
  // The result has passed `RequireValidEvidenceCase` and carries its computed
  // `evidence_id`; a case that cannot be validated is never returned.
  //
  // Fails with `InvalidArgument` when:
  //   * the declared level is `kUnspecified`;
  //   * any program-identity string the binding requires is empty;
  //   * the claim kind has no M10C construction (only `kBufferOverflow` does);
  //   * the input mixes analysis runs — across the flow slice, the fact sets,
  //     the provenance graph, or the completion bindings (BLD-010);
  //   * a fact carries a relation the mapper has no EIR predicate for, or its
  //     row does not satisfy its own relation schema (BLD-010);
  //   * a fact references a stable ID the case cannot declare an entity for.
  //
  // Never fails on an *open* dominating-check query: absence of evidence is
  // recorded as an unknown or an omission, not as an error and not as a
  // negative fact.
  StatusOr<EvidenceCase> Build(const EvidenceBuildRequest& request) const;
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_CASE_BUILDER_H_
