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

#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace veritas::core {

namespace {

// Self-contained SHA-256 implementation (FIPS 180-4).
// Avoids external hash-library dependency for M2. Can be replaced later with
// a vetted crypto library if performance requires it.

constexpr uint32_t kSHA256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t kSHA256H0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

inline uint32_t RotR(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

void ProcessBlock(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + S1 + ch + kSHA256K[i] + w[i];
    uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

SHA256Digest ComputeSHA256(std::span<const std::byte> data) {
  uint32_t state[8];
  std::memcpy(state, kSHA256H0, sizeof(kSHA256H0));

  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
  size_t len = data.size();

  size_t full_blocks = len / 64;
  for (size_t i = 0; i < full_blocks; ++i) {
    ProcessBlock(state, bytes + i * 64);
  }

  uint8_t final_block[128] = {0};
  size_t remaining = len - full_blocks * 64;
  if (remaining > 0) {
    std::memcpy(final_block, bytes + full_blocks * 64, remaining);
  }
  final_block[remaining] = 0x80;

  size_t total_len = remaining < 56 ? 64 : 128;
  uint64_t bit_len = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) {
    final_block[total_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
  }

  ProcessBlock(state, final_block);
  if (total_len == 128) {
    ProcessBlock(state, final_block + 64);
  }

  SHA256Digest result{};
  for (int i = 0; i < 8; ++i) {
    result[i * 4 + 0] = static_cast<std::byte>((state[i] >> 24) & 0xFF);
    result[i * 4 + 1] = static_cast<std::byte>((state[i] >> 16) & 0xFF);
    result[i * 4 + 2] = static_cast<std::byte>((state[i] >> 8) & 0xFF);
    result[i * 4 + 3] = static_cast<std::byte>(state[i] & 0xFF);
  }
  return result;
}

std::string DigestToHex(const SHA256Digest& digest) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (auto byte : digest) {
    oss << std::setw(2) << static_cast<unsigned>(static_cast<uint8_t>(byte));
  }
  return oss.str();
}

std::optional<SHA256Digest> HexToDigest(std::string_view hex) {
  if (hex.size() != 64) {
    return std::nullopt;
  }

  SHA256Digest result{};
  for (size_t i = 0; i < 32; ++i) {
    char high = hex[i * 2];
    char low = hex[i * 2 + 1];

    if (!std::isxdigit(high) || !std::isxdigit(low)) {
      return std::nullopt;
    }

    auto hex_to_nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };

    result[i] =
        static_cast<std::byte>((hex_to_nibble(high) << 4) | hex_to_nibble(low));
  }

  return result;
}

}  // namespace veritas::core
