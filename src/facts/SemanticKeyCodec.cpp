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

#include "veritas/facts/SemanticKeyCodec.h"

#include <limits>

namespace veritas::facts {

namespace {

bool IsKnownTag(char tag) {
  switch (static_cast<KeyFieldTag>(tag)) {
  case KeyFieldTag::kId:
  case KeyFieldTag::kSymbol:
  case KeyFieldTag::kNumber:
  case KeyFieldTag::kUnsigned:
  case KeyFieldTag::kEnum:
    return true;
  }
  return false;
}

}  // namespace

std::string EncodeField(KeyFieldTag tag, std::string_view value) {
  std::string encoded;
  encoded.push_back(static_cast<char>(tag));
  encoded.append(std::to_string(value.size()));
  encoded.push_back(':');
  encoded.append(value);
  return encoded;
}

std::string EncodeIdField(std::string_view stable_id) {
  return EncodeField(KeyFieldTag::kId, stable_id);
}

std::string EncodeSymbolField(std::string_view value) {
  return EncodeField(KeyFieldTag::kSymbol, value);
}

std::string EncodeNumberField(std::int64_t value) {
  return EncodeField(KeyFieldTag::kNumber, std::to_string(value));
}

std::string EncodeUnsignedField(std::uint64_t value) {
  return EncodeField(KeyFieldTag::kUnsigned, std::to_string(value));
}

std::string EncodeEnumField(std::uint64_t ordinal) {
  return EncodeField(KeyFieldTag::kEnum, std::to_string(ordinal));
}

std::string EncodeKeyHeader(std::string_view relation_name, std::size_t arity) {
  std::string header(kSemanticKeyVersion);
  header.append(EncodeSymbolField(relation_name));
  header.append(EncodeUnsignedField(static_cast<std::uint64_t>(arity)));
  return header;
}

StatusOr<std::vector<KeyField>> DecodeFields(std::string_view encoded) {
  std::vector<KeyField> fields;
  std::size_t cursor = 0;
  while (cursor < encoded.size()) {
    const char tag = encoded[cursor];
    if (!IsKnownTag(tag)) {
      return Status::InvalidArgument("semantic key has an unknown field tag");
    }
    ++cursor;

    // The length must be a non-empty run of digits. A leading '-' or an empty
    // run is malformed, not a zero-length field.
    const std::size_t digits_begin = cursor;
    while (cursor < encoded.size() && encoded[cursor] >= '0' &&
           encoded[cursor] <= '9') {
      ++cursor;
    }
    if (cursor == digits_begin) {
      return Status::InvalidArgument("semantic key field has no length");
    }
    if (cursor >= encoded.size() || encoded[cursor] != ':') {
      return Status::InvalidArgument(
          "semantic key field length is not terminated");
    }

    std::size_t length = 0;
    for (std::size_t i = digits_begin; i < cursor; ++i) {
      const std::size_t digit = static_cast<std::size_t>(encoded[i] - '0');
      if (length > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
        return Status::InvalidArgument("semantic key field length overflows");
      }
      length = length * 10 + digit;
    }
    ++cursor;  // consume ':'

    // A length running past the end must fail. Truncating instead would let a
    // malformed key decode as a shorter valid one.
    if (length > encoded.size() - cursor) {
      return Status::InvalidArgument("semantic key field length exceeds input");
    }
    fields.push_back(KeyField{static_cast<KeyFieldTag>(tag),
                              std::string(encoded.substr(cursor, length))});
    cursor += length;
  }
  return fields;
}

StatusOr<DecodedKey> DecodeKey(std::string_view encoded) {
  if (!encoded.starts_with(kSemanticKeyVersion)) {
    return Status::InvalidArgument("semantic key has no version prefix");
  }
  auto fields = DecodeFields(encoded.substr(kSemanticKeyVersion.size()));
  if (!fields.ok())
    return fields.status();
  if (fields->size() < 2) {
    return Status::InvalidArgument("semantic key has no relation and arity");
  }
  if ((*fields)[0].tag != KeyFieldTag::kSymbol ||
      (*fields)[1].tag != KeyFieldTag::kUnsigned) {
    return Status::InvalidArgument("semantic key header is malformed");
  }

  std::size_t arity = 0;
  const std::string& arity_text = (*fields)[1].value;
  if (arity_text.empty())
    return Status::InvalidArgument("semantic key arity is empty");
  for (const char digit : arity_text) {
    if (digit < '0' || digit > '9')
      return Status::InvalidArgument("semantic key arity is not numeric");
    arity = arity * 10 + static_cast<std::size_t>(digit - '0');
  }

  DecodedKey decoded;
  decoded.relation_name = (*fields)[0].value;
  decoded.cells.assign(fields->begin() + 2, fields->end());
  if (decoded.cells.size() != arity) {
    return Status::InvalidArgument(
        "semantic key arity does not match its cell count");
  }
  return decoded;
}

}  // namespace veritas::facts
