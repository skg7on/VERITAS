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

// NormalizedAnalysisFacts.h — the normalized SVF fact boundary.
//
// These structs are the typed, stable-ID replacement for the temporary
// string-based fact structs that used to live in SvfFactMapper.h. Every fact
// carries VERITAS stable identities (core::StableId) and semantic enums
// instead of raw LLVM names or SVF node IDs. SVF-native nodes never cross this
// boundary. This is the contract consumed by the V2 merge/pipeline (Task 9).

#ifndef VERITAS_ANALYSIS_SEMANTIC_NORMALIZED_ANALYSIS_FACTS_H_
#define VERITAS_ANALYSIS_SEMANTIC_NORMALIZED_ANALYSIS_FACTS_H_

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::semantic {

// The kind of a memory effect. Andersen is a may-analysis, so SVF-mapped
// effects are kMayRead / kMayWrite; the epistemic state carries the MUST/MAY
// distinction independently.
enum class MemoryEffectKind : std::uint8_t {
  kRead,
  kWrite,
  kMayRead,
  kMayWrite,
  kUnknown,
};

// The kind of a dependency edge between two values.
enum class DependencyKind : std::uint8_t {
  kControl,
  kData,
  kMemory,
  kUnknown,
};

// A value flow from a source value to a destination value.
struct NormalizedValueFlow {
  core::StableId source_value_id;       // must be kValueRef
  core::StableId destination_value_id;  // must be kValueRef
  EpistemicState epistemic;
  std::string provenance_ref;

  auto operator<=>(const NormalizedValueFlow&) const = default;
};

// A resolved (or unknown) call target at a call site.
struct NormalizedCallTarget {
  core::StableId call_site;               // must be kCallSite
  core::StableId caller;                  // caller function (kValueRef)
  std::optional<core::StableId> callee;   // resolved callee (kValueRef)
  DispatchKind dispatch;
  EpistemicState epistemic;
  std::string diagnostic_symbol;  // human-readable name, never identity
  std::string provenance_ref;

  auto operator<=>(const NormalizedCallTarget&) const = default;
};

// An alias relationship between two memory locations.
struct NormalizedAlias {
  MemoryLocation left;
  MemoryLocation right;
  AliasKind kind;
  EpistemicState epistemic;
  std::string provenance_ref;

  // MemoryLocation has no ordering of its own, so order and equality compare
  // the content-addressed location identity (left.id / right.id) plus the
  // semantic fields. Two locations with equal IDs are the same location.
  auto operator<=>(const NormalizedAlias& other) const {
    if (auto c = left.id <=> other.left.id; c != 0) return c;
    if (auto c = right.id <=> other.right.id; c != 0) return c;
    if (auto c = kind <=> other.kind; c != 0) return c;
    if (auto c = epistemic <=> other.epistemic; c != 0) return c;
    return provenance_ref <=> other.provenance_ref;
  }
  bool operator==(const NormalizedAlias& other) const {
    return left.id == other.left.id && right.id == other.right.id &&
           kind == other.kind && epistemic == other.epistemic &&
           provenance_ref == other.provenance_ref;
  }
};

// A memory read or write effect on a structured memory location.
struct NormalizedMemoryEffect {
  core::StableId operation;  // the load/store value ref (kValueRef)
  MemoryLocation location;
  MemoryEffectKind kind;
  EpistemicState epistemic;
  std::string provenance_ref;

  auto operator<=>(const NormalizedMemoryEffect& other) const {
    if (auto c = operation <=> other.operation; c != 0) return c;
    if (auto c = location.id <=> other.location.id; c != 0) return c;
    if (auto c = kind <=> other.kind; c != 0) return c;
    if (auto c = epistemic <=> other.epistemic; c != 0) return c;
    return provenance_ref <=> other.provenance_ref;
  }
  bool operator==(const NormalizedMemoryEffect& other) const {
    return operation == other.operation && location.id == other.location.id &&
           kind == other.kind && epistemic == other.epistemic &&
           provenance_ref == other.provenance_ref;
  }
};

// A scoped unknown: something SVF could not map or a budget truncation.
struct NormalizedUnknown {
  std::string scope;
  std::string reason;
  EpistemicState epistemic;
  std::string provenance_ref;

  auto operator<=>(const NormalizedUnknown&) const = default;
};

// A dependency edge between two values.
struct NormalizedDependency {
  core::StableId from;  // must be kValueRef
  core::StableId to;    // must be kValueRef
  DependencyKind kind;

  auto operator<=>(const NormalizedDependency&) const = default;
};

// The complete set of normalized SVF facts for one analysis run.
struct NormalizedAnalysisFacts {
  std::vector<NormalizedValueFlow> value_flows;
  std::vector<NormalizedAlias> aliases;
  std::vector<NormalizedMemoryEffect> memory_effects;
  std::vector<NormalizedCallTarget> calls;
  std::vector<NormalizedUnknown> unknowns;
  std::vector<NormalizedDependency> dependencies;
};

}  // namespace veritas::analysis::semantic

#endif  // VERITAS_ANALYSIS_SEMANTIC_NORMALIZED_ANALYSIS_FACTS_H_
