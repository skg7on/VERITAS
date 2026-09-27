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

#include "veritas/facts/RowArena.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/facts/SemanticKeyCodec.h"

namespace veritas::facts {
namespace {

// Cell tags. The order mirrors the `SemanticCellValue` alternative order, and
// an alternative added there must be added here: `AppendCell`'s static_assert
// fires on the write side and the switches below fail to compile on the read
// side, which is what keeps a new alternative from silently sharing a tag with
// an existing one. Witness.cpp's `kUnencodedCellKind` guards the semantic-key
// encoder the same way.
enum class CellTag : std::uint8_t {
  kId = 0,
  kSigned,
  kUnsigned,
  kSymbol,
  kDispatch,
  kAlias,
  kByteRange,
  kEpistemic,
};

// A cell alternative this encoder does not know. The dependent-false form is
// what makes `AppendCell`'s static_assert fire instead of being accepted as an
// unreachable statement.
template <typename T>
[[maybe_unused]] inline constexpr bool kUnencodedArenaCell = false;

// A stable id body: the kind byte and the 32 digest bytes.
constexpr std::size_t kIdBodyBytes = 1 + core::kSHA256DigestBytes;

// The narrowest a cell can be: a tag byte and the eight-byte length prefix a
// symbol carries. A cell count that cannot fit in the bytes left is a corrupt
// row rather than a large one, and rejecting it before any reserve keeps a
// forged count from asking for an allocation this build cannot recover from.
constexpr std::size_t kMinCellBytes = 9;

// The arena's numbers are native-order and native-width. Nothing here is a
// durable format -- the arena is memory only, and the run's cached objects are
// written by WpaRunRepository -- so a row never has to survive a byte-order
// change the way a fact id's preimage does.

void PutU32(std::string* out, std::uint32_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void PutU64(std::string* out, std::uint64_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void PutBytes(std::string* out, std::string_view value) {
  PutU64(out, value.size());
  out->append(value);
}

void PutTag(std::string* out, CellTag tag) {
  out->push_back(static_cast<char>(static_cast<std::uint8_t>(tag)));
}

std::uint32_t GetU32(std::string_view in, std::size_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, in.data() + at, sizeof(value));
  return value;
}

std::uint64_t GetU64(std::string_view in, std::size_t at) {
  std::uint64_t value = 0;
  std::memcpy(&value, in.data() + at, sizeof(value));
  return value;
}

StatusOr<std::byte> Nibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<std::byte>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<std::byte>(c - 'a' + 10);
  return Status::InvalidArgument("stable id digest is not lowercase hex");
}

// Decodes the canonical 64-character digest into bytes once, at append time, so
// no reader ever pays for the text form of an id it does not render.
//
// All-or-nothing: a rejected digest is one this walks part way through, and a
// caller that had to remember to roll back would eventually forget, leaving
// bytes behind for a later handle to point into.
Status AppendId(std::string* out, const core::StableId& id) {
  const std::size_t start = out->size();
  if (id.digest_hex.size() != core::kSHA256DigestBytes * 2) {
    return Status::InvalidArgument("stable id digest has the wrong length");
  }
  out->push_back(static_cast<char>(id.kind));
  for (std::size_t i = 0; i < core::kSHA256DigestBytes; ++i) {
    const auto high = Nibble(id.digest_hex[2 * i]);
    const auto low = Nibble(id.digest_hex[2 * i + 1]);
    // Both nibbles are read before either is committed, so a bad low nibble
    // cannot leave half a byte behind, and the one rollback site below covers
    // both.
    if (!high.ok() || !low.ok()) {
      out->resize(start);
      return high.ok() ? low.status() : high.status();
    }
    const auto byte = static_cast<std::uint8_t>(
        (std::to_integer<std::uint8_t>(*high) << 4) |
        std::to_integer<std::uint8_t>(*low));
    out->push_back(static_cast<char>(byte));
  }
  return Status::Ok();
}

Status AppendCell(std::string* out, const SemanticCellValue& cell) {
  Status status = Status::Ok();
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          PutTag(out, CellTag::kId);
          status = AppendId(out, value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          PutTag(out, CellTag::kSigned);
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          PutTag(out, CellTag::kUnsigned);
          PutU64(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          PutTag(out, CellTag::kSymbol);
          PutBytes(out, value);
        } else if constexpr (std::is_same_v<T, semantic::DispatchKind>) {
          PutTag(out, CellTag::kDispatch);
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::AliasKind>) {
          PutTag(out, CellTag::kAlias);
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::ByteRangeKind>) {
          PutTag(out, CellTag::kByteRange);
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, semantic::EpistemicState>) {
          PutTag(out, CellTag::kEpistemic);
          PutU64(out, static_cast<std::uint64_t>(value));
        } else {
          static_assert(kUnencodedArenaCell<T>,
                        "SemanticCellValue gained an alternative the arena "
                        "encoding does not know, so two rows differing only in "
                        "that cell would store the same bytes");
        }
      },
      cell);
  return status;
}

// The row encoding, written once: `Append`, `AppendFact` and `AppendWitness`
// all store rows through it, so a witness's result row and a bare row decode
// with one reader. Rolls the buffer back on failure, because a partial row left
// behind would be bytes a later handle could point into.
Status AppendRow(std::string* out, const SemanticRow& row) {
  const std::size_t start = out->size();
  PutU32(out, static_cast<std::uint32_t>(row.relation));
  PutU32(out, static_cast<std::uint32_t>(row.cells.size()));
  for (const SemanticCellValue& cell : row.cells) {
    if (Status appended = AppendCell(out, cell); !appended.ok()) {
      out->resize(start);
      return appended;
    }
  }
  return Status::Ok();
}

// A bounds-checked position in one stored entry's bytes.
//
// Every read is checked against the range this was constructed over and
// advances one position, so a truncated or forged range fails rather than
// reading past the buffer, and the two callers that walk a row -- materialising
// it and rendering its key -- cannot disagree about where a cell ends.
class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  std::size_t remaining() const { return bytes_.size() - pos_; }
  bool at_end() const { return pos_ == bytes_.size(); }

