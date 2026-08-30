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

#include "veritas/facts/AnalysisFact.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace veritas::facts {

namespace {

enum class CellTag : std::uint8_t {
  kStableId = 0,
  kInt64 = 1,
  kUint64 = 2,
  kString = 3,
  kDispatchKind = 4,
  kAliasKind = 5,
  kByteRangeKind = 6,
  kEpistemic = 7,
};

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendLenPrefixed(std::vector<std::byte>& out, std::string_view text) {
  const std::uint64_t size = text.size();
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<std::byte>((size >> (i * 8)) & 0xFF));
  }
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
  }
}

void AppendU64(std::vector<std::byte>& out, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFF));
  }
}

void AppendI64(std::vector<std::byte>& out, std::int64_t value) {
  AppendU64(out, static_cast<std::uint64_t>(value));
}

bool IsValidRelation(RelationId id) {
  return static_cast<std::size_t>(id) < kRelationCountV2;
}

bool IsRangeRelation(RelationId id) {
  return id == RelationId::kDirectRead || id == RelationId::kDirectWrite;
}

Status MatchStableId(const SemanticCellValue& cell, core::IdKind kind) {
  const auto* id = std::get_if<core::StableId>(&cell);
  if (id == nullptr) {
    return Status::InvalidArgument("expected stable id cell");
  }
  if (id->kind != kind) {
    return Status::InvalidArgument("stable id kind mismatch");
  }
  return Status::Ok();
}

template <typename T>
Status MatchSemanticType(const SemanticCellValue& cell) {
  return std::holds_alternative<T>(cell)
             ? Status::Ok()
             : Status::InvalidArgument("semantic cell type mismatch");
}

template <typename T>
Status MatchExecutionType(const ExecutionCellValue& cell) {
  return std::holds_alternative<T>(cell)
             ? Status::Ok()
             : Status::InvalidArgument("execution cell type mismatch");
}

Status ValidateSemanticCell(ColumnDomain domain, const SemanticCellValue& cell) {
  switch (domain) {
  case ColumnDomain::kFunctionId:
    return MatchStableId(cell, core::IdKind::kFunctionVariant);
  case ColumnDomain::kValueId:
    return MatchStableId(cell, core::IdKind::kValueRef);
  case ColumnDomain::kMemoryId:
    return MatchStableId(cell, core::IdKind::kMemoryRef);
  case ColumnDomain::kCallSiteId:
    return MatchStableId(cell, core::IdKind::kCallSite);
  case ColumnDomain::kFactId:
    return MatchStableId(cell, core::IdKind::kFact);
  case ColumnDomain::kModelId:
    return MatchStableId(cell, core::IdKind::kModel);
  case ColumnDomain::kInt64:
    return MatchSemanticType<std::int64_t>(cell);
  case ColumnDomain::kUint64:
    return MatchSemanticType<std::uint64_t>(cell);
  case ColumnDomain::kString:
    return MatchSemanticType<std::string>(cell);
  case ColumnDomain::kDispatchKind:
    return MatchSemanticType<semantic::DispatchKind>(cell);
  case ColumnDomain::kAliasKind:
    return MatchSemanticType<semantic::AliasKind>(cell);
  case ColumnDomain::kByteRangeKind:
    return MatchSemanticType<semantic::ByteRangeKind>(cell);
  case ColumnDomain::kEpistemic:
    return MatchSemanticType<semantic::EpistemicState>(cell);
  }
  return Status::InvalidArgument("unknown column domain");
}

Status ValidateExecutionCell(ColumnDomain domain,
                             const ExecutionCellValue& cell) {
  switch (domain) {
  case ColumnDomain::kFunctionId:
    return MatchExecutionType<FunctionId>(cell);
  case ColumnDomain::kValueId:
    return MatchExecutionType<ValueId>(cell);
  case ColumnDomain::kMemoryId:
    return MatchExecutionType<MemoryId>(cell);
  case ColumnDomain::kCallSiteId:
    return MatchExecutionType<CallSiteId>(cell);
  case ColumnDomain::kFactId:
    return MatchExecutionType<FactId>(cell);
  case ColumnDomain::kModelId:
    return MatchExecutionType<std::string>(cell);
  case ColumnDomain::kInt64:
    return MatchExecutionType<std::int64_t>(cell);
  case ColumnDomain::kUint64:
    return MatchExecutionType<std::uint64_t>(cell);
  case ColumnDomain::kString:
    return MatchExecutionType<std::string>(cell);
  case ColumnDomain::kDispatchKind:
    return MatchExecutionType<semantic::DispatchKind>(cell);
  case ColumnDomain::kAliasKind:
    return MatchExecutionType<semantic::AliasKind>(cell);
  case ColumnDomain::kByteRangeKind:
    return MatchExecutionType<semantic::ByteRangeKind>(cell);
  case ColumnDomain::kEpistemic:
    return MatchExecutionType<semantic::EpistemicState>(cell);
  }
  return Status::InvalidArgument("unknown column domain");
}

