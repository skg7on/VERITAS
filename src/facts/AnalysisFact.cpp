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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

// The rendered form of a stable ID inside the preimage: exactly what
// `core::ToString` returns, assembled without the stream it formats through.
constexpr std::string_view kAlgorithmSeparator = ":sha256:";

bool IsValidRelation(RelationId id) {
  return static_cast<std::size_t>(id) < kRelationCountV2;
}

// The relation name a row contributes to its preimage. A row whose relation id
// is out of range is rejected by validation, but the preimage is what the
// identity memo keys on and it is computed before validation runs, so the
// lookup here must stay in bounds for any row rather than only for valid ones.
// An empty name cannot collide with a real relation's, because a real name is
// never empty and every name is written length-prefixed.
std::string_view PreimageRelationName(RelationId id) {
  return IsValidRelation(id) ? RelationsV2().Get(id).name : std::string_view{};
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

// The canonical preimage of a semantic row: the byte string whose SHA-256 is
// the row's fact identity. Length-prefixed, tag-prefixed fields make
// concatenation injective, so two distinct rows cannot share a preimage --
// which is what lets the identity memo key on it without re-deriving.
//
// The preimage is sized exactly and then written through a cursor. The previous
// encoder appended one byte at a time to a growing vector; the bytes it
// produced are identical, and `DerivedIdentityCoversEveryCellKind` pins them.

std::size_t LenPrefixedSize(std::size_t text_size) { return 8 + text_size; }

// The length of a stable ID's rendered form: `<kind>:sha256:<digest>`. One
// expression, used both to size the field and to write its length prefix.
std::size_t RenderedStableIdSize(const core::StableId& id) {
  return IdKindToString(id.kind).size() + kAlgorithmSeparator.size() +
         id.digest_hex.size();
}

std::size_t CellSize(const SemanticCellValue& cell) {
  return 1 + std::visit(
                 [](const auto& value) -> std::size_t {
                   using T = std::decay_t<decltype(value)>;
                   if constexpr (std::is_same_v<T, core::StableId>) {
                     return LenPrefixedSize(RenderedStableIdSize(value));
                   } else if constexpr (std::is_same_v<T, std::string>) {
                     return LenPrefixedSize(value.size());
                   } else if constexpr (std::is_same_v<T, std::int64_t> ||
                                        std::is_same_v<T, std::uint64_t>) {
                     return 8;
                   } else {
                     return 1;
                   }
                 },
                 cell);
}

std::size_t PreimageSize(const SemanticRow& row) {
  std::size_t size = LenPrefixedSize(std::string_view("relations.v2").size()) +
                     LenPrefixedSize(PreimageRelationName(row.relation).size());
  for (const auto& cell : row.cells) {
    size += CellSize(cell);
  }
  return size;
}

// A bounds-respecting cursor over a buffer of exactly `PreimageSize(row)` bytes.
class PreimageCursor {
public:
  PreimageCursor(std::byte* out, std::size_t size) : out_(out), size_(size) {}

  void Put(const void* data, std::size_t count) {
    if (count == 0) {
      return;
    }
    // The count and the writer are two halves of one encoding. The assert is
    // the debug-build guard that they still agree; the clamp keeps a release
    // build from writing past the buffer if they ever drift, at the cost of a
    // truncated (and therefore different) identity rather than a corrupt heap.
    assert(pos_ + count <= size_);
    if (pos_ + count > size_) {
      pos_ = size_;
      return;
    }
    std::memcpy(out_ + pos_, data, count);
    pos_ += count;
  }

  void Byte(std::uint8_t value) { Put(&value, 1); }

  void U64(std::uint64_t value) {
    std::byte encoded[8];
    for (int i = 7; i >= 0; --i) {
      encoded[7 - i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    }
    Put(encoded, sizeof(encoded));
  }

  void LenPrefixed(std::string_view text) {
    U64(text.size());
    Put(text.data(), text.size());
  }

  void Cell(const SemanticCellValue& cell) {
    std::visit(
        [this](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::StableId>) {
            Byte(static_cast<std::uint8_t>(CellTag::kStableId));
            // The whole rendered id is one length-prefixed field: the prefix
            // counts the kind, the algorithm separator, and the digest.
            const std::string_view kind = IdKindToString(value.kind);
            U64(RenderedStableIdSize(value));
            Put(kind.data(), kind.size());
            Put(kAlgorithmSeparator.data(), kAlgorithmSeparator.size());
            Put(value.digest_hex.data(), value.digest_hex.size());
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            Byte(static_cast<std::uint8_t>(CellTag::kInt64));
            U64(static_cast<std::uint64_t>(value));
          } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            Byte(static_cast<std::uint8_t>(CellTag::kUint64));
            U64(value);
          } else if constexpr (std::is_same_v<T, std::string>) {
            Byte(static_cast<std::uint8_t>(CellTag::kString));
            LenPrefixed(value);
          } else if constexpr (std::is_same_v<T, semantic::DispatchKind>) {
            Byte(static_cast<std::uint8_t>(CellTag::kDispatchKind));
            Byte(static_cast<std::uint8_t>(value));
          } else if constexpr (std::is_same_v<T, semantic::AliasKind>) {
            Byte(static_cast<std::uint8_t>(CellTag::kAliasKind));
            Byte(static_cast<std::uint8_t>(value));
          } else if constexpr (std::is_same_v<T, semantic::ByteRangeKind>) {
            Byte(static_cast<std::uint8_t>(CellTag::kByteRangeKind));
            Byte(static_cast<std::uint8_t>(value));
          } else if constexpr (std::is_same_v<T, semantic::EpistemicState>) {
            Byte(static_cast<std::uint8_t>(CellTag::kEpistemic));
            Byte(static_cast<std::uint8_t>(value));
          }
        },
        cell);
  }

  bool Complete() const { return pos_ == size_; }

private:
  std::byte* out_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// Writes the preimage of `row` into `out`, which must hold at least
// `PreimageSize(row)` bytes.
void WritePreimage(std::byte* out, std::size_t size, const SemanticRow& row) {
  PreimageCursor cursor(out, size);
  cursor.LenPrefixed("relations.v2");
  cursor.LenPrefixed(PreimageRelationName(row.relation));
  for (const auto& cell : row.cells) {
    cursor.Cell(cell);
  }
  // The writer and the sizing pass are the same encoding expressed twice; this
  // is where a divergence between them is caught rather than hashed.
  assert(cursor.Complete());
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

StatusOr<core::StableId> DeriveFactId(const SemanticRow& row) {
  if (Status s = ValidateSemanticRow(row); !s.ok()) {
    return s;
  }
  std::vector<std::byte> bytes(PreimageSize(row));
  WritePreimage(bytes.data(), bytes.size(), row);
  return core::MakeStableId(core::IdKind::kFact, bytes);
}

StatusOr<core::StableId> FactIdentityMemo::Identify(const SemanticRow& row) {
  key_.resize(PreimageSize(row));
  WritePreimage(reinterpret_cast<std::byte*>(key_.data()), key_.size(), row);
  const auto cached = ids_.find(key_);
  if (cached != ids_.end()) {
    return cached->second;
  }
  // A miss always derives from the row. Storing only successful derivations is
  // what keeps a rejected row rejected: it never gets a memo entry that a later
  // lookup could answer with.
  auto derived = DeriveFactId(row);
  if (derived.ok()) {
    ids_.emplace(key_, *derived);
  }
  return derived;
}

StatusOr<AnalysisFact> MakeFact(const SemanticRow& row) {
  auto fact_id = DeriveFactId(row);
  if (!fact_id.ok()) {
    return fact_id.status();
  }
  AnalysisFact fact;
  fact.fact_id = std::move(*fact_id);
  fact.row = row;
  return fact;
}

}  // namespace veritas::facts