  StatusOr<std::uint8_t> Byte() {
    auto value = Fixed(1);
    if (!value.ok()) return value.status();
    return static_cast<std::uint8_t>((*value)[0]);
  }

  StatusOr<std::uint32_t> U32() {
    auto value = Fixed(sizeof(std::uint32_t));
    if (!value.ok()) return value.status();
    return GetU32(*value, 0);
  }

  StatusOr<std::uint64_t> U64() {
    auto value = Fixed(sizeof(std::uint64_t));
    if (!value.ok()) return value.status();
    return GetU64(*value, 0);
  }

  // `count` raw bytes. Advancing by the read rather than by a remembered offset
  // is what keeps the position and the value in step.
  StatusOr<std::string_view> Fixed(std::size_t count) {
    if (remaining() < count) return Truncated();
    const std::string_view value = bytes_.substr(pos_, count);
    pos_ += count;
    return value;
  }

  // An eight-byte length and the bytes it counts.
  StatusOr<std::string_view> LengthPrefixed() {
    auto length = U64();
    if (!length.ok()) return length.status();
    if (*length > remaining()) return Truncated();
    return Fixed(static_cast<std::size_t>(*length));
  }

 private:
  static Status Truncated() {
    return Status::InvalidArgument("stored entry is truncated");
  }

  std::string_view bytes_;
  std::size_t pos_ = 0;
};

// The body width a tag carries. A symbol's is length-prefixed instead, which is
// why it is not listed. An enumerator added to `CellTag` fails to compile here,
// the read-side counterpart of `AppendCell`'s static_assert.
std::size_t BodyBytes(CellTag tag) {
  switch (tag) {
    case CellTag::kId:
      return kIdBodyBytes;
    case CellTag::kSigned:
    case CellTag::kUnsigned:
    case CellTag::kDispatch:
    case CellTag::kAlias:
    case CellTag::kByteRange:
    case CellTag::kEpistemic:
      return sizeof(std::uint64_t);
    case CellTag::kSymbol:
      return 0;  // length-prefixed; ReadCell consumes it instead
  }
  return 0;
}

// Reads one cell's tag and the bytes of its body, with the framing already
// consumed: 33 bytes for a stable id, 8 for a number or an enum ordinal, the
// symbol's own bytes for a symbol.
Status ReadCell(Reader* reader, CellTag* tag, std::string_view* body) {
  auto raw = reader->Byte();
  if (!raw.ok()) return raw.status();
  if (*raw > static_cast<std::uint8_t>(CellTag::kEpistemic)) {
    return Status::InvalidArgument("stored entry has an unknown cell tag");
  }
  *tag = static_cast<CellTag>(*raw);
  auto payload = *tag == CellTag::kSymbol ? reader->LengthPrefixed()
                                          : reader->Fixed(BodyBytes(*tag));
  if (!payload.ok()) return payload.status();
  *body = *payload;
  return Status::Ok();
}

