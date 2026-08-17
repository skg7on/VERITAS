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

#include "veritas/core/CanonicalValue.h"

#include <cstring>

namespace veritas::core {

namespace {

enum class TypeTag : uint8_t {
  kNull = 0,
  kBool = 1,
  kInt = 2,
  kString = 3,
  kArray = 4,
  kObject = 5,
  kPath = 6,
};

void AppendU8(std::vector<std::byte>& out, uint8_t val) {
  out.push_back(static_cast<std::byte>(val));
}

void AppendU64(std::vector<std::byte>& out, uint64_t val) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<std::byte>((val >> (i * 8)) & 0xFF));
  }
}

void AppendI64(std::vector<std::byte>& out, int64_t val) {
  uint64_t uval;
  std::memcpy(&uval, &val, sizeof(val));
  AppendU64(out, uval);
}

void AppendString(std::vector<std::byte>& out, const std::string& str) {
  AppendU64(out, str.size());
  for (char c : str) {
    out.push_back(static_cast<std::byte>(static_cast<uint8_t>(c)));
  }
}

StatusOr<std::vector<std::byte>> EncodeValue(const CanonicalValue& value);

StatusOr<std::vector<std::byte>> EncodeNull() {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kNull));
  return result;
}

StatusOr<std::vector<std::byte>> EncodeBool(bool val) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kBool));
  AppendU8(result, val ? 1 : 0);
  return result;
}

StatusOr<std::vector<std::byte>> EncodeInt(int64_t val) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kInt));
  AppendI64(result, val);
  return result;
}

StatusOr<std::vector<std::byte>> EncodeString(const std::string& val) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kString));
  AppendString(result, val);
  return result;
}

StatusOr<std::vector<std::byte>> EncodeArray(const CanonicalArray& arr) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kArray));
  AppendU64(result, arr.size());
  for (const auto& item : arr) {
    auto encoded = EncodeValue(item);
    if (!encoded.ok()) {
      return encoded.status();
    }
    result.insert(result.end(), encoded.value().begin(), encoded.value().end());
  }
  return result;
}

StatusOr<std::vector<std::byte>> EncodeObject(const CanonicalObject& obj) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kObject));
  AppendU64(result, obj.size());
  for (const auto& [key, val] : obj) {
    AppendString(result, key);
    auto encoded = EncodeValue(val);
    if (!encoded.ok()) {
      return encoded.status();
    }
    result.insert(result.end(), encoded.value().begin(), encoded.value().end());
  }
  return result;
}

StatusOr<std::vector<std::byte>> EncodePath(const TaggedPath& path) {
  std::vector<std::byte> result;
  AppendU8(result, static_cast<uint8_t>(TypeTag::kPath));
  AppendU8(result, static_cast<uint8_t>(path.root_kind));
  AppendString(result, path.root_id);
  AppendString(result, path.relative_path);
  return result;
}

StatusOr<std::vector<std::byte>> EncodeValue(const CanonicalValue& value) {
  return std::visit(
      [](const auto& v) -> StatusOr<std::vector<std::byte>> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, CanonicalNull>) {
          return EncodeNull();
        } else if constexpr (std::is_same_v<T, CanonicalBool>) {
          return EncodeBool(v);
        } else if constexpr (std::is_same_v<T, CanonicalInt>) {
          return EncodeInt(v);
        } else if constexpr (std::is_same_v<T, CanonicalString>) {
          return EncodeString(v);
        } else if constexpr (std::is_same_v<T, CanonicalArray>) {
          return EncodeArray(v);
        } else if constexpr (std::is_same_v<T, CanonicalObject>) {
          return EncodeObject(v);
        } else if constexpr (std::is_same_v<T, CanonicalPath>) {
          return EncodePath(v);
        }
      },
      value.value);
}

}  // namespace

StatusOr<std::vector<std::byte>> CanonicalEncode(const CanonicalValue& value) {
  return EncodeValue(value);
}

}  // namespace veritas::core
