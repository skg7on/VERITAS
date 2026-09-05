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

// HexCodec.h — lowercase hex encoding for the fact store's TEXT-only SQLite
// interface. Serialized protobuf blobs are hex-encoded so they can be bound
// and read through MetadataStore's string-only Execute/Query.

#ifndef VERITAS_FACTS_HEX_CODEC_H_
#define VERITAS_FACTS_HEX_CODEC_H_

#include <string>
#include <string_view>

#include "veritas/core/Status.h"

namespace veritas::facts {

inline std::string HexEncode(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char c : bytes) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 0x0F]);
  }
  return out;
}

inline StatusOr<std::string> HexDecode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Status::InvalidArgument("odd-length hex string");
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return Status::InvalidArgument("invalid hex digit");
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_HEX_CODEC_H_