// The kind a stored id body carries. The ordinal is what is stored, so the
// round trip never has to hold the kind's spelling.
core::IdKind IdKindOf(std::string_view body) {
  return static_cast<core::IdKind>(static_cast<std::uint8_t>(body[0]));
}

// The 32 stored digest bytes as the digest `DigestToHex` renders.
core::SHA256Digest DigestAt(std::string_view digest_bytes) {
  core::SHA256Digest digest{};
  std::memcpy(digest.data(), digest_bytes.data(), digest.size());
  return digest;
}

// The digest bytes an id cell's body carries, past its kind byte.
core::SHA256Digest DigestOf(std::string_view body) {
  return DigestAt(body.substr(1));
}

// Reads one stored stable id: the kind byte and the 32 digest bytes. A fact's
// id is a field of its entry rather than a cell, so it is read directly; a
// cell's body carries the same two pieces and is read by ReadCell.
Status ReadId(Reader* reader, core::StableId* id) {
  auto kind = reader->Byte();
  if (!kind.ok()) return kind.status();
  auto digest = reader->Fixed(core::kSHA256DigestBytes);
  if (!digest.ok()) return digest.status();
  *id = core::StableId{static_cast<core::IdKind>(*kind),
                       core::DigestToHex(DigestAt(*digest))};
  return Status::Ok();
}

// Reads a length-prefixed string into `out`.
Status ReadLengthPrefixed(Reader* reader, std::string* out) {
  auto value = reader->LengthPrefixed();
  if (!value.ok()) return value.status();
  out->assign(*value);
  return Status::Ok();
}

// The canonical text of a stored id, re-rendered from the kind and the digest.
// `core::ToString` is the authority for the text form -- the rich encoder in
// Witness.cpp renders every id cell through it -- so a key built here and a key
// built from the rich row cannot drift.
std::string RenderedId(std::string_view body) {
  return core::ToString(
      core::StableId{IdKindOf(body), core::DigestToHex(DigestOf(body))});
}

StatusOr<SemanticCellValue> DecodeCell(CellTag tag, std::string_view body) {
  switch (tag) {
    case CellTag::kId:
      return SemanticCellValue{
          core::StableId{IdKindOf(body), core::DigestToHex(DigestOf(body))}};
    case CellTag::kSigned:
      return SemanticCellValue{static_cast<std::int64_t>(GetU64(body, 0))};
    case CellTag::kUnsigned:
      return SemanticCellValue{GetU64(body, 0)};
    case CellTag::kSymbol:
      return SemanticCellValue{std::string(body)};
    case CellTag::kDispatch:
      return SemanticCellValue{
          static_cast<semantic::DispatchKind>(GetU64(body, 0))};
    case CellTag::kAlias:
      return SemanticCellValue{
          static_cast<semantic::AliasKind>(GetU64(body, 0))};
    case CellTag::kByteRange:
      return SemanticCellValue{
          static_cast<semantic::ByteRangeKind>(GetU64(body, 0))};
    case CellTag::kEpistemic:
      return SemanticCellValue{
          static_cast<semantic::EpistemicState>(GetU64(body, 0))};
  }
  return Status::InvalidArgument("stored entry has an unknown cell tag");
}

// Reads one row from the front of `reader`, leaving the position at its end.
// The caller decides whether the position must then sit at the end of the
// entry, because a witness entry packs two rows and their fields into one span.
Status ReadRow(Reader* reader, SemanticRow* row) {
  auto relation = reader->U32();
  if (!relation.ok()) return relation.status();
  auto count = reader->U32();
  if (!count.ok()) return count.status();
  if (*count > reader->remaining() / kMinCellBytes) {
    return Status::InvalidArgument(
        "stored row claims more cells than it holds");
  }
  std::vector<SemanticCellValue> cells;
  cells.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    CellTag tag = CellTag::kId;
    std::string_view body;
    if (Status read = ReadCell(reader, &tag, &body); !read.ok()) return read;
    auto cell = DecodeCell(tag, body);
    if (!cell.ok()) return cell.status();
    cells.push_back(std::move(*cell));
  }
  row->relation = static_cast<RelationId>(*relation);
  row->cells = std::move(cells);
  return Status::Ok();
}

