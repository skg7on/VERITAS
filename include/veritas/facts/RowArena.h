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

// RowArena.h — a compact, append-only store for semantic rows.
//
// A run holds a million facts and more than a million witness endpoints. A
// `SemanticRow` in the rich form is a heap vector of `std::variant` cells, each
// `StableId` cell owning a 64-character string, so the in-memory payload of a
// run is an order of magnitude larger than the same content encoded. An arena
// holds one contiguous buffer and describes a row as a byte range in it.
//
// Stable IDs are stored as their kind and 32 raw digest bytes, never as
// canonical text: the text form is rendered on demand by Decode and AppendKey,
// so a round trip never re-parses a digest.

#ifndef VERITAS_FACTS_ROW_ARENA_H_
#define VERITAS_FACTS_ROW_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {

// A stable handle into a RowArena. Never a pointer: the arena's buffer moves
// when it grows, and a handle must survive that.
struct RowHandle {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  bool operator==(const RowHandle&) const = default;
};

// The sub-ranges one appended fact occupies. `entry` spans the id and the row;
// `row` spans only the row, which is what a comparison addresses.
struct FactHandles {
  RowHandle entry;
  RowHandle row;
};

// The sub-ranges one appended witness occupies. A witness stores three rows'
// worth of content in one entry, so a reader that needs the result row or the
// input row addresses it directly rather than decoding the edge.
struct WitnessHandles {
  RowHandle entry;
  RowHandle result_row;
  RowHandle input_row;
};

class RowArena {
 public:
  // Appends one row and returns its handle. Fails with InvalidArgument when a
  // cell's stable ID carries a digest that is not 64 lowercase hexadecimal
  // characters.
  StatusOr<RowHandle> Append(const SemanticRow& row);

  // Materialises a row. Allocates, so callers decode one row at a time rather
  // than decoding a payload and keeping it. Fails with InvalidArgument when the
  // handle does not name a whole row inside this arena.
  StatusOr<SemanticRow> Decode(RowHandle handle) const;

  // Appends the row's canonical semantic key to `out`, in exactly the bytes
  // AppendSemanticKey produces for the rich row. The key is rendered from the
  // stored cells, so no row is decoded and no digest is re-parsed. Fails with
  // InvalidArgument, leaving `out` untouched, when the handle does not name a
  // whole row inside this arena.
  Status AppendKey(RowHandle handle, std::string* out) const;

  // True when both handles name rows in this arena with identical cells. False
  // when either handle is invalid, which is the answer an equality test can
  // give without a third state on its interface.
  bool RowEquals(RowHandle left, RowHandle right) const;

  // Payload-level forms. A fact entry stores its id and then its row; a witness
  // entry stores its result row, rule id, input row and ordinal. Each returns
  // the sub-range of every row it stored, because a reader that compares rows
  // must address a row and not the entry around it.
  StatusOr<FactHandles> AppendFact(const AnalysisFact& fact);
  StatusOr<WitnessHandles> AppendWitness(const WitnessEdge& edge);
  StatusOr<AnalysisFact> DecodeFact(RowHandle entry) const;
  StatusOr<WitnessEdge> DecodeWitness(RowHandle entry) const;

  // The entry handle of the index-th stored entry, so a caller can address an
  // entry without decoding it. Fails with InvalidArgument past the end.
  StatusOr<RowHandle> handle_at(std::size_t index) const;

  // Row sub-ranges by position, for the payload kinds whose entries hold more
  // than one row. A caller that compares rows uses these instead of decoding.
  // Each fails with InvalidArgument past the end or when the entry was appended
  // as a different payload kind.
  StatusOr<RowHandle> fact_row_handle_at(std::size_t index) const;
  StatusOr<RowHandle> witness_result_row_handle_at(std::size_t index) const;
  StatusOr<RowHandle> witness_input_row_handle_at(std::size_t index) const;

  // Compares two rows that may live in different arenas, which `RowEquals`
  // cannot: a batch compares a published fact's row against a witness's result
  // row, and those are stored in the facts arena and the witnesses arena
  // respectively. Valid because both arenas encode rows identically.
  static bool RowsEqual(const RowArena& left, RowHandle left_row,
                        const RowArena& right, RowHandle right_row);

  std::size_t size() const { return entries_.size(); }
  std::size_t bytes() const { return buffer_.size(); }

 private:
  // One record per appended entry. `kind` distinguishes a bare row from a fact
  // or witness payload, so a row-only accessor rejects the wrong entry kind
  // instead of reading bytes that mean something else.
  enum class EntryKind : std::uint8_t { kRow, kFact, kWitness };

