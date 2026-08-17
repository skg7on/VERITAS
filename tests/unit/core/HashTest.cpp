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

#include "veritas/core/Hash.h"

#include <gtest/gtest.h>

using namespace veritas::core;

TEST(HashTest, ComputesSHA256) {
  std::vector<std::byte> data = {std::byte{0x61}, std::byte{0x62},
                                  std::byte{0x63}};  // "abc"
  auto digest = ComputeSHA256(data);
  EXPECT_EQ(digest.size(), kSHA256DigestBytes);
}

TEST(HashTest, SameInputProducesSameDigest) {
  std::vector<std::byte> data = {std::byte{0xff}};
  auto digest1 = ComputeSHA256(data);
  auto digest2 = ComputeSHA256(data);
  EXPECT_EQ(digest1, digest2);
}

TEST(HashTest, DifferentInputsProduceDifferentDigests) {
  std::vector<std::byte> data1 = {std::byte{0x00}};
  std::vector<std::byte> data2 = {std::byte{0x01}};
  auto digest1 = ComputeSHA256(data1);
  auto digest2 = ComputeSHA256(data2);
  EXPECT_NE(digest1, digest2);
}

TEST(HashTest, DigestToHexProduces64Characters) {
  std::vector<std::byte> data = {std::byte{0x42}};
  auto digest = ComputeSHA256(data);
  auto hex = DigestToHex(digest);
  EXPECT_EQ(hex.size(), 64u);
}

TEST(HashTest, HexRoundTrip) {
  std::vector<std::byte> data = {std::byte{0xaa}, std::byte{0xbb}};
  auto digest = ComputeSHA256(data);
  auto hex = DigestToHex(digest);
  auto parsed = HexToDigest(hex);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value(), digest);
}

TEST(HashTest, HexToDigestRejectsInvalidLength) {
  EXPECT_FALSE(HexToDigest("").has_value());
  EXPECT_FALSE(HexToDigest("short").has_value());
  EXPECT_FALSE(HexToDigest(std::string(63, '0')).has_value());
  EXPECT_FALSE(HexToDigest(std::string(65, '0')).has_value());
}

TEST(HashTest, HexToDigestRejectsNonHex) {
  EXPECT_FALSE(HexToDigest(std::string(64, 'g')).has_value());
  EXPECT_FALSE(HexToDigest(std::string(64, ' ')).has_value());
}
