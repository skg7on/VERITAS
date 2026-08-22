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

// SemanticTypes.h — typed semantic values shared across VERITAS analysis.
//
// These types separate semantic *value* (what is true) from epistemic *state*
// (how confidently we know it). They are the stable, typed contract consumed
// by the V2 relation registry, the summary builder, SVF normalization, and WPA
// materialization. They intentionally carry no LLVM or SVF dependency.

#ifndef VERITAS_ANALYSIS_SEMANTIC_SEMANTIC_TYPES_H_
#define VERITAS_ANALYSIS_SEMANTIC_SEMANTIC_TYPES_H_

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::analysis::semantic {

enum class EpistemicState : std::uint8_t {
  kMust,
  kMay,
  kMustNot,
  kInferred,
  kAssumed,
  kUnknown,
};

enum class AliasKind : std::uint8_t {
  kMustAlias,
  kMayAlias,
  kNoAlias,
  kUnknownAlias,
};

enum class DispatchKind : std::uint8_t {
  kDirect,
  kIndirect,
  kVirtual,
  kCallback,
  kExternal,
  kUnknown,
};

enum class AbstractObjectKind : std::uint8_t {
  kGlobal,
  kStack,
  kHeap,
  kArgument,
  kFunction,
  kExternal,
  kUnknown,
  kLegacyOpaque,
};

enum class ByteRangeKind : std::uint8_t { kKnown, kUnknown };

struct AliasObservation {
  AliasKind kind;
  EpistemicState epistemic;
  auto operator<=>(const AliasObservation&) const = default;
};

struct AccessPathSegment {
  enum class Kind : std::uint8_t { kField, kArrayIndex, kArrayRange, kUnknown };
  Kind kind;
  std::int64_t first = 0;
  std::int64_t last = 0;
  auto operator<=>(const AccessPathSegment&) const = default;
};

struct ByteRange {
  std::optional<std::int64_t> offset;
  std::optional<std::uint64_t> size;
  static ByteRange Unknown() { return {}; }
  static ByteRange Known(std::int64_t offset, std::uint64_t size) {
    return {.offset = offset, .size = size};
  }
  auto operator<=>(const ByteRange&) const = default;
};

struct AbstractObject {
  core::StableId id;
  AbstractObjectKind kind;
  std::optional<core::StableId> owner_function;
  std::string semantic_anchor;
  std::string diagnostic_name;
};

struct MemoryLocation {
  core::StableId id;
  AbstractObject object;
  std::vector<AccessPathSegment> access_path;
  ByteRange byte_range;
};

// Maps a byte range to its relation-projection kind. A range is known only
// when both offset and size are present; anything else projects as unknown.
ByteRangeKind RelationRangeKind(const ByteRange& range);

// Validates a byte range: half-known ranges (exactly one of offset/size set)
// are rejected.
Status Validate(const ByteRange& range);

// Validates an abstract object: its ID must be an abstract object, any owner
// must be a function variant, and the semantic anchor must be control-free.
Status Validate(const AbstractObject& object);

// Validates a memory location: its ID must be a memory reference, and the
// embedded object and byte range must themselves be valid.
Status Validate(const MemoryLocation& location);

}  // namespace veritas::analysis::semantic

#endif  // VERITAS_ANALYSIS_SEMANTIC_SEMANTIC_TYPES_H_