  struct Entry {
    RowHandle entry;
    EntryKind kind = EntryKind::kRow;
    // The row itself; for a witness, its result row.
    RowHandle first_row;
    // The witness's input row. Unset for kRow and kFact.
    RowHandle second_row;
  };

  // The recorded entry that starts exactly at `handle`, or null. Entries are
  // appended in increasing offset order, so a midpoint search is exact; a
  // comparison that scanned them instead would be quadratic in a batch that
  // compares a row per fact.
  const Entry* FindEntry(RowHandle handle) const;

  // The recorded entry that carries `handle` as one of its rows: a bare row's
  // whole entry, a fact's row, or a witness's result or input row. Null for
  // anything else, which is what makes a row-only accessor reject a handle that
  // names an entry boundary or a position inside a row.
  const Entry* FindRowOwner(RowHandle handle) const;

  // The last recorded entry that starts at or before `offset`, or null when
  // none does. The midpoint search both finders above are built on.
  const Entry* EntryAtOrBefore(std::uint64_t offset) const;

  // The bytes a handle names. Only ever called with a handle a lookup above
  // just validated.
  std::string_view Span(RowHandle handle) const;

  std::string buffer_;
  std::vector<Entry> entries_;
};

// A decoded view over an arena of facts. Yields one entry per step and
// allocates only the entry it is yielding, so a caller that streams does not
// materialise the payload.
//
// The iterator also exposes the entry handle and the row handle its entry
// carries, so a caller that needs only to identify a row -- validation compares
// stored bytes rather than decoded rows -- never decodes.
class AnalysisFactRange {
 public:
  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = AnalysisFact;
    using difference_type = std::ptrdiff_t;
    // The dereference yields the fact itself, so there is no `pointer` typedef
    // and no `operator->` to go with one.
    using reference = AnalysisFact;

    // The position `index` of `arena`; past the last entry it is the end
    // sentinel.
    Iterator(const RowArena* arena, std::size_t index);

    // The entry handle, so a caller that needs only identity addresses the
    // entry without decoding it.
    RowHandle handle() const { return handle_; }

    // The fact's row sub-range, which is what a cross-arena comparison
    // addresses.
    RowHandle row_handle() const { return row_; }

    // Decodes this entry. Allocates the fact it returns and nothing else.
    AnalysisFact operator*() const;

    Iterator& operator++();
    Iterator operator++(int);

    bool operator==(const Iterator& other) const {
      return arena_ == other.arena_ && index_ == other.index_;
    }

   private:
    // Reads the handles this position carries. A range is only ever built over
    // an arena of the payload kind it names, so a position that carries neither
    // is a programming error rather than caller input; a debug build reports it
    // and the handles stay zero.
    void Load();

    const RowArena* arena_ = nullptr;
    std::size_t index_ = 0;
    RowHandle handle_;
    RowHandle row_;
  };

  explicit AnalysisFactRange(const RowArena* arena) : arena_(arena) {}

  Iterator begin() const { return Iterator(arena_, 0); }
  Iterator end() const {
    return Iterator(arena_, arena_ == nullptr ? 0 : arena_->size());
  }
  std::size_t size() const { return arena_ == nullptr ? 0 : arena_->size(); }
  bool empty() const { return size() == 0; }

 private:
  const RowArena* arena_ = nullptr;
};

// The witness twin of `AnalysisFactRange`. A witness entry carries two rows, so
// the iterator exposes both: validation compares a published fact's row against
// whichever endpoint the edge names.
class WitnessRange {
 public:
  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = WitnessEdge;
    using difference_type = std::ptrdiff_t;
    using reference = WitnessEdge;

    Iterator(const RowArena* arena, std::size_t index);

    RowHandle handle() const { return handle_; }
    RowHandle result_row_handle() const { return result_row_; }
    RowHandle input_row_handle() const { return input_row_; }

    // Decodes this entry. Allocates the edge it returns and nothing else.
    WitnessEdge operator*() const;

    Iterator& operator++();
    Iterator operator++(int);

    bool operator==(const Iterator& other) const {
      return arena_ == other.arena_ && index_ == other.index_;
    }

   private:
    void Load();

    const RowArena* arena_ = nullptr;
    std::size_t index_ = 0;
    RowHandle handle_;
    RowHandle result_row_;
    RowHandle input_row_;
  };

  explicit WitnessRange(const RowArena* arena) : arena_(arena) {}

  Iterator begin() const { return Iterator(arena_, 0); }
  Iterator end() const {
    return Iterator(arena_, arena_ == nullptr ? 0 : arena_->size());
  }
  std::size_t size() const { return arena_ == nullptr ? 0 : arena_->size(); }
  bool empty() const { return size() == 0; }

 private:
  const RowArena* arena_ = nullptr;
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_ROW_ARENA_H_
