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

// RelationSchema.h — the typed, authoritative relations.v2 schema registry.
//
// Replaces the historical `std::vector<std::string>` relation authority with a
// typed registry: every relation declares its column names and domains, so
// semantic and execution rows can be validated against a real schema.

#ifndef VERITAS_FACTS_RELATION_SCHEMA_H_
#define VERITAS_FACTS_RELATION_SCHEMA_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace veritas::facts {

// Run-local typed dense ID. Dense IDs are meaningful only within one
// AnalysisRun and never escape it.
template <typename Tag>
struct DenseId {
  std::uint32_t value;
  auto operator<=>(const DenseId&) const = default;
};

using FunctionId = DenseId<struct FunctionTag>;
using ValueId = DenseId<struct ValueTag>;
using MemoryId = DenseId<struct MemoryTag>;
using CallSiteId = DenseId<struct CallSiteTag>;
using FactId = DenseId<struct FactTag>;

// Relation ordinals index the registry table and are not durable: fact
// identity is derived from the relation *name*, so new relations are appended
// here and existing ordinals are never reused for a different relation.
enum class RelationId : std::uint16_t {
  kFunctionMap,
  kValueMap,
  kMemoryMap,
  kCallSiteMap,
  kFactMap,
  kDirectCall,
  kUnknownCall,
  kDirectRead,
  kDirectWrite,
  kAlias,
  kLocalFlow,
  kParameterFlow,
  kReturnFlow,
  kModeledEffect,
  kUnsupportedFeature,
  kReachableCall,
  kMayWrite,
  // Successor-SCC results enter a component as EDB support relations that
  // mirror the column shape of the IDB relation whose results they carry.
  kSupportReachableCall,
  kSupportMayWrite,
};

// Number of relations in the relations.v2 registry. Single authority for both
// the registry table and relation-id range validation.
inline constexpr std::size_t kRelationCountV2 = 19;

// Semantic domain of a relation column. ID domains are carried as a stable ID
// in a semantic row and as a typed dense ID in an execution row (model IDs are
// carried as canonical text in the execution projection, since models are not
// dense-mapped).
enum class ColumnDomain : std::uint16_t {
  kFunctionId,
  kValueId,
  kMemoryId,
  kCallSiteId,
  kFactId,
  kModelId,
  kInt64,
  kUint64,
  kString,
  kDispatchKind,
  kAliasKind,
  kByteRangeKind,
  kEpistemic,
};

enum class RelationOwnership : std::uint8_t {
  kEdb,
  kIdb,
};

struct ColumnSpec {
  std::string name;
  ColumnDomain domain;

  auto operator<=>(const ColumnSpec&) const = default;
};

struct RelationSchema {
  std::string name;
  RelationOwnership ownership;
  std::vector<ColumnSpec> columns;
};

class RelationRegistry {
 public:
  const RelationSchema& Get(RelationId id) const;
};

// Returns the immutable relations.v2 registry.
const RelationRegistry& RelationsV2();

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_RELATION_SCHEMA_H_