// DirectRead/DirectWrite carry (range_kind, offset, size) at columns 2..4.
// UNKNOWN requires canonical zero payload cells; a known zero range is
// distinguished by its KNOWN tag and never collapses into unknown.
Status ValidateSemanticRangePayload(const SemanticRow& row) {
  if (!IsRangeRelation(row.relation)) {
    return Status::Ok();
  }
  const auto* range_kind = std::get_if<semantic::ByteRangeKind>(&row.cells[2]);
  if (range_kind == nullptr || *range_kind == semantic::ByteRangeKind::kKnown) {
    return range_kind == nullptr ? Status::InvalidArgument("missing range kind")
                                 : Status::Ok();
  }
  const auto* offset = std::get_if<std::int64_t>(&row.cells[3]);
  const auto* size = std::get_if<std::uint64_t>(&row.cells[4]);
  if (offset == nullptr || size == nullptr) {
    return Status::InvalidArgument("missing range payload");
  }
  if (*offset != 0 || *size != 0) {
    return Status::InvalidArgument("non-canonical unknown range payload");
  }
  return Status::Ok();
}

Status ValidateExecutionRangePayload(const ExecutionRow& row) {
  if (!IsRangeRelation(row.relation)) {
    return Status::Ok();
  }
  const auto* range_kind = std::get_if<semantic::ByteRangeKind>(&row.cells[2]);
  if (range_kind == nullptr || *range_kind == semantic::ByteRangeKind::kKnown) {
    return range_kind == nullptr ? Status::InvalidArgument("missing range kind")
                                 : Status::Ok();
  }
  const auto* offset = std::get_if<std::int64_t>(&row.cells[3]);
  const auto* size = std::get_if<std::uint64_t>(&row.cells[4]);
  if (offset == nullptr || size == nullptr) {
    return Status::InvalidArgument("missing range payload");
  }
  if (*offset != 0 || *size != 0) {
    return Status::InvalidArgument("non-canonical unknown range payload");
  }
  return Status::Ok();
}

void AppendCell(std::vector<std::byte>& out, const SemanticCellValue& cell) {
  std::visit(
      [&out](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kStableId));
          AppendLenPrefixed(out, core::ToString(value));
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kInt64));
          AppendI64(out, value);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kUint64));
          AppendU64(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kString));
          AppendLenPrefixed(out, value);
        } else if constexpr (std::is_same_v<T, semantic::DispatchKind>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kDispatchKind));
          AppendU8(out, static_cast<std::uint8_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::AliasKind>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kAliasKind));
          AppendU8(out, static_cast<std::uint8_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::ByteRangeKind>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kByteRangeKind));
          AppendU8(out, static_cast<std::uint8_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::EpistemicState>) {
          AppendU8(out, static_cast<std::uint8_t>(CellTag::kEpistemic));
          AppendU8(out, static_cast<std::uint8_t>(value));
        }
      },
      cell);
}

}  // namespace

Status ValidateSemanticRow(const SemanticRow& row) {
  if (!IsValidRelation(row.relation)) {
    return Status::InvalidArgument("unknown relation id");
  }
  const RelationSchema& schema = RelationsV2().Get(row.relation);
  if (row.cells.size() != schema.columns.size()) {
    return Status::InvalidArgument("semantic cell count mismatch");
  }
  for (std::size_t i = 0; i < row.cells.size(); ++i) {
    if (Status s = ValidateSemanticCell(schema.columns[i].domain, row.cells[i]);
        !s.ok()) {
      return s;
    }
  }
  return ValidateSemanticRangePayload(row);
}

Status ValidateExecutionRow(const ExecutionRow& row) {
  if (!IsValidRelation(row.relation)) {
    return Status::InvalidArgument("unknown relation id");
  }
  const RelationSchema& schema = RelationsV2().Get(row.relation);
  if (row.cells.size() != schema.columns.size()) {
    return Status::InvalidArgument("execution cell count mismatch");
  }
  for (std::size_t i = 0; i < row.cells.size(); ++i) {
    if (Status s = ValidateExecutionCell(schema.columns[i].domain, row.cells[i]);
        !s.ok()) {
      return s;
    }
  }
  return ValidateExecutionRangePayload(row);
}

StatusOr<AnalysisFact> MakeFact(const SemanticRow& row) {
  if (Status s = ValidateSemanticRow(row); !s.ok()) {
    return s;
  }
  std::vector<std::byte> bytes;
  AppendLenPrefixed(bytes, "relations.v2");
  AppendLenPrefixed(bytes, RelationsV2().Get(row.relation).name);
  for (const auto& cell : row.cells) {
    AppendCell(bytes, cell);
  }
  AnalysisFact fact;
  fact.fact_id = core::MakeStableId(core::IdKind::kFact, bytes);
  fact.row = row;
  return fact;
}

}  // namespace veritas::facts
