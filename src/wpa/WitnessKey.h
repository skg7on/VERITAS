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

// WitnessKey.h — the one decoder from an encoded witness key to a semantic row.
//
// A witness key is durable evidence: it names the result a derivation produced
// and the input it produced it from, in the codec's own length-prefixed,
// type-tagged field grammar (SemanticKeyCodec.h). The bundles build those keys
// with the pinned functors, and two consumers read them back — the file-backed
// relation reader and the in-memory session executor. Both decode here, so
// there is exactly one decoder: a second copy would be kept in step by nothing
// but a test, and a divergence would silently corrupt the provenance record
// rather than fail the build.
//
// The relation's schema decides how each field is interpreted, so a key whose
// field tags disagree with the relation it names is rejected rather than
// coerced into a plausible-looking fact.

#ifndef VERITAS_WPA_WITNESS_KEY_H_
#define VERITAS_WPA_WITNESS_KEY_H_

#include <cstdint>
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::wpa {

// Decimal text to an unsigned integer. Rejects an empty string and any
// character that is not a decimal digit.
StatusOr<std::uint64_t> ParseUnsigned(std::string_view text);

// Decimal text to a signed integer, with an optional leading '-'.
StatusOr<std::int64_t> ParseSigned(std::string_view text);

// Rebuilds a semantic row from an encoded witness key. Fails on a malformed
// key, on a key naming an unknown relation, on an arity or field tag that
// disagrees with that relation's schema, and on a row that does not pass
// semantic validation.
StatusOr<facts::SemanticRow> RowFromKey(std::string_view key);

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WITNESS_KEY_H_