// Appends a row's canonical key to `out`, from the stored cells rather than
// from a decoded row: the fields go through the same codec primitives the rich
// `AppendSemanticKey` uses, with the same tags, and an id is re-rendered rather
// than re-parsed. The two encoders therefore produce identical bytes, which the
// batch id depends on.
Status WriteKey(Reader* reader, std::string* out) {
  auto relation = reader->U32();
  if (!relation.ok()) return relation.status();
  auto count = reader->U32();
  if (!count.ok()) return count.status();
  const RelationSchema& schema =
      RelationsV2().Get(static_cast<RelationId>(*relation));
  AppendKeyHeader(out, schema.name, *count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    CellTag tag = CellTag::kId;
    std::string_view body;
    if (Status read = ReadCell(reader, &tag, &body); !read.ok()) return read;
    switch (tag) {
      case CellTag::kId:
        AppendField(out, KeyFieldTag::kId, RenderedId(body));
        break;
      case CellTag::kSymbol:
        AppendField(out, KeyFieldTag::kSymbol, body);
        break;
      case CellTag::kSigned:
        AppendField(out, KeyFieldTag::kNumber,
                    std::to_string(static_cast<std::int64_t>(GetU64(body, 0))));
        break;
      case CellTag::kUnsigned:
        AppendField(out, KeyFieldTag::kUnsigned,
                    std::to_string(GetU64(body, 0)));
        break;
      case CellTag::kDispatch:
      case CellTag::kAlias:
      case CellTag::kByteRange:
      case CellTag::kEpistemic:
        AppendField(out, KeyFieldTag::kEnum, std::to_string(GetU64(body, 0)));
        break;
    }
  }
  return Status::Ok();
}

// Reads exactly one row from `bytes`: anything left over means `bytes` was not
// one row, so a handle that names an entry boundary fails rather than decoding
// as a shorter valid row.
Status ReadWholeRow(std::string_view bytes, SemanticRow* row) {
  Reader reader(bytes);
  if (Status read = ReadRow(&reader, row); !read.ok()) return read;
  if (!reader.at_end()) {
    return Status::InvalidArgument("stored row has trailing bytes");
  }
  return Status::Ok();
}

}  // namespace

StatusOr<RowHandle> RowArena::Append(const SemanticRow& row) {
  const std::size_t start = buffer_.size();
  if (Status appended = AppendRow(&buffer_, row); !appended.ok()) {
    return appended;
  }
  const RowHandle handle{start, buffer_.size() - start};
  entries_.push_back(Entry{handle, EntryKind::kRow, handle, RowHandle{}});
  return handle;
}

StatusOr<SemanticRow> RowArena::Decode(RowHandle handle) const {
  if (FindRowOwner(handle) == nullptr) {
    return Status::InvalidArgument("handle does not name a stored row");
  }
  SemanticRow row;
  if (Status read = ReadWholeRow(Span(handle), &row); !read.ok()) {
    return read;
  }
  return row;
}

Status RowArena::AppendKey(RowHandle handle, std::string* out) const {
  if (FindRowOwner(handle) == nullptr) {
    return Status::InvalidArgument("handle does not name a stored row");
  }
  Reader reader(Span(handle));
  if (Status written = WriteKey(&reader, out); !written.ok()) return written;
  if (!reader.at_end()) {
    return Status::InvalidArgument("stored row has trailing bytes");
  }
  return Status::Ok();
}

bool RowArena::RowEquals(RowHandle left, RowHandle right) const {
  if (FindRowOwner(left) == nullptr || FindRowOwner(right) == nullptr) {
    return false;
  }
  const std::string_view left_bytes = Span(left);
  const std::string_view right_bytes = Span(right);
  // The ranges are the row encodings, so equality here is exactly "identical
  // cells" and neither side is decoded.
  return left_bytes.size() == right_bytes.size() &&
         std::memcmp(left_bytes.data(), right_bytes.data(),
                     left_bytes.size()) == 0;
}

StatusOr<FactHandles> RowArena::AppendFact(const AnalysisFact& fact) {
  const std::size_t start = buffer_.size();
  // The id comes first, so the row sub-range is one contiguous span and a
  // reader that compares rows addresses it without knowing the id's width.
  // AppendId rolls itself back, so a rejected digest leaves no bytes behind.
  if (Status appended = AppendId(&buffer_, fact.fact_id); !appended.ok()) {
    return appended;
  }
  const std::size_t row_at = buffer_.size();
  if (Status appended = AppendRow(&buffer_, fact.row); !appended.ok()) {
    buffer_.resize(start);
    return appended;
  }
  const RowHandle entry{start, buffer_.size() - start};
  const RowHandle row{row_at, buffer_.size() - row_at};
  entries_.push_back(Entry{entry, EntryKind::kFact, row, RowHandle{}});
  return FactHandles{entry, row};
}

