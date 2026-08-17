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
#include <optional>
#include <span>
#include <string>

namespace veritas::core {

constexpr size_t kSHA256DigestBytes = 32;

using SHA256Digest = std::array<std::byte, kSHA256DigestBytes>;

// Compute SHA-256 of the input bytes.
SHA256Digest ComputeSHA256(std::span<const std::byte> data);

// Convert a digest to lowercase hex string (64 characters).
std::string DigestToHex(const SHA256Digest& digest);

// Parse a hex string (64 characters) into a digest. Returns nullopt if the
// input is not valid hex or has the wrong length.
std::optional<SHA256Digest> HexToDigest(std::string_view hex);

}  // namespace veritas::core

#endif  // VERITAS_CORE_HASH_H_
