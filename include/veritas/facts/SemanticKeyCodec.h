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

// SemanticKeyCodec.h — the versioned, injective semantic-key encoding.
//
// A witness edge names its result and input by semantic key, so the key is
// part of the durable evidence protocol and both engines must produce the same
// bytes. Every field is type-tagged and length-prefixed: decoding is driven by
// the length and never scans for a delimiter, so a cell may contain any bytes
// -- including the codec's own punctuation -- without being able to move a
// field boundary. That is what makes "a"+"bc" distinguishable from "ab"+"c".
//
// The field functions here are the primitives a Souffle stateful functor
// exposes to the generated program (M8R.4), so only encoded fields are ever
// concatenated. Concatenating raw cells with a delimiter is forbidden.

#ifndef VERITAS_FACTS_SEMANTIC_KEY_CODEC_H_
#define VERITAS_FACTS_SEMANTIC_KEY_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"

namespace veritas::facts {

// The codec version. Any change to the encoding requires a new tag.
inline constexpr std::string_view kSemanticKeyVersion = "veritas.semantic-key.v1";

// A field's domain. Tags keep domains apart, so the symbol "1" and the number
// 1 never encode alike, and a stable id is never mistaken for a plain string
// that happens to have the same text.
enum class KeyFieldTag : char {
  kId = 'I',
  kSymbol = 'S',
  kNumber = 'N',
  kUnsigned = 'U',
  kEnum = 'E',
};

struct KeyField {
  KeyFieldTag tag;
  std::string value;

  auto operator<=>(const KeyField&) const = default;
  bool operator==(const KeyField&) const = default;
};

std::string EncodeField(KeyFieldTag tag, std::string_view value);
std::string EncodeIdField(std::string_view stable_id);
std::string EncodeSymbolField(std::string_view value);
std::string EncodeNumberField(std::int64_t value);
std::string EncodeUnsignedField(std::uint64_t value);
std::string EncodeEnumField(std::uint64_t ordinal);

// Opens a key: the codec version, the relation name, and the arity. Two
// relations sharing a column shape therefore cannot collide.
std::string EncodeKeyHeader(std::string_view relation_name, std::size_t arity);

// Decodes a concatenation of encoded fields. Rejects an unknown tag, a missing
// or non-numeric length, a negative length, and a length that runs past the
// end of the input -- a malformed key must fail rather than decode as a
// shorter valid one.
StatusOr<std::vector<KeyField>> DecodeFields(std::string_view encoded);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_SEMANTIC_KEY_CODEC_H_
