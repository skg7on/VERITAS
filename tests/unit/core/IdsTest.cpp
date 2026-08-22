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

#include "veritas/core/Ids.h"

#include <array>

#include <gtest/gtest.h>

using namespace veritas::core;

TEST(IdsTest, RoundTripsStableIdText) {
  std::vector<std::byte> data = {std::byte{0x01}, std::byte{0x02},
                                 std::byte{0x03}};
  auto id = MakeStableId(IdKind::kRepository, data);
  auto text = ToString(id);
  auto parsed = ParseStableId(text);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value(), id);
}

TEST(IdsTest, DifferentKindsSameBytesProduceDifferentStrings) {
  std::vector<std::byte> data = {std::byte{0xaa}, std::byte{0xbb}};
  auto repo_id = MakeStableId(IdKind::kRepository, data);
  auto rev_id = MakeStableId(IdKind::kRevision, data);
  EXPECT_NE(ToString(repo_id), ToString(rev_id));
}

TEST(IdsTest, SameKindSameBytesProduceSameId) {
  std::vector<std::byte> data = {std::byte{0xff}};
  auto id1 = MakeStableId(IdKind::kFunctionBody, data);
  auto id2 = MakeStableId(IdKind::kFunctionBody, data);
  EXPECT_EQ(id1, id2);
  EXPECT_EQ(ToString(id1), ToString(id2));
}

TEST(IdsTest, ParseRejectsInvalidFormat) {
  EXPECT_FALSE(ParseStableId("").ok());
  EXPECT_FALSE(ParseStableId("malformed").ok());
  EXPECT_FALSE(ParseStableId("no:colons").ok());
  EXPECT_FALSE(ParseStableId("repo:md5:abc123").ok()); // wrong algorithm
  EXPECT_FALSE(ParseStableId("unknownkind:sha256:abc123").ok());
}

TEST(IdsTest, ParseRejectsNonHexDigest) {
  EXPECT_FALSE(ParseStableId("fact:sha256:" + std::string(64, 'g')).ok());
}

TEST(IdsTest, ParseRejectsUppercaseDigestAlias) {
  EXPECT_FALSE(ParseStableId("fact:sha256:" + std::string(64, 'A')).ok());
}

TEST(IdsTest, ToStringIncludesKindAlgorithmAndDigest) {
  std::vector<std::byte> data = {std::byte{0x42}};
  auto id = MakeStableId(IdKind::kFunctionSummary, data);
  auto text = ToString(id);
  EXPECT_TRUE(text.find("summary:") != std::string::npos);
  EXPECT_TRUE(text.find("sha256:") != std::string::npos);
  EXPECT_GT(text.size(), 20u); // kind + algo + hex digest
}

TEST(IdsTest, EmptyBytesProduceValidId) {
  std::vector<std::byte> empty;
  auto id = MakeStableId(IdKind::kRepository, empty);
  auto text = ToString(id);
  EXPECT_FALSE(text.empty());
  EXPECT_TRUE(ParseStableId(text).ok());
}

TEST(IdsTest, SccIdRoundTripsWithDedicatedPrefix) {
  const std::vector<std::byte> data = {std::byte{0x53}, std::byte{0x43},
                                       std::byte{0x43}};
  const auto id = MakeStableId(IdKind::kScc, data);
  const std::string text = ToString(id);

  EXPECT_EQ(text.rfind("scc:sha256:", 0), 0u);
  auto parsed = ParseStableId(text);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(*parsed, id);
}

TEST(IdsTest, AnalysisRunAndAbstractObjectIdsRoundTrip) {
  const std::array bytes{std::byte{0x01}};
  for (auto kind : {IdKind::kAnalysisRun, IdKind::kAbstractObject,
                    IdKind::kModel}) {
    const auto id = MakeStableId(kind, bytes);
    ASSERT_TRUE(ParseStableId(ToString(id)).ok());
    EXPECT_EQ(*ParseStableId(ToString(id)), id);
  }
}
