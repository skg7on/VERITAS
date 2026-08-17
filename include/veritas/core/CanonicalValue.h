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

// CanonicalValue.h — structured value representation for deterministic hashing.
//
// Every VERITAS ID is a hash over canonical bytes. This module provides the
// structured representation (CanonicalValue) and encoding rules that guarantee
// equivalent semantic input produces identical byte sequences:
//
// - Maps are sorted by key.
// - Sets are sorted by canonical child ID.
// - Ordered lists preserve order only when order has semantic meaning.
// - Strings are UTF-8.
// - Default values are encoded consistently.
// - Timestamps and debug text are excluded.
// - Absolute local paths are rejected unless explicitly tagged as external
//   roots.

#ifndef VERITAS_CORE_CANONICALVALUE_H_
#define VERITAS_CORE_CANONICALVALUE_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "veritas/core/Status.h"

namespace veritas::core {

struct CanonicalValue;

enum class PathRootKind : uint8_t {
  kRepository = 0,
  kGenerated = 1,
  kExternal = 2,
  kToolchain = 3,
};

struct TaggedPath {
  PathRootKind root_kind;
  std::string root_id;
  std::string relative_path;
};

using CanonicalNull = std::monostate;
using CanonicalBool = bool;
using CanonicalInt = int64_t;
using CanonicalString = std::string;
using CanonicalArray = std::vector<CanonicalValue>;
using CanonicalObject = std::map<std::string, CanonicalValue>;
using CanonicalPath = TaggedPath;

struct CanonicalValue {
  std::variant<CanonicalNull, CanonicalBool, CanonicalInt, CanonicalString,
               CanonicalArray, CanonicalObject, CanonicalPath>
      value;
};

// Encode a CanonicalValue into a deterministic byte sequence. Keys in objects
// are sorted; arrays preserve order. Tagged paths are encoded with their kind
// and root ID; untagged absolute paths return InvalidArgument.
StatusOr<std::vector<std::byte>> CanonicalEncode(const CanonicalValue& value);

// Helper constructors for test ergonomics.
inline CanonicalValue Null() { return {CanonicalNull{}}; }
inline CanonicalValue Bool(bool v) { return {v}; }
inline CanonicalValue Int(int64_t v) { return {v}; }
inline CanonicalValue String(std::string v) { return {std::move(v)}; }
inline CanonicalValue Array(CanonicalArray v) { return {std::move(v)}; }
inline CanonicalValue Object(CanonicalObject v) { return {std::move(v)}; }
inline CanonicalValue Path(TaggedPath v) { return {std::move(v)}; }

}  // namespace veritas::core

#endif  // VERITAS_CORE_CANONICALVALUE_H_