StatusOr<WitnessHandles> RowArena::AppendWitness(const WitnessEdge& edge) {
  const std::size_t start = buffer_.size();
  // The layout follows the durable cache's witness record -- result row, rule
  // id, input row, ordinal -- so the two encodings read alike. The derivation
  // key is not in that record, because it is a grouping handle and not durable,
  // but a decoded edge must carry it, so the arena keeps it between the two.
  if (Status appended = AppendRow(&buffer_, edge.result.row); !appended.ok()) {
    return appended;
  }
  const std::size_t result_end = buffer_.size();
  PutBytes(&buffer_, edge.rule_id);
  PutBytes(&buffer_, edge.derivation_key);
  const std::size_t input_at = buffer_.size();
  if (Status appended = AppendRow(&buffer_, edge.input.row); !appended.ok()) {
    buffer_.resize(start);
    return appended;
  }
  const std::size_t input_end = buffer_.size();
  PutU32(&buffer_, edge.input_ordinal);
  const RowHandle entry{start, buffer_.size() - start};
  const RowHandle result_row{start, result_end - start};
  const RowHandle input_row{input_at, input_end - input_at};
  entries_.push_back(
      Entry{entry, EntryKind::kWitness, result_row, input_row});
  return WitnessHandles{entry, result_row, input_row};
}

StatusOr<AnalysisFact> RowArena::DecodeFact(RowHandle entry) const {
  const Entry* found = FindEntry(entry);
  if (found == nullptr || found->kind != EntryKind::kFact) {
    return Status::InvalidArgument("handle does not name a stored fact");
  }
  Reader reader(Span(entry));
  AnalysisFact fact;
  if (Status read = ReadId(&reader, &fact.fact_id); !read.ok()) return read;
  if (Status read = ReadRow(&reader, &fact.row); !read.ok()) return read;
  if (!reader.at_end()) {
    return Status::InvalidArgument("stored fact has trailing bytes");
  }
  return fact;
}

StatusOr<WitnessEdge> RowArena::DecodeWitness(RowHandle entry) const {
  const Entry* found = FindEntry(entry);
  if (found == nullptr || found->kind != EntryKind::kWitness) {
    return Status::InvalidArgument("handle does not name a stored witness");
  }
  Reader reader(Span(entry));
  WitnessEdge edge;
  if (Status read = ReadRow(&reader, &edge.result.row); !read.ok()) return read;
  if (Status read = ReadLengthPrefixed(&reader, &edge.rule_id); !read.ok()) {
    return read;
  }
  if (Status read = ReadLengthPrefixed(&reader, &edge.derivation_key);
      !read.ok()) {
    return read;
  }
  if (Status read = ReadRow(&reader, &edge.input.row); !read.ok()) return read;
  auto ordinal = reader.U32();
  if (!ordinal.ok()) return ordinal.status();
  edge.input_ordinal = *ordinal;
  if (!reader.at_end()) {
    return Status::InvalidArgument("stored witness has trailing bytes");
  }
  return edge;
}

StatusOr<RowHandle> RowArena::handle_at(std::size_t index) const {
  if (index >= entries_.size()) {
    return Status::InvalidArgument("entry index is past the end of the arena");
  }
  return entries_[index].entry;
}

StatusOr<RowHandle> RowArena::fact_row_handle_at(std::size_t index) const {
  if (index >= entries_.size() || entries_[index].kind != EntryKind::kFact) {
    return Status::InvalidArgument("entry is not a stored fact");
  }
  return entries_[index].first_row;
}

StatusOr<RowHandle> RowArena::witness_result_row_handle_at(
    std::size_t index) const {
  if (index >= entries_.size() || entries_[index].kind != EntryKind::kWitness) {
    return Status::InvalidArgument("entry is not a stored witness");
  }
  return entries_[index].first_row;
}

StatusOr<RowHandle> RowArena::witness_input_row_handle_at(
    std::size_t index) const {
  if (index >= entries_.size() || entries_[index].kind != EntryKind::kWitness) {
    return Status::InvalidArgument("entry is not a stored witness");
  }
  return entries_[index].second_row;
}

