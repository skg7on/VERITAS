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

#include "veritas/review/ReviewTypes.h"
#include <gtest/gtest.h>
#include <type_traits>

namespace veritas::review {
TEST(ReviewTypesTest, LocalBindingHasNoNativeRun) {
  ReviewBinding binding;
  EXPECT_FALSE(binding.native_binding.has_value());
}
TEST(ReviewTypesTest, ArtifactKindsCannotBeSubstituted) {
  static_assert(!std::is_convertible_v<ContextId, SnapshotId>);
  EXPECT_FALSE(
      ParseArtifactId<SnapshotTag>("context:sha256:" + std::string(64, 'a'))
          .ok());
}
TEST(ReviewTypesTest, RejectsDuplicateAndUnknownKeys) {
  EXPECT_FALSE(
      DecodeV2(
          R"({"protocol":"veritas.review.backend.v2","request_id":"x","operation":"handshake","parameters":{"accepted_protocols":[],"authority":"FORMAL"}})")
          .ok());
  EXPECT_FALSE(
      DecodeV2(
          R"({"protocol":"veritas.review.backend.v2","request_id":"x","request_id":"y","operation":"handshake","parameters":{"accepted_protocols":[]}})")
          .ok());
}
TEST(ReviewTypesTest, HandshakeHasNoBinding) {
  auto value = DecodeV2(
      R"({"protocol":"veritas.review.backend.v2","request_id":"x","operation":"handshake","parameters":{"accepted_protocols":["veritas.review.backend.v2"]}})");
  ASSERT_TRUE(value.ok()) << value.status().message();
  EXPECT_FALSE(value->binding.has_value());
  EXPECT_FALSE(
      DecodeV2(
          R"({"protocol":"veritas.review.backend.v1","request_id":"x","operation":"handshake","parameters":{"accepted_protocols":[]}})")
          .ok());
}
TEST(ReviewTypesTest, CanonicalIdsIgnoreAuditTiming) {
  auto a = MakeArtifactId<ContextTag>(
      Json::Object{{"text", "hé\n"}, {"n", uint64_t{18446744073709551615ULL}}});
  ASSERT_TRUE(a.ok());
  EXPECT_EQ(a->value(),
            "context:sha256:"
            "1b7b242e87e11268112f4919a50c07e5b7dc8e3ce15b4700646fa7d7d2a33bdf");
  auto json =
      CanonicalJson(Json::Object{{"z", nullptr},
                                 {"a", "\x01/é"},
                                 {"n", int64_t{-9223372036854775807LL - 1}}});
  ASSERT_TRUE(json.ok());
  EXPECT_EQ(*json,
            "{\"a\":\"\\u0001/é\",\"n\":-9223372036854775808,\"z\":null}");
}
TEST(ReviewTypesTest, RejectsNonIntegerAndInvalidUnicode) {
  for (auto text :
       {"{\"n\":1.0}", "{\"n\":1e0}", "{\"n\":18446744073709551616}",
        "{\"n\":-9223372036854775809}", "{\"n\":NaN}", "{\"x\":\"\\ud800\"}"})
    EXPECT_FALSE(ParseStrictJson(text).ok()) << text;
  EXPECT_FALSE(
      ParseStrictJson(std::string("{\"x\":\"") + char(0xff) + "\"}").ok());
  EXPECT_FALSE(ParseStrictJson("{} {}").ok());
}

TEST(ReviewTypesTest, CaptureUsesTrustedSelectionKeysBeforeRefs) {
  const std::string valid =
      R"({"protocol":"veritas.review.backend.v2","request_id":"r","operation":"capture","parameters":{"project_key":"p","base_ref":"HEAD","target_ref":null,"selected_untracked":["new.cc"],"program_manifest_key":"program","context_selection_keys":[]}})";
  EXPECT_TRUE(DecodeV2(valid).ok());
  for (const auto &key :
       {"/tmp/program.json",
        "context:sha256:"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
    auto bad = valid;
    bad.replace(bad.find("\"program\""), 9, "\"" + std::string(key) + "\"");
    EXPECT_FALSE(DecodeV2(bad).ok());
  }
}

TEST(ReviewTypesTest, CanonicalUnicodeNulAndEmptyArrays) {
  auto bytes = CanonicalJson(Json::Object{
      {"unicode", "é😀"}, {"nul", std::string(1, '\0')}, {"a", Json::Array{}}});
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(*bytes, "{\"a\":[],\"nul\":\"\\u0000\",\"unicode\":\"é😀\"}");
  EXPECT_FALSE(
      CanonicalJson(Json::Object{{std::string(1, char(0xff)), "value"}}).ok());
}
} // namespace veritas::review
