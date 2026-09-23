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

#include <array>
#include <gtest/gtest.h>
#include <locale>
#include <string_view>
#include <vector>

using namespace veritas::core;

class CorruptingNumPut : public std::num_put<char> {
protected:
  iter_type do_put(iter_type out, std::ios_base &, char_type,
                   unsigned long) const override {
    *out++ = 'x';
    return out;
  }

  iter_type do_put(iter_type out, std::ios_base &, char_type,
                   unsigned long long) const override {
    *out++ = 'x';
    return out;
  }
};

class GlobalLocaleGuard {
public:
  GlobalLocaleGuard() : previous_(std::locale()) {
    std::locale::global(std::locale(previous_, new CorruptingNumPut));
  }
  ~GlobalLocaleGuard() { std::locale::global(previous_); }

private:
  std::locale previous_;
};

TEST(HashTest, ComputesSHA256) {
  std::vector<std::byte> data = {std::byte{0x61}, std::byte{0x62},
                                 std::byte{0x63}}; // "abc"
  auto digest = ComputeSHA256(data);
  EXPECT_EQ(DigestToHex(digest),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
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

TEST(HashTest, StreamingSHA256MatchesOneShotAcrossBlockBoundaries) {
  std::vector<std::byte> data(137);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::byte>(i);
  }

  SHA256Hasher hasher;
  hasher.Update(std::span(data).subspan(0, 1));
  hasher.Update(std::span(data).subspan(1, 62));
  hasher.Update(std::span(data).subspan(63, 1));
  hasher.Update(std::span(data).subspan(64, 64));
  hasher.Update(std::span(data).subspan(128));

  EXPECT_EQ(hasher.Finalize(), ComputeSHA256(data));
}

TEST(HashTest, StreamingSHA256MatchesKnownAnswersAtPaddingBoundaries) {
  struct KnownAnswer {
    std::size_t input_size;
    std::string_view digest;
  };
  constexpr std::array<KnownAnswer, 4> kKnownAnswers = {{
      {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
      {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
      {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
      {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
  }};

  for (const auto &[input_size, digest] : kKnownAnswers) {
    SCOPED_TRACE(input_size);
    std::vector<std::byte> data(input_size, static_cast<std::byte>('a'));
    SHA256Hasher hasher;
    hasher.Update(std::span(data).first(1));
    hasher.Update(std::span(data).subspan(1, input_size - 2));
    hasher.Update(std::span(data).last(1));
    EXPECT_EQ(DigestToHex(hasher.Finalize()), digest);
  }
}

TEST(HashTest, StreamingSHA256HandlesEmptyUpdates) {
  SHA256Hasher hasher;
  const std::array prefix = {std::byte{0x61}};
  hasher.Update(prefix);
  hasher.Update({});
  EXPECT_EQ(hasher.Finalize(), ComputeSHA256(prefix));
}

TEST(HashTest, DigestToHexProduces64Characters) {
  std::vector<std::byte> data = {std::byte{0x42}};
  auto digest = ComputeSHA256(data);
  auto hex = DigestToHex(digest);
  EXPECT_EQ(hex.size(), 64u);
}

TEST(HashTest, DigestToHexIsIndependentOfTheGlobalNumericLocale) {
  SHA256Digest digest{};
  digest[0] = std::byte{0x01};
  digest[1] = std::byte{0xaf};
  digest[31] = std::byte{0xf0};
  GlobalLocaleGuard locale;
  EXPECT_EQ(DigestToHex(digest),
            "01af0000000000000000000000000000000000000000000000000000000000f0");
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
  std::string high_bit(64, '0');
  high_bit[0] = static_cast<char>(0x80);
  EXPECT_FALSE(HexToDigest(high_bit).has_value());
}
