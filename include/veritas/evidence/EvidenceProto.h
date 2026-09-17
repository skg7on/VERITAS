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

// EvidenceProto.h — the `eir.v1` Protobuf representation boundary.
//
// `proto/veritas/evidence/v1/evidence.proto` mirrors the semantic model in
// `EvidenceCase.h`; this header owns the explicit conversion between the two
// and the byte-level encode/decode built on it. Protobuf is one representation
// of a case, never its identity: wire bytes are not hashed, field order and
// unknown fields cannot reach `EvidenceID`, and a decoded case is
// re-canonicalized like any other, so all representations of one case share one
// identity (`EvidenceCanonicalizer.h`).
//
// The generated message types appear here and in `EvidenceProto.cpp` only.
// `EvidenceCase.h` and every other model header stay free of Protobuf, so the
// semantic model never depends on a wire format.
//
// PRESENCE AND LOSSLESSNESS
//
// The conversion is total over the model: every field of every record is
// carried, and `EncodeEvidenceProto` followed by `DecodeEvidenceProto`
// reproduces a case byte for byte, including its canonical bytes and its
// `EvidenceID`. Where the model legitimately expresses absence, absence is
// encoded as field absence rather than as a sentinel, so the two directions
// agree:
//
//   * An empty string is an absent `core::StableId` or an absent case-local
//     handle, exactly as in the model. A non-empty stable-ID string is parsed
//     with `core::ParseStableId`, and a string that does not parse is an
//     `InvalidArgument`.
//   * An absent `Claim::predicate` or `ProofObligation::budget` message is an
//     expression the grammar made optional, and decodes back to an
//     `Expression` of kind `kUnspecified`. A budget that is absent and one that
//     is empty are the same semantic value, so neither is manufactured into a
//     rejection.
//   * `Hypothesis::confidence` declares explicit presence, because
//     `Confidence::kUnspecified` is a legal state for a hypothesis and must
//     stay distinguishable from an explicitly encoded zero.
//
// REJECTION
//
// proto3 forces a zero-valued enumerator on every enum. Zero is never a
// semantic value for the enums this codec writes, so `FromEvidenceProto`
// rejects a zero-valued enum with `InvalidArgument` wherever the model declares
// no "unset" meaning — the case level and verification state, a claim kind or
// severity, an entity or relation kind, an epistemic state, a confidence, a
// path kind or feasibility, an unknown reason code, a proof goal kind or
// status, and a dependency kind. The exceptions are exactly the three absences
// listed above; there, a *present* field carrying zero is still rejected.
//
// A decoded case is validated before it is returned: `RequireValidEvidenceCase`
// must accept it, its `EvidenceID` is recomputed from its canonical bytes, and
// an encoded `evidence_id` that disagrees with that recomputation is an
// `InvalidArgument`. An empty `evidence_id` is an absent identity and is filled
// in by the recomputation rather than rejected.
//
// FAILURE MODES
//
// Every function fails only with `Status`/`StatusOr<T>`, and no failure returns
// a partially initialized case: a rejected decode yields no `EvidenceCase` at
// all. `ToEvidenceProto` and `EncodeEvidenceProto` validate their input and
// refuse a case that is not well-formed, so no serializer emits an invalid or
// zero-valued enum; `EncodeEvidenceProto` additionally refuses a case whose
// engaged `evidence_id` disagrees with its recomputed identity. A byte string
// that does not parse as an `EvidenceCase` message — malformed, truncated, or
// empty — is an `InvalidArgument`, as is one that parses into a case the model
// or the validator refuses.

#ifndef VERITAS_EVIDENCE_EVIDENCE_PROTO_H_
#define VERITAS_EVIDENCE_EVIDENCE_PROTO_H_

#include <string>
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/v1/evidence.pb.h"

namespace veritas::evidence {

// Converts `value` into its Protobuf representation. Every semantic field is
// carried, and no field is invented.
//
// Returns `InvalidArgument` when `value` is not a well-formed case, or when its
// engaged `evidence_id` disagrees with the identity recomputed from its
// canonical bytes. A case with no `evidence_id` converts with an empty
// `evidence_id` field.
StatusOr<veritas::evidence::v1::EvidenceCase> ToEvidenceProto(
    const EvidenceCase& value);

// Converts `value` back into the semantic model, rejecting anything the model
// cannot represent: a zero-valued enum in a required position, a stable-ID
// string that does not parse, or an `evidence_id` that disagrees with the
// identity recomputed from the decoded case's canonical bytes.
//
// The returned case is validated and carries its recomputed `evidence_id`.
StatusOr<EvidenceCase> FromEvidenceProto(
    const veritas::evidence::v1::EvidenceCase& value);

// The serialized form of `value`: `ToEvidenceProto` followed by
// `SerializeToString`. Wire bytes are a representation, not an identity, and
// are never a hash input.
//
// Fails with the same statuses as `ToEvidenceProto`, and with `Internal` if the
// message cannot be serialized.
StatusOr<std::string> EncodeEvidenceProto(const EvidenceCase& value);

// Parses `bytes` as an `EvidenceCase` message and converts it with
// `FromEvidenceProto`.
//
// Returns `InvalidArgument` when `bytes` is not a parsable `EvidenceCase`
// message, and otherwise the statuses `FromEvidenceProto` produces. On every
// failure no case is returned.
StatusOr<EvidenceCase> DecodeEvidenceProto(std::string_view bytes);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_PROTO_H_
