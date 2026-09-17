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

// EvidenceCanonicalizer.h — the one canonical byte encoding of an `eir.v1`
// case, and the `EvidenceID` derived from it.
//
// One case has exactly one canonical encoding, and therefore exactly one
// identity:
//
//   CanonicalEvidenceBytes(value) = CanonicalEncode(CanonicalValue(value))
//   EvidenceID                    = evidence:sha256:<digest of those bytes>
//
// The encoding is built from `core::CanonicalValue` and encoded by
// `core::CanonicalEncode`; Protobuf wire bytes, EIR-T text, diagnostic JSON,
// and source spans are never hash inputs. Every other M10C representation
// boundary (EIR-T, Protobuf, JSON) decodes into this model and re-canonicalizes
// to the same bytes, so all representations share one identity.
//
// WHAT IS ENCODED
//
// One root object carrying the case's semantic content, in the model's declared
// field order: `schema`, `level`, `state`, `program` (the whole program
// binding, analyzer versions included), `claim`, then every member collection,
// each one canonically ordered as described below. Every field of every record
// is encoded, with two deliberate exceptions:
//
//   * `EvidenceCase::evidence_id` is excluded. It is the output of this
//     function, so hashing it would be circular; a case that already carries an
//     identity canonicalizes exactly like one that does not, and
//     `FinalizeEvidenceIdentity` recomputes rather than trusts it.
//   * There is no second exception. `Claim::description`, provenance text,
//     `Omission::reason`, and the rest are model fields, so they are hashed.
//
// `core::CanonicalValue` has no filesystem-path carrier here: nothing in an
// `EvidenceCase` encodes as `core::Path` (`TaggedPath`), and untagged local
// paths are in any case rejected by `CanonicalEncode`. Path independence is
// structural, not a rule the encoder enforces — `ProgramBinding` has no
// member that can hold a checkout root, so two checkouts of the same revision
// produce byte-identical canonical input and therefore one identity. (The
// executable path-variation test belongs at the M10B-to-EIR assembly boundary,
// where a real checkout path exists.)
//
// CANONICAL ORDER
//
// Collections whose sequence is semantic keep it exactly:
//
//   * `Path::entity_ids` — the segment sequence *is* the path;
//   * `Expression::operands` for every kind except the commutative pair below
//     (comparison operands, implication antecedent/consequent, call arguments,
//     quantifier domain/body, `not`'s operand).
//
// Collections that denote an unordered set are sorted by a total, deterministic
// key. The key is `(kind, canonical stable-ID string, local ID)`, where `kind`
// is the record's declared kind family (`EntityKind`, `RelationKind`,
// `PathKind`, `DependencyKind`, `ProofGoalKind`) or `0` for a record with no
// kind family, the stable-ID component is `core::ToString(stable_id)` when the
// `std::optional` is engaged and the empty string when it is not, and the local
// ID component is the record's case-local handle. Because a record can have no
// identity at all (`AnalyzerVersion` has neither a kind nor any ID), the key is
// completed by the record's own canonical encoding as the final tie-break, so
// the order is total and never depends on the input order. Applying it:
//
//   * members — `entities`, `edges`, `paths`, `facts`, `assumptions`,
//     `hypotheses`, `unknowns`, `constraints`, `provenance`,
//     `proof_obligations`, `summaries`, `dependencies`, `omissions`;
//   * reference lists — `Unknown::blocking_ids`,
//     `Provenance::input_fact_ids`, `SummaryReference::components`,
//     `ProofObligation::verifier_kinds` (sorted lexicographically by value,
//     duplicates preserved);
//   * `Path::conditions` — a path's conditions constrain it conjunctively, so
//     the set is ordered canonically like an `and` over the same predicates;
//   * `ProgramBinding::analyzer_versions`.
//
// `Entity::properties` is key-sorted structurally: it is a `std::map` and
// encodes as a `CanonicalObject`, whose member order `CanonicalEncode` fixes.
//
// EXPRESSION NORMALIZATION
//
// Expression trees are normalized recursively. Every operand is normalized
// first, then one rule applies: a commutative `kAnd`/`kOr` node orders its
// operands by their canonical encodings, so `a and b` and `b and a` are one
// case. Nothing else is reordered and no algebraic rewriting is performed:
// a comparison stays left-to-right, implication keeps its antecedent and
// consequent, call arguments keep their positions, quantifier bodies keep their
// binding, and a nested `and` inside an `and` keeps its structure (only the
// operand order among the node's own children is normalized).
//
// FAILURE MODES
//
// `CanonicalEvidenceBytes`, `ComputeEvidenceId`, and `FinalizeEvidenceIdentity`
// fail only with `Status`/`StatusOr<T>`. `CanonicalEvidenceBytes` and
// `ComputeEvidenceId` are pure: they do not validate the case and do not mutate
// it, and they encode whatever the case declares, so a caller that needs a
// well-formed case must validate first (or use `FinalizeEvidenceIdentity`,
// which does). `FinalizeEvidenceIdentity` returns the validator's
// `InvalidArgument` for a case that is not well-formed. Every failure path
// leaves the case byte-for-byte unchanged, including its `evidence_id`.

#ifndef VERITAS_EVIDENCE_EVIDENCE_CANONICALIZER_H_
#define VERITAS_EVIDENCE_EVIDENCE_CANONICALIZER_H_

#include <cstddef>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

// The canonical byte encoding of `value`, excluding `value.evidence_id`.
//
// Pure and order-independent: reordering any semantically unordered collection
// in `value` produces identical bytes, while any change to the case's semantic
// content produces different bytes. The case may be invalid; validating is the
// caller's contract.
//
// Fails only if the value cannot be canonically encoded, which this encoding
// never produces (it contains no path carrier).
StatusOr<std::vector<std::byte>> CanonicalEvidenceBytes(
    const EvidenceCase& value);

// The content address of `value`: `core::IdKind::kEvidence` over
// `CanonicalEvidenceBytes(value)`, spelled `evidence:sha256:<digest>`.
//
// Does not validate `value`; the bytes it hashes are exactly those
// `CanonicalEvidenceBytes` returns, so a case and its re-decoded
// representations agree.
StatusOr<core::StableId> ComputeEvidenceId(const EvidenceCase& value);

// Validates `value`, computes its identity, and assigns it — in that order,
// and the assignment happens only after every earlier step succeeded, so a
// failed call never leaves a half-written identity behind.
//
// Returns `InvalidArgument` carrying the validator's first issue when `value`
// violates the well-formedness contract, and `InvalidArgument` when `value` is
// null. On success `value->evidence_id` is the freshly computed identity, which
// replaces any identity the case already carried.
Status FinalizeEvidenceIdentity(EvidenceCase* value);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_CANONICALIZER_H_
