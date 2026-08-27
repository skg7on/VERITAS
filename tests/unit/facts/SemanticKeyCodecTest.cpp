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

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::facts {
namespace {

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& part : parts) {
    out += part;
  }
  return out;
}

std::string SymbolFields(const std::vector<std::string>& values) {
  std::vector<std::string> parts;
  for (const auto& value : values) {
    parts.push_back(EncodeSymbolField(value));
  }
  return Join(parts);
}

// Without a length prefix, {"a","bc"} and {"ab","c"} concatenate identically.
// The whole point of the codec is that a cell cannot move a field boundary.
TEST(SemanticKeyCodecTest, AdjacentFieldsCannotBeReassociated) {
  EXPECT_NE(SymbolFields({"a", "bc"}), SymbolFields({"ab", "c"}));
}

// A cell may legitimately contain the codec's own delimiters. Decoding is
// driven by the length, never by scanning for a separator, so this is inert.
TEST(SemanticKeyCodecTest, DelimiterLikeFieldsRemainDistinct) {
  EXPECT_NE(SymbolFields({"", ":1:|"}), SymbolFields({":1:", "|"}));
}

TEST(SemanticKeyCodecTest, DigitPrefixedFieldsRemainDistinct) {
  EXPECT_NE(SymbolFields({"", "01"}), SymbolFields({"0", "1"}));
}

TEST(SemanticKeyCodecTest, RoundTripsUnicodeEmptyAndDigitPrefixedFields) {
  const std::string encoded = SymbolFields({"\xCE\xBB", "01", ""});
  auto decoded = DecodeFields(encoded);
  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(decoded->size(), 3u);
  EXPECT_EQ((*decoded)[0].value, "\xCE\xBB");
  EXPECT_EQ((*decoded)[1].value, "01");
  EXPECT_EQ((*decoded)[2].value, "");
  for (const auto& field : *decoded) {
    EXPECT_EQ(field.tag, KeyFieldTag::kSymbol);
  }
}

// The type tag keeps domains apart: the text "1" and the number 1 are
// different semantic content and must not encode alike.
TEST(SemanticKeyCodecTest, TypeTagSeparatesDomains) {
  EXPECT_NE(EncodeSymbolField("1"), EncodeNumberField(1));
  EXPECT_NE(EncodeNumberField(1), EncodeUnsignedField(1));
  EXPECT_NE(EncodeUnsignedField(1), EncodeEnumField(1));
  EXPECT_NE(EncodeSymbolField("memref:sha256:x"),
            EncodeIdField("memref:sha256:x"));
}

// Signed bounds must survive, including the asymmetric minimum.
TEST(SemanticKeyCodecTest, RoundTripsSignedBounds) {
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  auto decoded = DecodeFields(EncodeNumberField(min) + EncodeNumberField(max) +
                              EncodeNumberField(0));
  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(decoded->size(), 3u);
  EXPECT_EQ((*decoded)[0].value, std::to_string(min));
  EXPECT_EQ((*decoded)[1].value, std::to_string(max));
  EXPECT_EQ((*decoded)[2].value, "0");
}

TEST(SemanticKeyCodecTest, RoundTripsUnsignedBounds) {
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  auto decoded = DecodeFields(EncodeUnsignedField(max) + EncodeUnsignedField(0));
  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(decoded->size(), 2u);
  EXPECT_EQ((*decoded)[0].value, std::to_string(max));
  EXPECT_EQ((*decoded)[1].value, "0");
}

// A truncated or over-long length must be rejected rather than silently
// producing a short field, which would let a malformed key decode as a valid
// one.
TEST(SemanticKeyCodecTest, RejectsMalformedEncodings) {
  EXPECT_FALSE(DecodeFields("S5:ab").ok());
  EXPECT_FALSE(DecodeFields("S2ab").ok());
  EXPECT_FALSE(DecodeFields("X1:a").ok());
  EXPECT_FALSE(DecodeFields("S:a").ok());
  EXPECT_FALSE(DecodeFields("S-1:a").ok());
}

// The header pins the relation and arity, so two relations that share a column
// shape cannot produce the same key.
TEST(SemanticKeyCodecTest, HeaderSeparatesRelationsOfEqualArity) {
  EXPECT_NE(EncodeKeyHeader("ReachableCall", 3),
            EncodeKeyHeader("SupportReachableCall", 3));
  EXPECT_NE(EncodeKeyHeader("ReachableCall", 3),
            EncodeKeyHeader("ReachableCall", 4));
}

// A witness read back from an engine arrives as a key, so a key must decode to
// exactly the relation and cells it was built from.
TEST(SemanticKeyCodecTest, RoundTripsAWholeKey) {
  const std::string key = EncodeKeyHeader("ReachableCall", 3) +
                          EncodeIdField("func:sha256:aa") +
                          EncodeIdField("func:sha256:bb") + EncodeEnumField(1);
  auto decoded = DecodeKey(key);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->relation_name, "ReachableCall");
  ASSERT_EQ(decoded->cells.size(), 3u);
  EXPECT_EQ(decoded->cells[0].tag, KeyFieldTag::kId);
  EXPECT_EQ(decoded->cells[0].value, "func:sha256:aa");
  EXPECT_EQ(decoded->cells[2].tag, KeyFieldTag::kEnum);
  EXPECT_EQ(decoded->cells[2].value, "1");
}

// A key whose declared arity disagrees with its cell count is malformed. If it
// decoded anyway, a truncated key could impersonate a shorter valid fact.
TEST(SemanticKeyCodecTest, RejectsArityMismatch) {
  const std::string key = EncodeKeyHeader("ReachableCall", 3) +
                          EncodeIdField("func:sha256:aa");
  EXPECT_FALSE(DecodeKey(key).ok());
}

TEST(SemanticKeyCodecTest, RejectsKeyWithoutVersionPrefix) {
  EXPECT_FALSE(DecodeKey(EncodeSymbolField("ReachableCall") +
                         EncodeUnsignedField(0))
                   .ok());
}

}  // namespace
}  // namespace veritas::facts
