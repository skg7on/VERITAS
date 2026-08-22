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

// Ids.h — stable, typed, domain-separated identifiers for VERITAS entities.
//
// Every VERITAS ID is semantic, content-derived, and includes its kind and
// hash algorithm in the serialized form. Format:
//
//   <kind>:<algorithm>:<hex_digest>
//
// Examples:
//   repo:sha256:a3f8b9...
//   funcbody:sha256:d7e2c1...
//
// IDs are immutable and comparable. Invalid ID strings return InvalidArgument.

#ifndef VERITAS_CORE_IDS_H_
#define VERITAS_CORE_IDS_H_

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "veritas/core/Status.h"

namespace veritas::core {

enum class IdKind {
  kRepository,
  kRevision,
  kBuildVariant,
  kTranslationUnit,
  kFunctionSymbol,
  kFunctionVariant,
  kFunctionBody,
  kFunctionSummary,
  kFact,
  kScc,
  // CPG (M6) identities
  kValueRef,
  kMemoryRef,
  kCallSite,
  kBasicBlockSummary,
  kCpgProjection,
  kCpgEdge,
  kUnknownNode,
};

struct StableId {
  IdKind kind;
  std::string digest_hex;

  auto operator<=>(const StableId &) const = default;
};

// Construct a StableId from kind and canonical bytes. Uses SHA-256.
StableId MakeStableId(IdKind kind, std::span<const std::byte> canonical_bytes);

// Serialize a StableId to its string form: <kind>:<algorithm>:<digest>.
std::string ToString(const StableId &id);

// Parse a StableId from its string form. Returns InvalidArgument if the
// format is invalid or the kind/algorithm is unrecognized.
StatusOr<StableId> ParseStableId(std::string_view text);

// Type aliases for specific ID kinds
using FunctionSymbolId = std::string;

} // namespace veritas::core

#endif // VERITAS_CORE_IDS_H_
