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

// Hash.h — SHA-256 hashing interface for VERITAS.
//
// Provides a simple wrapper around the SHA-256 implementation chosen at build
// time. All VERITAS IDs use SHA-256; no other hash algorithm is supported in
// V1.

#ifndef VERITAS_CORE_HASH_H_
#define VERITAS_CORE_HASH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace veritas::core {

constexpr size_t kSHA256DigestBytes = 32;

using SHA256Digest = std::array<std::byte, kSHA256DigestBytes>;

// Incremental SHA-256 state for hashing canonical encodings without first
// materializing them as one contiguous allocation.
class SHA256Hasher {
public:
  SHA256Hasher();

  void Update(std::span<const std::byte> data);
  SHA256Digest Finalize() const;

private:
  std::array<std::uint32_t, 8> state_{};
  std::array<std::byte, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

// Compute SHA-256 of the input bytes.
SHA256Digest ComputeSHA256(std::span<const std::byte> data);

// Convert a digest to lowercase hex string (64 characters).
std::string DigestToHex(const SHA256Digest &digest);

// Parse a hex string (64 characters) into a digest. Returns nullopt if the
// input is not valid hex or has the wrong length.
std::optional<SHA256Digest> HexToDigest(std::string_view hex);

} // namespace veritas::core

#endif // VERITAS_CORE_HASH_H_
