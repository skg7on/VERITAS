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
// One traversal produces both the length and the bytes. `PreimageCursor` has
// two modes: a measuring cursor has no buffer, advances a position and writes
// nothing, so `PreimageSize` is a dry run of the same writer. The byte format
// is therefore expressed once. It used to be expressed twice -- a sizing pass
// and a writer that had to be kept in step by hand -- and a cell alternative
// handled by one and not the other is not a loud failure: it truncates the
// preimage of every row carrying it, and two rows differing only in that cell
// then share one fact id.
//
// The bytes are unchanged by this: `DerivedIdentityCoversEveryCellKind` pins
// the fact ids of ten rows covering every cell kind, both range payloads, a
// negative offset, and a string carrying a NUL and a colon.

// The big-endian 8-byte form a length prefix and a number cell both take.
void EncodeU64(std::uint64_t value, std::byte* out) {
  for (int i = 7; i >= 0; --i) {
    out[7 - i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
  }
}

// A cell alternative this encoder does not know. A new alternative in
// `SemanticCellValue` must be encoded here; leaving it out would let two rows
// that differ only in it derive the same fact id, which merges facts rather
// than rejecting a batch. The dependent-false form is what makes the
// `static_assert` below fire instead of being accepted as an unreachable
// statement.
template <typename T>
[[maybe_unused]] inline constexpr bool kUnencodedCellKind = false;

// A cursor over the preimage. Constructed with `out == nullptr` it measures;
// constructed over a buffer of exactly `PreimageSize(row)` bytes it writes.
class PreimageCursor {
public:
  PreimageCursor() = default;
  PreimageCursor(std::byte* out, std::size_t size) : out_(out), size_(size) {}

  // The number of bytes written, or, in a measuring pass, that would be.
  std::size_t Position() const { return pos_; }

  void Put(const void* data, std::size_t count) {
    if (count == 0) {
      return;
    }
    if (out_ == nullptr) {
      pos_ += count;
      return;
    }
    // The clamp keeps a release build from writing past the buffer if the
    // measure and the write ever disagree, at the cost of a truncated -- and
    // therefore different -- identity rather than a corrupt heap. The size
    // came from a dry run of this same writer, so the assert is what a mismatch
    // would be caught by in a build that keeps asserts.
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
    EncodeU64(value, encoded);
    Put(encoded, sizeof(encoded));
  }

  // Begins a length-prefixed field and returns the offset of its prefix, for
  // `EndField`. The prefix is filled in from the bytes the body wrote, so it
  // cannot disagree with them; a measuring pass writes no prefix and needs
  // none, because the position already holds the value it must carry.
  std::size_t BeginField() {
    const std::size_t at = pos_;
    U64(0);
    return at;
  }

  void EndField(std::size_t field_at) {
    // `BeginField` reserved these eight bytes, so this is in bounds whenever
    // the measure held; the bound check is the same release-mode backstop
    // `Put` carries, so that no path writes past the buffer even if a future
    // field is miscounted.
    if (out_ == nullptr || field_at + 8 > size_) {
      return;
    }
    EncodeU64(pos_ - field_at - 8, out_ + field_at);
  }

  void LenPrefixed(std::string_view text) {
    const std::size_t at = BeginField();
    Put(text.data(), text.size());
    EndField(at);
  }

  void Cell(const SemanticCellValue& cell) {
    std::visit(
        [this](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::StableId>) {
            Byte(static_cast<std::uint8_t>(CellTag::kStableId));
            // The whole rendered id is one field: the prefix counts the kind,
            // the algorithm separator, and the digest together.
            const std::size_t at = BeginField();
            const std::string_view kind = IdKindToString(value.kind);
            Put(kind.data(), kind.size());
            Put(kAlgorithmSeparator.data(), kAlgorithmSeparator.size());
            Put(value.digest_hex.data(), value.digest_hex.size());
            EndField(at);
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
          } else {
            static_assert(kUnencodedCellKind<T>,
                          "SemanticCellValue gained an alternative the preimage "
                          "encoding does not know, so rows differing only in "
                          "that cell would derive one fact id");
          }
        },
        cell);
  }

private:
  std::byte* out_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
};

// The preimage of `row` through `cursor`: the single expression of the byte
// format, used both to measure it and to write it.
void WritePreimage(PreimageCursor& cursor, const SemanticRow& row) {
  cursor.LenPrefixed("relations.v2");
  cursor.LenPrefixed(PreimageRelationName(row.relation));
  for (const auto& cell : row.cells) {
    cursor.Cell(cell);
  }
}

// The exact byte count of `row`'s preimage: a dry run of the writer that will
// produce it.
std::size_t PreimageSize(const SemanticRow& row) {
  PreimageCursor measuring;
  WritePreimage(measuring, row);
  return measuring.Position();
}

// Writes the preimage of `row` into `out`, which must hold exactly `size`
// bytes.
void WritePreimage(std::byte* out, std::size_t size, const SemanticRow& row) {
  PreimageCursor cursor(out, size);
  WritePreimage(cursor, row);
  assert(cursor.Position() == size);
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
