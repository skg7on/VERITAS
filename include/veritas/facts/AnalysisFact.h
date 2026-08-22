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

// AnalysisFact.h — typed V2 fact rows and their witness-independent identity.
//
// A semantic row carries durable stable IDs and typed semantic enums; an
// execution row carries run-local dense IDs for hot joins. Both are validated
// against the relations.v2 schema. MakeFact derives a content-addressed fact
// ID from the semantic row alone, independent of engine, tuple order, or
// witness edges.

#ifndef VERITAS_FACTS_ANALYSIS_FACT_H_
#define VERITAS_FACTS_ANALYSIS_FACT_H_

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::facts {

// Shorthand for the semantic value namespace (nested under veritas::analysis).
namespace semantic = analysis::semantic;

using SemanticCellValue =
    std::variant<core::StableId, std::int64_t, std::uint64_t, std::string,
                 semantic::DispatchKind, semantic::AliasKind,
                 semantic::ByteRangeKind, semantic::EpistemicState>;

using ExecutionCellValue =
    std::variant<FunctionId, ValueId, MemoryId, CallSiteId, FactId,
                 std::int64_t, std::uint64_t, std::string,
                 semantic::DispatchKind, semantic::AliasKind,
                 semantic::ByteRangeKind, semantic::EpistemicState>;

struct SemanticRow {
  RelationId relation;
  std::vector<SemanticCellValue> cells;
};

struct ExecutionRow {
  RelationId relation;
  std::vector<ExecutionCellValue> cells;
};

struct AnalysisFact {
  core::StableId fact_id;
  SemanticRow row;
};

// Validates a semantic row against its relation schema: cell count, per-cell
// domain, stable-ID kind, and canonical unknown-range payloads.
Status ValidateSemanticRow(const SemanticRow& row);

// Validates an execution row against its relation schema: cell count, per-cell
// dense-ID domain, and canonical unknown-range payloads.
Status ValidateExecutionRow(const ExecutionRow& row);

// Validates a semantic row and derives its witness-independent fact ID.
StatusOr<AnalysisFact> MakeFact(const SemanticRow& row);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_ANALYSIS_FACT_H_