bool RowArena::RowsEqual(const RowArena& left, RowHandle left_row,
                         const RowArena& right, RowHandle right_row) {
  // Each handle is resolved against its own arena, so a caller cannot compare
  // an entry against a row by passing the two in the wrong order.
  if (left.FindRowOwner(left_row) == nullptr ||
      right.FindRowOwner(right_row) == nullptr) {
    return false;
  }
  const std::string_view left_bytes = left.Span(left_row);
  const std::string_view right_bytes = right.Span(right_row);
  // The ranges are the row encodings, so equality here is exactly "identical
  // cells" and neither side is decoded.
  return left_bytes.size() == right_bytes.size() &&
         std::memcmp(left_bytes.data(), right_bytes.data(),
                     left_bytes.size()) == 0;
}

const RowArena::Entry* RowArena::EntryAtOrBefore(std::uint64_t offset) const {
  std::size_t low = 0;
  std::size_t high = entries_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (entries_[mid].entry.offset <= offset) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low == 0 ? nullptr : &entries_[low - 1];
}

const RowArena::Entry* RowArena::FindEntry(RowHandle handle) const {
  const Entry* candidate = EntryAtOrBefore(handle.offset);
  if (candidate == nullptr || !(candidate->entry == handle)) return nullptr;
  return candidate;
}

const RowArena::Entry* RowArena::FindRowOwner(RowHandle handle) const {
  const Entry* candidate = EntryAtOrBefore(handle.offset);
  if (candidate == nullptr) return nullptr;
  if (candidate->first_row == handle) return candidate;
  if (candidate->kind == EntryKind::kWitness &&
      candidate->second_row == handle) {
    return candidate;
  }
  return nullptr;
}

std::string_view RowArena::Span(RowHandle handle) const {
  return std::string_view(buffer_.data() + handle.offset,
                          static_cast<std::size_t>(handle.size));
}

AnalysisFactRange::Iterator::Iterator(const RowArena* arena, std::size_t index)
    : arena_(arena), index_(index) {
  Load();
}

void AnalysisFactRange::Iterator::Load() {
  if (arena_ == nullptr || index_ >= arena_->size()) return;  // end sentinel
  const auto handle = arena_->handle_at(index_);
  const auto row = arena_->fact_row_handle_at(index_);
  // The position is inside the arena, so both lookups succeed. A range built
  // over an arena that does not hold facts is the one way to reach this, and it
  // is a programming error rather than input.
  assert(handle.ok() && row.ok());
  if (handle.ok()) handle_ = *handle;
  if (row.ok()) row_ = *row;
}

AnalysisFact AnalysisFactRange::Iterator::operator*() const {
  if (arena_ == nullptr) return AnalysisFact{};
  auto decoded = arena_->DecodeFact(handle_);
  // The handle came from this arena's own index, so the bytes it names are ones
  // the arena wrote; a failure here is an internal invariant violation, not
  // input. The fallback is a default-valued fact, which is detectably wrong
  // downstream -- its id is in no batch id -- rather than a corrupt read.
  assert(decoded.ok());
  if (!decoded.ok()) return AnalysisFact{};
  return std::move(*decoded);
}

AnalysisFactRange::Iterator& AnalysisFactRange::Iterator::operator++() {
  ++index_;
  Load();
  return *this;
}

AnalysisFactRange::Iterator AnalysisFactRange::Iterator::operator++(int) {
  Iterator previous = *this;
  ++*this;
  return previous;
}

WitnessRange::Iterator::Iterator(const RowArena* arena, std::size_t index)
    : arena_(arena), index_(index) {
  Load();
}

void WitnessRange::Iterator::Load() {
  if (arena_ == nullptr || index_ >= arena_->size()) return;  // end sentinel
  const auto handle = arena_->handle_at(index_);
  const auto result = arena_->witness_result_row_handle_at(index_);
  const auto input = arena_->witness_input_row_handle_at(index_);
  assert(handle.ok() && result.ok() && input.ok());
  if (handle.ok()) handle_ = *handle;
  if (result.ok()) result_row_ = *result;
  if (input.ok()) input_row_ = *input;
}

WitnessEdge WitnessRange::Iterator::operator*() const {
  if (arena_ == nullptr) return WitnessEdge{};
  auto decoded = arena_->DecodeWitness(handle_);
  assert(decoded.ok());
  if (!decoded.ok()) return WitnessEdge{};
  return std::move(*decoded);
}

WitnessRange::Iterator& WitnessRange::Iterator::operator++() {
  ++index_;
  Load();
  return *this;
}

WitnessRange::Iterator WitnessRange::Iterator::operator++(int) {
  Iterator previous = *this;
  ++*this;
  return previous;
}

}  // namespace veritas::facts
