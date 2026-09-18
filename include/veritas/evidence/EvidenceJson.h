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

// EvidenceJson.h — the full-fidelity JSON encoding of an `eir.v1` case.
//
// This is the *semantic* JSON boundary, and it is deliberately not the
// diagnostic JSON that `SliceTypes.h` declares. `evidence::ToDiagnosticJson`
// is a hand-rolled, hand-indented projection of the M10B *slice* types written
// for `veritas-query --format json`; it carries no `llvm::json` dependency, it
// renders the slice the query returned rather than the case the case can
// prove, and it drops everything the query did not look at. Nothing here
// reuses it and nothing here changes it.
//
// WHAT THIS EMITS
//
// Every field of every record in `EvidenceCase`, including the ones a
// diagnostic view has no use for: `Claim::description`, `Assumption::source`,
// `Unknown::suggested_resolution`, the whole `ProgramBinding` (analyzer
// versions included), both `Omission` reason strings, and every `Expression`
// node of every predicate in the case. The invariant this writer keeps is the
// one the EIR-T writer keeps, in a different syntax: a field the model carries
// is a field the document carries. A member that is empty because the model
// says "absent" is emitted as absent — `null` for a disengaged optional, the
// empty string for an empty string — and never as a key that quietly vanishes,
// so a reader can tell "the case records nothing here" from "this writer
// forgot".
//
// ORDER
//
// The output is a pure function of the case's *meaning*, not of the order the
// case happened to be assembled in. Two rules produce that:
//
//   * Object members are emitted in the `std::map` order `llvm::json::OStream`
//     fixes for an object — lexicographic by key — which is the same order
//     `core::CanonicalEncode` emits a `core::CanonicalObject` in, and the same
//     convention `build::ToDiagnosticJson` documents for its own output.
//   * Every collection the canonicalizer treats as an unordered set is emitted
//     in the canonicalizer's own order, so the two boundaries agree about what
//     the case is. The key is the one `EvidenceCanonicalizer.cpp` documents:
//     `(kind spelled as the payload spells it, canonical stable-ID string,
//     case-local handle)`. Concretely — entities and facts carry both a kind
//     family and a stable identity where they have one, edges, paths, proof
//     obligations, and dependencies carry a kind family, and the remaining
//     records sort on their local handle alone.
//
// Three sequences are *not* reordered, because the model's order is what they
// mean: `Path::entity_ids` (the segment sequence is the path), the operands of
// a non-commutative expression (`kCompare`'s {left, right}, `kImplies`'s
// antecedent and consequent, `kForAll`/`kExists`'s domain and body, and a
// call's arguments), and the members of `Entity::properties`, which is already
// a key-sorted `std::map`.
//
// The four reference lists the model declares sorted — `Unknown::blocking_ids`,
// `Provenance::input_fact_ids`, `SummaryReference::components`, and
// `ProofObligation::verifier_kinds` — are sorted here too, and
// `Path::conditions` and the operands of `kAnd`/`kOr` are ordered by their own
// encoding, which is what §19.1 and the canonicalizer's expression
// normalization require. `ProgramBinding::analyzer_versions` sorts on
// `(producer, version, configuration)`.
//
// The canonicalizer breaks a key tie with the record's own canonical encoding.
// This writer cannot reach that encoding — `EvidenceCanonicalizer.cpp` keeps it
// private — so it relies on the key being total instead, which it is for every
// case this function accepts: `ValidateEvidenceCase` refuses a case whose
// members share a local identifier, and the local handle is the last component
// of every one of those keys. A case whose collection order could vary is
// therefore a case this boundary refuses rather than serializes.
//
// ENUM AND VALUE SPELLINGS
//
// Every enum is emitted through the model's own `ToString`, so the JSON spells
// a value exactly as EIR-T spells it and the corresponding `Parse*` function
// reads it back. That includes the M10B-owned `ClaimKind` and `Severity`. The
// spellings are not uniformly lower-case and are not meant to be: the grammar
// spells `EvidenceLevel` and `EpistemicState` lower-case and
// `VerificationState` and `RelationKind` upper-case, and a JSON that
// re-cased them would be a second, disagreeing vocabulary for one model.
//
// `Expression` is rendered structurally and uniformly — `kind`, `text`,
// `integer`, `boolean`, `operands` — for every node, leaves included, so a
// consumer never has to know which fields a kind "uses" to read the tree. The
// sentinel `Expression::Kind::kUnspecified` is spelled `"unspecified"` like
// every other sentinel the model renders, which is how the grammar's absent
// proof budget appears: the model has exactly one way to say "no budget" and
// this preserves it.
//
// REFUSALS
//
// This is the second representation boundary and it refuses exactly what the
// first one refuses. `WriteEirText` documents the contract in
// `EirText.h`'s FAILURE MODES block, and the conditions, their order, and
// their `InvalidArgument` code are mirrored here:
//
//   * a case carrying no `evidence_id` — it has not been finalized, so its
//     identity is not yet its own;
//   * a case that is not well-formed, reported with `RequireValidEvidenceCase`'s
//     own status;
//   * a case whose `evidence_id` is not its recomputed content address;
//   * a case holding a value this boundary cannot spell.
//
// The fourth is narrower than the EIR-T writer's and wider in exactly one
// place. Narrower: EIR-T refuses a producer string outside the `QualifiedId`
// alphabet, and this writer does **not**. That alphabet is the grammar's
// constraint, not the model's — JSON has no `Producer` production, a JSON
// string carries any byte sequence, and imposing the alphabet here would
// invent a restriction the format does not have and make the fixture
// `MakeOverflowEvidenceCase()`, whose witnesses carry the producer
// `"evidence-query"`, unwritable. Wider: a string that is not valid UTF-8 is
// refused, because JSON text is UTF-8 by definition (RFC 8259 §8.1) and
// `llvm::json` asserts on such a value rather than escaping it. A crash is not
// a refusal, so the check is explicit. Everything either writer can spell with
// ASCII, hyphens and all, both writers accept.
//
// FAILURE MODES
//
// Returns `InvalidArgument` for each of the four conditions above and never
// `Ok` with a partial document. Every failure leaves `value` unchanged — the
// writer takes it by const reference, never mutates it, and computes its
// identity by re-canonicalizing rather than trusting the field.
//
// On success the returned string is a single JSON document with **exactly one**
// trailing newline, which is the layout `build::ToDiagnosticJson` produces and
// the layout a golden file is pinned against.

#ifndef VERITAS_EVIDENCE_EVIDENCE_JSON_H_
#define VERITAS_EVIDENCE_EVIDENCE_JSON_H_

#include <string>

#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

// Writes one `eir.v1` case as full-fidelity JSON, with one trailing newline.
//
// `value` must be a validated, finalized case: that is, one
// `FinalizeEvidenceIdentity` has accepted. The resulting document carries
// `value.evidence_id` as its recomputed content address, which is what makes
// the JSON readable back to the same case that `WriteEirText` and the Protobuf
// codec write.
//
// `value` is not mutated. See the file header for the emitted shape, the
// ordering contract, and the refusal conditions.
StatusOr<std::string> ToEvidenceJson(const EvidenceCase& value);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_JSON_H_
