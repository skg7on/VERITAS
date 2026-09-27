# `veritas-build analyze` Round 4 Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hold the run's semantic payload in a compact arena instead of a rich
object graph per row, and key the remaining stages on dense identifiers, so the
terms that scale with a project's derived-fact count stop being ~12× larger than
the information they carry.

**Architecture:** One `RowArena` per payload owner encodes each semantic row as a
byte range in a contiguous buffer, with stable IDs carried as raw digest bytes
rather than 64-character text. `WpaComponentResult` and `AnalysisFactBatch` hold
arenas and expose decoded ranges; `ResultCanonicalizer`, the fact store, and
batch validation key their containers on interned dense identifiers or key
hashes rather than on ~250-byte encoded strings.

**Tech Stack:** C++20, LLVM/Clang libraries, compiled Soufflé, RocksDB, SQLite,
CMake/Ninja, GoogleTest. No RTTI, no exceptions.

**Spec:**
[`docs/specs/veritas-build-analyze-round4-performance-design-spec.md`](../specs/veritas-build-analyze-round4-performance-design-spec.md)

**Tracking:** [GitHub issue #133](https://github.com/skg7on/VERITAS/issues/133)

## Global Constraints

- Preserve the production Soufflé engine and all four WPA domains.
- Preserve reverse-topological SCC execution and one immutable logical input for
  every `(SccId, WpaComponentKind)` pair.
- Preserve byte-identical EDB rows, mappings, `LogicalInputHash`, `FixpointHash`,
  `ExternalHash`, fact identity, witness selection, canonical order, and
  `BatchId` for unchanged inputs.
- **Do not release, evict, or reload component payloads.** Round 3 section 7.7
  built that design, measured it (CPU +109.01 s, instructions +13.8%, peak RSS
  +0.30 GiB) and reverted it in `deaead8`. Every payload stays resident.
- **Do not remove or weaken any `AnalysisFactBus::Validate` check**, including
  the `DeriveBatchId` recomputation. Only the containers it consults change.
- The arena is an in-memory representation only. Do not change `relations.v2`,
  `summary.v2`, rule or model bundles, the published schema, `cells_hex`, or any
  on-disk cache object format.
- Use `veritas::Status` and `veritas::StatusOr<T>` for failures; never use
  exceptions, RTTI, `dynamic_cast`, or `typeid`.
- Every new or modified C++, header, and CMake file opens with the full
  Apache-2.0 header from `.claude/rules/license-header-policy.md`.
- Test binaries land in `build/bin/`. Run one case with
  `./build/bin/<Binary> --gtest_filter='<Suite>.<Case>'`, or the whole binary
  through `ctest --test-dir build --output-on-failure -R '<Suite>'`. There is
  **no** `veritas_unit_tests` target; the merged round-3 plan's commands name one
  and are wrong.
- Perform all edits, builds, tests, commits, pushes, and PR updates in the task
  worktree on `claude/analyze-payload-arena`.

## File Structure

| File | Responsibility in this change |
| --- | --- |
| `include/veritas/facts/RowArena.h` (new) | `RowHandle` and the `RowArena` interface. |
| `src/facts/RowArena.cpp` (new) | Compact cell encoding, decode, key rendering, row equality. |
| `src/facts/CMakeLists.txt` | Add `RowArena.cpp` to the `veritas::facts` target. |
| `tests/unit/facts/RowArenaTest.cpp` (new) | Round-trip, key equality, row equality, handle rejection. |
| `tests/unit/facts/CMakeLists.txt` | Register `RowArenaTest`. |
| `include/veritas/wpa/WpaComponent.h` | `WpaComponentResult` payload becomes a `RowArena`. |
| `src/wpa/WpaOrchestrator.cpp` | Retain arenas; decode only the support relations. |
| `src/wpa/WpaRunRepository.cpp` | Serialize/deserialize a component result from its arena. |
| `src/facts/ResultCanonicalizer.cpp` | Intern rows to dense ids; rank sort; worklist cost relaxation. |
| `include/veritas/facts/AnalysisFactBus.h` | Arena-backed batch with `FactRange`/`WitnessRange`. |
| `src/facts/AnalysisFactBus.cpp` | Assembly into arenas; handle-based validation; CSR dependencies. |
| `src/facts/FactStore.cpp` | Witness grouping and fact/root lookups on dense ids. |
| `tests/unit/facts/AnalysisFactBusTest.cpp` | Arena batch, unchanged batch id and ownership. |
| `tests/unit/facts/FactStoreTest.cpp` | Batch construction through the new builder API. |
| `tests/unit/facts/ProvenanceStoreTest.cpp` | Batch construction through the new builder API. |
| `tests/integration/facts/VeritasExplainTest.cpp` | Batch construction through the new builder API. |
| `tests/integration/wpa/WpaEndToEndTest.cpp` | Publication checks through the range accessors. |
| `docs/specs/veritas-build-analyze-performance-design-spec.md` | Restore the three sections `4493cc4` deleted. |

---

### Task 1: The row arena

**Files:**

- Create: `include/veritas/facts/RowArena.h`
- Create: `src/facts/RowArena.cpp`
- Create: `tests/unit/facts/RowArenaTest.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**

- Consumes: `facts::SemanticRow`, `facts::SemanticCellValue`, `RelationsV2()`,
  `core::StableId`, `core::HexToDigest`, `core::DigestToHex`, and the
  `SemanticKeyCodec` primitives `AppendKeyHeader` / `AppendField` / `KeyFieldTag`.
- Produces:
  - `facts::RowHandle` — `{ std::uint64_t offset; std::uint64_t size; }`.
  - `RowArena::Append(const SemanticRow&) -> StatusOr<RowHandle>`
  - `RowArena::Decode(RowHandle) const -> StatusOr<SemanticRow>`
  - `RowArena::AppendKey(RowHandle, std::string*) const -> Status`
  - `RowArena::RowEquals(RowHandle, RowHandle) const -> bool`
  - `RowArena::size() const -> std::size_t`, `bytes() const -> std::size_t`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/facts/RowArenaTest.cpp` with the license header, then:

```cpp
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RowArena.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {
namespace {

// One row per cell alternative the encoding must know, so a new alternative
// added without teaching the arena fails here rather than silently colliding
// two rows onto one key.
std::vector<SemanticRow> Corpus() {
  const auto fn = MakeStableId(::veritas::core::IdKind::kFunctionVariant,
                               std::span<const std::byte>{});
  const auto mem = MakeStableId(::veritas::core::IdKind::kMemoryRef,
                                std::span<const std::byte>{});
  std::vector<SemanticRow> rows;
  rows.push_back(SemanticRow{RelationId::kReachableCall, {fn, mem}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, std::string("a_symbol")}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, static_cast<std::int64_t>(-7)}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, static_cast<std::uint64_t>(7)}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::DispatchKind::kDirect}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::AliasKind::kMay}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::ByteRangeKind::kBounded}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::EpistemicState::kMust}});
  return rows;
}

TEST(RowArenaTest, DecodeRoundTripsEveryCellAlternative) {
  RowArena arena;
  for (const SemanticRow& row : Corpus()) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok()) << handle.status().message();
    auto decoded = arena.Decode(*handle);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(*decoded, row);
  }
  EXPECT_EQ(arena.size(), Corpus().size());
}

TEST(RowArenaTest, AppendKeyMatchesAppendSemanticKey) {
  RowArena arena;
  for (const SemanticRow& row : Corpus()) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok()) << handle.status().message();
    std::string from_arena;
    ASSERT_TRUE(arena.AppendKey(*handle, &from_arena).ok());
    EXPECT_EQ(from_arena, EncodeSemanticKey(row));
  }
}

TEST(RowArenaTest, RowEqualsMatchesSemanticRowEquality) {
  RowArena arena;
  const auto rows = Corpus();
  std::vector<RowHandle> handles;
  for (const SemanticRow& row : rows) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok());
    handles.push_back(*handle);
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    for (std::size_t j = 0; j < rows.size(); ++j) {
      EXPECT_EQ(arena.RowEquals(handles[i], handles[j]), rows[i] == rows[j])
          << i << " vs " << j;
    }
  }
}

TEST(RowArenaTest, RejectsAHandleOutsideTheArena) {
  RowArena arena;
  auto handle = arena.Append(Corpus().front());
  ASSERT_TRUE(handle.ok());
  const RowHandle past_end{arena.bytes() + 8, 4};
  EXPECT_FALSE(arena.Decode(past_end).ok());
  EXPECT_FALSE(arena.AppendKey(past_end, nullptr).ok());
  EXPECT_FALSE(arena.RowEquals(*handle, past_end));
}

TEST(RowArenaTest, HandlesSurviveReallocation) {
  RowArena arena;
  auto first = arena.Append(Corpus().front());
  ASSERT_TRUE(first.ok());
  for (int i = 0; i < 4096; ++i) {
    ASSERT_TRUE(arena.Append(Corpus()[std::size_t(i) % Corpus().size()]).ok());
  }
  auto decoded = arena.Decode(*first);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, Corpus().front());
}

}  // namespace
}  // namespace veritas::facts
```

- [ ] **Step 2: Register the test and verify it fails to build**

Append to `tests/unit/facts/CMakeLists.txt`, matching the file's existing style:

```cmake
add_executable(RowArenaTest RowArenaTest.cpp)
target_link_libraries(RowArenaTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(RowArenaTest)
gtest_discover_tests(RowArenaTest DISCOVERY_TIMEOUT 60)
```

Run:

```bash
cmake --build --preset default --target RowArenaTest
```

Expected: compilation fails — `veritas/facts/RowArena.h` does not exist.

- [ ] **Step 3: Write the arena header**

Create `include/veritas/facts/RowArena.h` with the license header and:

```cpp
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
#include <string>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::facts {

// A stable handle into a RowArena. Never a pointer: the arena's buffer moves
// when it grows, and a handle must survive that.
struct RowHandle {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
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
  // stored cells, so no row is decoded and no digest is re-parsed.
  Status AppendKey(RowHandle handle, std::string* out) const;

  // True when both handles name rows in this arena with identical cells. False
  // when either handle is invalid, which is the answer an equality test can
  // give without a third state on its interface.
  bool RowEquals(RowHandle left, RowHandle right) const;

  std::size_t size() const { return handles_.size(); }
  std::size_t bytes() const { return buffer_.size(); }

 private:
  std::string buffer_;
  std::vector<RowHandle> handles_;
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_ROW_ARENA_H_
```

Add `#include <vector>` to the header's includes.

- [ ] **Step 4: Write the arena implementation**

Create `src/facts/RowArena.cpp` with the license header. The encoding is private
to this file; the tag order mirrors the `SemanticCellValue` alternative order.

```cpp
#include "veritas/facts/RowArena.h"

#include <array>
#include <cstring>
#include <variant>

#include "veritas/core/Hash.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/facts/SemanticKeyCodec.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {
namespace {

// Cell tags. A new alternative in SemanticCellValue must be added here and in
// both switches below; Witness.cpp's kUnencodedCellKind guard covers the key
// encoder, and the static_assert in AppendCell covers this one.
constexpr std::uint8_t kId = 0;
constexpr std::uint8_t kSigned = 1;
constexpr std::uint8_t kUnsigned = 2;
constexpr std::uint8_t kSymbol = 3;
constexpr std::uint8_t kDispatch = 4;
constexpr std::uint8_t kAlias = 5;
constexpr std::uint8_t kByteRange = 6;
constexpr std::uint8_t kEpistemic = 7;

template <typename T>
[[maybe_unused]] inline constexpr bool kUnencodedArenaCell = false;

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

// Decodes the canonical 64-character digest into bytes once, at append time.
Status AppendId(std::string* out, const core::StableId& id) {
  if (id.digest_hex.size() != kSHA256DigestBytes * 2) {
    return Status::InvalidArgument("stable id digest has the wrong length");
  }
  out->push_back(static_cast<char>(id.kind));
  for (std::size_t i = 0; i < kSHA256DigestBytes; ++i) {
    auto high = Nibble(id.digest_hex[2 * i]);
    if (!high.ok()) return high.status();
    auto low = Nibble(id.digest_hex[2 * i + 1]);
    if (!low.ok()) return low.status();
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
          out->push_back(static_cast<char>(kId));
          status = AppendId(out, value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          out->push_back(static_cast<char>(kSigned));
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          out->push_back(static_cast<char>(kUnsigned));
          PutU64(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          out->push_back(static_cast<char>(kSymbol));
          PutBytes(out, value);
        } else if constexpr (std::is_same_v<T, analysis::semantic::DispatchKind>) {
          out->push_back(static_cast<char>(kDispatch));
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, analysis::semantic::AliasKind>) {
          out->push_back(static_cast<char>(kAlias));
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, analysis::semantic::ByteRangeKind>) {
          out->push_back(static_cast<char>(kByteRange));
          PutU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, analysis::semantic::EpistemicState>) {
          out->push_back(static_cast<char>(kEpistemic));
          PutU64(out, static_cast<std::uint64_t>(value));
        } else {
          static_assert(kUnencodedArenaCell<T>,
                        "SemanticCellValue gained an alternative the arena "
                        "encoding does not know");
        }
      },
      cell);
  return status;
}

}  // namespace
```

Then the public methods. `Append` writes the relation, the cell count, and the
cells, then records the handle:

```cpp
StatusOr<RowHandle> RowArena::Append(const SemanticRow& row) {
  const std::size_t start = buffer_.size();
  PutU32(&buffer_, static_cast<std::uint32_t>(row.relation));
  PutU32(&buffer_, static_cast<std::uint32_t>(row.cells.size()));
  for (const SemanticCellValue& cell : row.cells) {
    Status appended = AppendCell(&buffer_, cell);
    if (!appended.ok()) {
      buffer_.resize(start);  // the append is all-or-nothing
      return appended;
    }
  }
  RowHandle handle{start, buffer_.size() - start};
  handles_.push_back(handle);
  return handle;
}
```

`Decode` range-checks the handle against the buffer and a recorded handle, then
reads relation, count and cells into a `SemanticRow`. `AppendKey` walks the same
bytes and appends through `AppendKeyHeader` / `AppendField` with
`KeyFieldTag::kId` for `kId`, `kSymbol` for `kSymbol`, `kNumber` for `kSigned`,
`kUnsigned` for `kUnsigned`, and `kEnum` for the four enum tags. `RowEquals`
compares the two byte ranges with `std::memcmp` after confirming every handle is
one of `handles_`.

Note for the implementer: `kSigned` and `kUnsigned` render through
`std::to_string` into a temporary for `AppendField`, and `kId` renders through
`core::DigestToHex` over the 32 stored bytes — no digest is ever parsed.

- [ ] **Step 5: Add the source to the library**

Add `RowArena.cpp` to the source list in `src/facts/CMakeLists.txt`, keeping the
file's existing ordering and formatting.

- [ ] **Step 6: Run the tests and verify they pass**

```bash
cmake --build --preset default --target RowArenaTest
./build/bin/RowArenaTest
```

Expected: 5 tests pass. Then confirm the encoders the arena shares still pass,
because `RowArena.cpp` includes `SemanticKeyCodec.h` and `Witness.cpp`'s key
encoder is the oracle `AppendKey` is tested against:

```bash
ctest --test-dir build --output-on-failure \
  -R 'AnalysisFactTest|WitnessTest|RelationSchemaTest|SemanticKeyCodecTest'
```

- [ ] **Step 7: Commit**

```bash
git add include/veritas/facts/RowArena.h src/facts/RowArena.cpp \
  src/facts/CMakeLists.txt tests/unit/facts/RowArenaTest.cpp \
  tests/unit/facts/CMakeLists.txt
git commit -m "perf(facts): add a compact arena for semantic rows"
```

---

### Task 2: Component results hold arenas

**Files:**

- Modify: `include/veritas/wpa/WpaComponent.h:98-107`
- Modify: `src/wpa/WpaOrchestrator.cpp:47-89, 326-360`
- Modify: `src/wpa/WpaRunRepository.cpp:110-130, 311-390, 589-620`
- Modify: `src/facts/ResultCanonicalizer.cpp` (return path only)
- Verify: `tests/unit/wpa/WpaOrchestratorTest.cpp`
- Verify: `tests/integration/wpa/WpaEndToEndTest.cpp`

**Interfaces:**

- Consumes: `RowArena`, `RowHandle` from Task 1.
- Produces: `WpaComponentResult` with `RowArena facts; RowArena witnesses;`
  in place of the two `std::vector` payload fields, and a
  `WpaComponentPayloadRange` helper used by both the repository and the
  orchestrator. `SerializeResult` and `DeserializeResult` keep their external
  contract; only their access to the payload changes.

- [ ] **Step 1: Add a failing equivalence test**

In `tests/unit/wpa/WpaOrchestratorTest.cpp`, add a case asserting that a
component's `facts` and `witnesses` round-trip through the arena and that
`result.facts` is no longer a vector:

```cpp
TEST(WpaOrchestratorTest, ComponentResultPayloadRoundTripsThroughTheArena) {
  auto run = SingleComponentRun();  // the fixture this file already builds
  const auto& completion = run.completed_components.front();

  std::vector<AnalysisFact> decoded;
  for (const AnalysisFact& fact : completion.result.facts) {
    decoded.push_back(fact);
  }
  ASSERT_FALSE(decoded.empty());
  // The identity is the one the existing single-component case asserts on;
  // reuse its expectation so the two cases cannot disagree about the fixture.
  EXPECT_EQ(decoded.front().fact_id, ExpectedSingleComponentFactId());

  std::vector<WitnessEdge> edges;
  for (const WitnessEdge& edge : completion.result.witnesses) {
    edges.push_back(edge);
  }
  EXPECT_EQ(edges.size(), completion.result.witnesses.size());
}
```

`SingleComponentRun()` and `ExpectedSingleComponentFactId()` are named here to
say what the fixture must supply. If this file already factors its setup, use
that helper's name instead and keep one fixture rather than adding a second; if
it does not, extract it from the existing case in this same step. Do not invent
a second fixture that could drift from the first.

- [ ] **Step 2: Run it and verify it fails to build**

```bash
cmake --build --preset default --target WpaOrchestratorTest
```

Expected: compilation fails — `WpaComponentResult::facts` is still a vector with
no `begin()`/`end()` iterator pair of the required shape.

- [ ] **Step 3: Change `WpaComponentResult`**

In `include/veritas/wpa/WpaComponent.h`, replace the two payload members and add
the range type:

```cpp
// A decoded view over an arena-backed payload. Yields one row per step and
// allocates only the row it is yielding, so a caller that streams does not
// materialise the payload.
class AnalysisFactRange {
 public:
  class Iterator;
  AnalysisFactRange(const RowArena* arena);
  Iterator begin() const;
  Iterator end() const;
  std::size_t size() const;
  bool empty() const;
};

struct WpaComponentResult {
  core::StableId scc_id;
  WpaComponentKind component = WpaComponentKind::kReachability;
  std::string logical_input_hash;
  std::string fixpoint_hash;
  std::string external_hash;
  // The payload is held compactly; iterate it rather than copying it.
  RowArena facts;
  RowArena witnesses;
  std::vector<std::string> diagnostics;
};
```

`WitnessRange` is the same shape over `WitnessEdge`; define both in `RowArena.h`
so Task 3's batch reuses them rather than declaring a second pair.

- [ ] **Step 4: Update the canonicalizer's return path**

`CanonicalizedResult` keeps its rich vectors — it is one component's result,
transient, and the arena is built where ownership is taken. In
`WpaOrchestrator.cpp`'s `MakeResult`, append into arenas instead of assigning
vectors, and return a `StatusOr<WpaComponentResult>` so an append failure
propagates:

```cpp
StatusOr<WpaComponentResult> MakeResult(const WpaLogicalComponentInput& logical,
                                        const facts::CanonicalizedResult& canonical) {
  WpaComponentResult result;
  result.scc_id = logical.scc_id;
  result.component = logical.component;
  result.logical_input_hash = logical.logical_input_hash;
  result.fixpoint_hash = canonical.fixpoint_hash;
  result.external_hash = canonical.external_hash;
  for (const auto& fact : canonical.facts) {
    auto appended = result.facts.Append(fact.row);
    if (!appended.ok()) return appended.status();
  }
  for (const auto& edge : canonical.witnesses) {
    // A witness edge is three rows in one entry: the result row, the input
    // row, and the rule and ordinal between them.
    auto appended = result.witnesses.AppendWitness(edge);
    if (!appended.ok()) return appended.status();
  }
  result.diagnostics = canonical.diagnostics;
  return result;
}
```

Add `RowArena::AppendWitness(const WitnessEdge&) -> Status` and
`AppendFact(const AnalysisFact&) -> Status` to Task 1's arena so both payloads
encode ids alongside rows; `AppendFact` writes the fact id then the row,
`AppendWitness` writes the result row, the rule id, the input row, and the
ordinal. Decoding is symmetric (`DecodeFact`, `DecodeWitness`).

- [ ] **Step 5: Update `SuccessorSupport` and the repository**

`SuccessorSupport` iterates the successor's arena and materializes only rows
whose relation is in the expected derived set:

```cpp
for (const auto& fact : completed[it->second].result.facts) {
  if (expected.contains(fact.row.relation)) {
    support.push_back(fact);
  }
}
```

becomes the same loop over the range, which yields by value and allocates one
row at a time. `SerializeResult` walks the range instead of the vector;
`DeserializeResult` appends into the arena. Neither changes its bytes.

Update `LoadReusableComponent`'s revalidation to iterate the range.

- [ ] **Step 6: Run the WPA and repository tests**

```bash
cmake --build --preset default --target WpaOrchestratorTest WpaEndToEndTest \
  WpaInputMaterializerTest WpaRunRepositoryTest
ctest --test-dir build --output-on-failure \
  -R 'WpaOrchestratorTest|WpaEndToEndTest|WpaInputMaterializerTest|WpaRunRepositoryTest'
```

Expected: all pass. The cached-result path is the risk here: a
`LoadReusableComponent` failure surfaces as a hard cache-integrity error, so a
failure in `WpaRunRepositoryTest` means the round trip changed.

- [ ] **Step 7: Commit**

```bash
git add include/veritas/wpa/WpaComponent.h src/wpa/WpaOrchestrator.cpp \
  src/wpa/WpaRunRepository.cpp include/veritas/facts/RowArena.h \
  src/facts/RowArena.cpp tests/unit/wpa/WpaOrchestratorTest.cpp
git commit -m "perf(wpa): hold component payloads in a compact arena"
```

---

### Task 3: The batch holds arenas

**Files:**

- Modify: `include/veritas/facts/AnalysisFactBus.h:46-74`
- Modify: `src/facts/AnalysisFactBus.cpp:274-416`
- Modify: `src/analysis/ProjectAnalyzer.cpp:346-358`
- Modify: `tests/unit/facts/AnalysisFactBusTest.cpp:341-342, 462-480, 660-670`
- Modify: `tests/unit/facts/FactStoreTest.cpp:111-150`
- Modify: `tests/unit/facts/ProvenanceStoreTest.cpp:108-109`
- Modify: `tests/integration/facts/VeritasExplainTest.cpp:159-160`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp:232-233`

**Interfaces:**

- Consumes: `RowArena`, `AnalysisFactRange`, `WitnessRange` from Tasks 1-2.
- Produces: `AnalysisFactBatch::facts()` and `witnesses()` returning ranges,
  `fact_count()` / `witness_count()`, and builders `SetFacts(std::vector<AnalysisFact>)`
  and `SetWitnesses(std::vector<WitnessEdge>)` for tests and small callers.

- [ ] **Step 1: Add a failing test that pins the batch id**

In `tests/unit/facts/AnalysisFactBusTest.cpp`, add:

```cpp
TEST(AnalysisFactBusTest, ArenaBatchKeepsTheCanonicalBatchIdAndOwnership) {
  auto run = DuplicateProofRun();
  const auto expected = MakeAnalysisFactBatch(run);
  auto consumed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(consumed.batch_id, expected.batch_id);
  ASSERT_EQ(consumed.fact_count(), expected.fact_count());
  ASSERT_EQ(consumed.witness_count(), expected.witness_count());
  std::size_t i = 0;
  for (const auto& fact : consumed.facts()) {
    EXPECT_EQ(fact.fact_id, expected.facts[i].fact_id);
    ++i;
  }
  for (const auto& completion : consumed.completed_components) {
    EXPECT_TRUE(completion.result.facts.empty());
    EXPECT_TRUE(completion.result.witnesses.empty());
    EXPECT_FALSE(completion.result.logical_input_hash.empty());
  }
}
```

- [ ] **Step 2: Run it and verify it fails to build**

```bash
cmake --build --preset default --target AnalysisFactBusTest
```

Expected: compilation fails — `fact_count`, `facts()` and `witness_count` do not
exist.

- [ ] **Step 3: Change the batch**

In `include/veritas/facts/AnalysisFactBus.h`:

```cpp
struct AnalysisFactBatch {
  core::StableId batch_id;
  AnalysisRunManifest run;
  std::vector<wpa::WpaComponentKey> expected_components;
  std::vector<wpa::WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  std::vector<RootedInputFact> rooted_input_facts;
  std::vector<std::string> diagnostics;

  // Canonical facts and witnesses, held compactly. Iterate, do not copy.
  AnalysisFactRange facts() const { return AnalysisFactRange(&facts_); }
  WitnessRange witnesses() const { return WitnessRange(&witnesses_); }
  std::size_t fact_count() const { return facts_.size(); }
  std::size_t witness_count() const { return witnesses_.size(); }

  // Builder side. The arena is append-only, so these replace the whole payload.
  void SetFacts(const std::vector<AnalysisFact>& facts);
  void SetWitnesses(const std::vector<WitnessEdge>& witnesses);
  void ClearPayload();

 private:
  RowArena facts_;
  RowArena witnesses_;
};
```

`DeriveBatchId` iterates the ranges and produces the same bytes it does today:
it calls `AppendSemanticKey` on the rich row of each entry, so the range must
expose the whole `AnalysisFact`/`WitnessEdge`, not only the row. If iteration
cost here is a concern, `DeriveBatchId` may instead call `AppendKey` on the
arena handle; the two produce identical bytes (Task 1 Step 1) and the batch id
is the proof.

- [ ] **Step 4: Rewrite assembly to append into the arenas**

`MakeAnalysisFactBatch` keeps its structure — sort completions by key, intern
ranks, select the canonical owner, sort by rank, collapse duplicates — and
replaces the four `batch.facts`/`batch.witnesses` container operations with
arena appends in the same order. The two `std::ranges::unique` passes become a
single pass that skips an entry whose rank equals the previous entry's, because
an arena cannot erase.

Note: the existing duplicate collapse relies on equal entries being adjacent
after the rank sort, which stays true; only the mechanism changes.

- [ ] **Step 5: Update every call site**

```bash
cmake --build --preset default 2>&1 | grep -E 'error:' | head -40
```

Fix each site the compiler reports. The mechanical translations are:

- `batch.facts = {a, b, c};` → `batch.SetFacts({a, b, c});`
- `batch.witnesses = {...};` → `batch.SetWitnesses({...});`
- `batch.facts.size()` → `batch.fact_count()`
- `batch.facts[i]` → iterate with an index counter, or compare through the arena
  as Task 4 does
- `batch.facts.empty()` → `batch.fact_count() == 0`
- `for (const auto& f : batch.facts)` → `for (const auto& f : batch.facts())`

One site is not mechanical. `ValidateStillRejectsEachTamperedIdentity`
(`tests/unit/facts/AnalysisFactBusTest.cpp:658`) mutates rows in place —
`bad_fact.facts[0].fact_id = *zero_fact` and
`bad_result.witnesses[0].result.row.cells[0] = *other_function` — then
recomputes the batch id. An arena is append-only, so a row cannot be mutated
after it is stored. Rewrite each tamper case to rebuild the batch from the
original rows with the one row replaced:

```cpp
std::vector<AnalysisFact> rows;
for (const AnalysisFact& fact : batch.facts()) {
  rows.push_back(fact);
}
rows[0].fact_id = *zero_fact;
AnalysisFactBatch bad_fact = batch;
bad_fact.SetFacts(rows);
bad_fact.batch_id = DeriveBatchId(bad_fact);
```

Keep every assertion and every expected message byte-identical: this test is a
security-relevant guard on batch integrity and the rewrite must not weaken it.
The batch id must still be recomputed after each rebuild, exactly as the comment
at the top of that case explains — a stale id would be rejected at the batch-id
gate before the check under test runs, and the case would pass vacuously.

- [ ] **Step 6: Run the batch, store, and explain tests**

```bash
cmake --build --preset default --target AnalysisFactBusTest FactStoreTest \
  ProvenanceStoreTest VeritasExplainTest WpaEndToEndTest ProjectAnalyzerWpaTest
ctest --test-dir build --output-on-failure \
  -R 'AnalysisFactBusTest|FactStoreTest|ProvenanceStoreTest|VeritasExplainTest|WpaEndToEndTest|ProjectAnalyzerWpaTest'
```

Expected: all pass, and `AnalysisFactBusTest.ArenaBatchKeepsTheCanonicalBatchIdAndOwnership`
confirms the batch id did not move.

- [ ] **Step 7: Commit**

```bash
git add include/veritas/facts/AnalysisFactBus.h src/facts/AnalysisFactBus.cpp \
  src/analysis/ProjectAnalyzer.cpp tests/unit/facts/AnalysisFactBusTest.cpp \
  tests/unit/facts/FactStoreTest.cpp tests/unit/facts/ProvenanceStoreTest.cpp \
  tests/integration/facts/VeritasExplainTest.cpp \
  tests/integration/wpa/WpaEndToEndTest.cpp
git commit -m "perf(facts): hold the assembled batch in arenas"
```

---

### Task 4: Validation reads handles, not decoded rows

**Files:**

- Modify: `src/facts/AnalysisFactBus.cpp:425-564`

**Interfaces:**

- Consumes: the arena-backed batch from Task 3, and `RowArena::RowEquals`.
- Produces: unchanged `Validate` semantics and messages; its fact index becomes a
  hash map keyed on a 64-bit key hash, and its dependencies become two flat
  arrays.

- [ ] **Step 1: Add a test that forces every key to collide**

Hashing the key instead of comparing it exactly trades a comparison for a
bucket, so the new failure mode is a collision resolved wrongly. Build the test
to force the worst case: a seam that makes every row hash to the same value, so
the index degenerates to one bucket and only the exact comparison can resolve a
lookup.

Give `Validate` one injectable seam in the style `RunMetrics` already uses for
its clocks — a `std::function<std::uint64_t(const SemanticRow&)>` defaulting to
the real key hash — and add to `AnalysisFactBusTest.cpp`:

```cpp
TEST(AnalysisFactBusTest, ValidateResolvesLookupsExactlyUnderATotalHashCollision) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  // Every row hashes alike, so the fact index holds one bucket and a lookup
  // can only be answered by comparing arena bytes. A hash-only index passes
  // the well-formed case below and then resolves the tampered one to the wrong
  // entry, which is the failure this test exists to catch.
  AnalysisFactBus bus(*repo);
  bus.SetKeyHashForTesting([](const SemanticRow&) { return 0u; });

  const AnalysisFactBatch batch = SuccessfulBatch();
  ASSERT_TRUE(bus.Publish(batch).ok());
}
```

Then extend the collision test to the tampered rows: for each mutation the
existing `ValidateStillRejectsEachTamperedIdentity` covers, rebuild the batch
with that row replaced and assert the same status code and the same message,
with the constant hash still installed. A tampered row that now *passes* is the
bug this test is for.

Note for the implementer: this file's existing tamper case mutates the batch in
place (`bad_fact.facts[0].fact_id = ...`). That cannot survive an append-only
arena, so the tamper cases must rebuild the batch instead — see Task 3 Step 5.

- [ ] **Step 2: Run it against the current implementation and verify it passes**

```bash
cmake --build --preset default --target AnalysisFactBusTest
./build/bin/AnalysisFactBusTest --gtest_filter='AnalysisFactBusTest.*Collision*'
```

Expected: PASS before the rewrite, because the current `std::map` compares keys
exactly and is unaffected by the hash. It must still pass in Step 6; that
invariance is what makes it a guard on the rewrite rather than on the old code.
If it fails before the rewrite, the seam is not wired and the test proves
nothing.

- [ ] **Step 3: Replace the fact index**

`std::map<core::StableId, std::size_t> fact_index` becomes
`std::unordered_map<std::uint64_t, std::vector<std::size_t>>` keyed on a 64-bit
hash of the row's canonical key, with the bucket resolved by exact arena byte
comparison. A bucket with more than one index means two distinct rows hashed
alike; the exact comparison picks the right one and a bucket that matches none
falls through to the current failure.

The 64-bit hash is the seam Step 1 installs, so it reads:

```cpp
// Defaults to the canonical key hash. A test injects a constant to force every
// row into one bucket, which is the only way to exercise the exact-comparison
// path from a unit test.
std::function<std::uint64_t(const SemanticRow&)> key_hash_ = DefaultKeyHash;
```

`SetKeyHashForTesting` assigns `key_hash_`. The member follows `SetMetrics`'s
precedent of a non-owning, control-flow-neutral seam; it is not a global and it
is not read anywhere decisions are made.

- [ ] **Step 4: Replace the dependency vectors**

```cpp
std::vector<std::vector<std::size_t>> dependencies(batch.fact_count());
```

becomes two flat arrays plus one offset table, built in the same pass that
already walks the endpoints:

```cpp
std::vector<std::size_t> depend_offset(batch.fact_count() + 1, 0);
std::vector<std::size_t> depend_target;
// Count, prefix-sum, then fill by replaying the endpoint pass.
```

The traversal in the current code becomes index arithmetic over the two arrays
and must produce the same `processed` count, hence the same cycle rejection.

- [ ] **Step 5: Compare rows through the arena**

The two row comparisons that today decode both sides become
`arena.RowEquals(handle_a, handle_b)` on the handles the fact index stored, so
no row is decoded during validation.

- [ ] **Step 6: Run the validation tests**

```bash
cmake --build --preset default --target AnalysisFactBusTest
ctest --test-dir build --output-on-failure -R 'AnalysisFactBusTest'
```

Expected: every case passes, including the tamper case from Step 1 and the
cycle-rejection case. A tamper case that now passes is the failure mode to watch
for: it means the exact comparison stopped firing.

- [ ] **Step 7: Commit**

```bash
git add src/facts/AnalysisFactBus.cpp tests/unit/facts/AnalysisFactBusTest.cpp
git commit -m "perf(facts): validate batches through arena handles"
```

---

### Task 5: The canonicalizer interns and stops encoding in comparators

**Files:**

- Modify: `src/facts/ResultCanonicalizer.cpp:60-100, 107-200, 203-330`
- Verify: `tests/unit/facts/ResultCanonicalizerTest.cpp`

**Interfaces:**

- Consumes: nothing from Tasks 1-4; this task is independent of the arena and
  can be reviewed on its own.
- Produces: unchanged `CanonicalizedResult` and unchanged failure messages.

- [ ] **Step 1: Extend the two existing cases that already pin this behaviour**

This file already has the guards; do not add parallel ones that could drift.
Two cases carry the behaviour the rewrite must preserve, and each gets one
addition:

- `ResultCanonicalizerTest.SelectsShortestProofDeterministically` (line 181)
  already pins proof selection. Add to it a result set whose encoded keys share
  a long prefix and whose cells include a numeric value and a symbol that sort
  differently as bytes than as values — for example a `kSigned` cell holding
  `-7` and a `kSymbol` cell holding `"-7"` — so a rank sort that ranked by
  anything but the encoded key would order them differently. Assert the same
  canonical order the existing expectations in that case use.
- `ResultCanonicalizerTest.RejectsCyclicUnrootedWitnesses` (line 162) already
  pins the cycle rejection. Extend its fixture with a longer acyclic chain
  rooted in a declared input — at least four derivations deep — so the worklist
  must propagate through several levels rather than converging in one pass.
  Assert the case still succeeds and that the selected proof is the shortest
  one, which is what a worklist that terminates early would get wrong.

Both extensions use the file's existing `Root`, `Edge`, `Derivation` and
`RequestFor` helpers; do not introduce new fixture construction.

- [ ] **Step 2: Run them and verify they pass before the change**

```bash
cmake --build --preset default --target ResultCanonicalizerTest
./build/bin/ResultCanonicalizerTest \
  --gtest_filter='ResultCanonicalizerTest.SelectsShortestProofDeterministically'
./build/bin/ResultCanonicalizerTest \
  --gtest_filter='ResultCanonicalizerTest.RejectsCyclicUnrootedWitnesses'
```

Expected: both PASS. They pin current behaviour, so the change must keep them
green; that invariance is what proves the rewrites below are refactorings.
Ordering and proof expectations the extensions need are read off the current
implementation's own output, not hand-derived.

- [ ] **Step 3: Intern rows to dense ids**

Add a private interner built once per `Canonicalize` call, mapping a row's
canonical key to a dense id in the same style as `FactRanks` in
`AnalysisFactBus.cpp:308`. Replace the three nested
`std::map<std::string, std::map<std::string, std::map<std::string, Derivation>>>`
levels with a flat vector of derivations keyed by `(result_id, rule_id,
derivation_id)`.

- [ ] **Step 4: Replace the comparator encoding**

The two sorts that call `EncodeSemanticKey` inside their comparators
(`ResultCanonicalizer.cpp:67-85` and `:302-316`) sort on the interned dense id,
which preserves encoded-key order by construction because ids are assigned in
sorted key order.

- [ ] **Step 5: Replace the cost sweep with a worklist**

The relaxation at `:211-245` sweeps every derivation for up to `results.size()`
rounds. Replace it with a worklist seeded from every derivation whose inputs are
all roots, re-enqueueing a derivation only when the cost of one of its inputs
decreases. Convergence, the `kUnproven` sentinel, and the resulting costs are
unchanged; Step 1's cycle test is the guard.

- [ ] **Step 6: Run the canonicalizer and conformance tests**

```bash
cmake --build --preset default --target ResultCanonicalizerTest \
  WpaExecutorConformanceTest AnalysisFactBusTest
ctest --test-dir build --output-on-failure \
  -R 'ResultCanonicalizerTest|WpaExecutorConformanceTest|AnalysisFactBusTest'
```

Expected: all pass. `WpaExecutorConformanceTest` is the cross-engine guard and
must execute rather than report `GTEST_SKIP`.

- [ ] **Step 7: Commit**

```bash
git add src/facts/ResultCanonicalizer.cpp \
  tests/unit/facts/ResultCanonicalizerTest.cpp
git commit -m "perf(facts): intern canonicalizer rows and relax costs by worklist"
```

---

### Task 6: The fact store groups by dense rank

**Files:**

- Modify: `src/facts/FactStore.cpp:175-378`
- Verify: `tests/unit/facts/FactStoreTest.cpp`
- Verify: `tests/unit/facts/ProvenanceStoreTest.cpp`
- Verify: `tests/integration/facts/VeritasExplainTest.cpp`

**Interfaces:**

- Consumes: the arena-backed batch from Task 3.
- Produces: unchanged published rows and unchanged failure statuses.

- [ ] **Step 1: Add a failing test for the witness grouping**

```cpp
TEST(FactStoreTest, WitnessGroupingIsKeyedByRankNotByEncodedKey) {
  // Two results whose encoded keys share a long prefix, each with several
  // derivations. Publish and assert that each result's provenance node carries
  // its own edges, which is what a grouping bug would merge.
}
```

- [ ] **Step 2: Run it and verify it passes before the change**

```bash
cmake --build --preset default --target FactStoreTest
./build/bin/FactStoreTest --gtest_filter='FactStoreTest.*Grouping*'
```

Expected: PASS. This is the behaviour the rewrite must preserve.

- [ ] **Step 3: Replace the grouping maps**

`std::map<std::string, ResultWitness> result_witnesses` keyed by the encoded
result key becomes a vector of groups indexed by an interned result rank, in the
same style as the assembly ranks. `std::map<core::StableId, std::string>
witness_id_by_fact` and `std::map<core::StableId, const RootedInputFact*>
root_evidence` become hash maps keyed on the stable id's digest bytes.
`std::set<core::StableId> fact_ids` becomes a hash set.

The sort of each group's edges by `input_ordinal` stays, and
`DeriveWitnessId` — which hashes the ordered edges — is unchanged, so the
witness ids, the `selected_witness_id` column, and every published row are
unchanged.

- [ ] **Step 4: Run the store and explain tests**

```bash
cmake --build --preset default --target FactStoreTest ProvenanceStoreTest \
  VeritasExplainTest ProjectAnalyzerWpaTest
ctest --test-dir build --output-on-failure \
  -R 'FactStoreTest|ProvenanceStoreTest|VeritasExplainTest|ProjectAnalyzerWpaTest'
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/facts/FactStore.cpp tests/unit/facts/FactStoreTest.cpp
git commit -m "perf(facts): group published witnesses by interned rank"
```

---

### Task 7: Prove equivalence and measure acceptance

**Files:**

- Modify: `docs/specs/veritas-build-analyze-performance-design-spec.md`
- Modify: `docs/specs/veritas-build-analyze-round4-performance-design-spec.md`
- Verify: every file changed in Tasks 1-6

**Interfaces:**

- Consumes: a clean build of the round-4 tree and the LevelDB checkout at
  `/Users/skg7on/Workspace/Projects/leveldb`.
- Produces: the equivalence evidence and the per-term measurements section 9.4
  of the spec requires.

- [ ] **Step 1: Run the targeted semantic suite**

```bash
cmake --build --preset default
ctest --test-dir build --output-on-failure \
  -R 'RowArenaTest|AnalysisFactBusTest|ResultCanonicalizerTest|FactStoreTest|ProvenanceStoreTest|VeritasExplainTest|WpaOrchestratorTest|WpaEndToEndTest|WpaInputMaterializerTest|WpaRunRepositoryTest|WpaExecutorConformanceTest|ProjectAnalyzerWpaTest'
```

Expected: every selected test passes and the conformance test executes rather
than reporting `GTEST_SKIP`.

- [ ] **Step 2: Clean full build and full suite**

```bash
rm -rf build
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
ctest --test-dir build --output-on-failure
```

Expected: configuration and build exit 0; CTest reports no failures, crashes,
timeouts, skips, or not-run tests. Round 3's bar was 811 of 811; record this
run's count.

- [ ] **Step 3: Run the qualification gates**

```bash
python3 tests/qualification/M9EntryGateTest.py
ctest --test-dir build -L wpa-qualification --no-tests=error \
  --output-on-failure --output-junit wpa-qualification.xml
python3 tests/qualification/check_no_skips.py \
  build/wpa-qualification.xml \
  --expect WpaDifferentialQualificationTest,WpaDeterminismQualificationTest,WpaFailureQualificationTest,WpaMigrationQualificationTest,WpaPerformanceQualificationTest
python3 tools/check_m9_entry.py --build-dir build
```

Expected: the gate unit tests pass, all five qualification aggregates run
without missing, disabled, or skipped tests, and all ten M9 criteria pass.

- [ ] **Step 4: Prove published content is unchanged**

Build the pre-change revision and this revision in one build tree, run each
against a fresh output directory, and compare with the store instrument:

```bash
# pre-change: check out the branch point, rebuild, run, dump
./build/bin/veritas-store-diff <pre>/metadata.db <post>/metadata.db
```

Run the comparison in the form round 3's spec section 9.1 defines, including the
per-component `input_hash` / `fixpoint_hash` / `externally_visible_hash`
comparison across all 13,716 components, and both dump orderings. Expected:
equality. A difference here fails the round regardless of the measurements.

- [ ] **Step 5: Measure acceptance as a controlled pair**

Reuse the phase recorder and the same protocol for both revisions:

```bash
OUT=/tmp/veritas-round4-<rev>  # a fresh, non-existent directory per revision
test ! -e "$OUT" && mkdir "$OUT"
/usr/bin/time -lp ./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output "$OUT" --metrics true --metrics-interval-ms 250 \
  --metrics-top-n 15 --metrics-series true
```

Collect, for both revisions: worst-of-three CPU (user + sys); instructions
retired where the platform counter is available; worst-of-three peak RSS and
peak physical footprint; and the per-span `rss_start` / `rss_end` deltas for
`wpa.orchestrate`, `facts.batch_assemble` and `facts.publish`.

Expected per section 9.4: the payload terms move by roughly the measured
representation gap and CPU falls. Record every term, including any that did not
move, as not having moved.

- [ ] **Step 6: Restore the three deleted spec sections**

`4493cc4` deleted `## 10. Risks and Mitigations`, `## 11. Rejected Alternatives`
and `## 12. Completion Criteria` from
`docs/specs/veritas-build-analyze-performance-design-spec.md`, leaving its
section 9.5.4 citing a section that does not exist. Restore all three verbatim:

```bash
git show e22f042:docs/specs/veritas-build-analyze-performance-design-spec.md | \
  sed -n '/^## 10\. Risks and Mitigations/,$p'
```

Append that text at the end of the current file. Then confirm the citation
resolves:

```bash
grep -n "^## 11\|^### 11.3" docs/specs/veritas-build-analyze-performance-design-spec.md
```

Expected: both match.

- [ ] **Step 7: Record the results in the round-4 spec and commit**

Add a "Measured outcome" section to the round-4 spec with the pair, the
equivalence result, and each term. Then:

```bash
git add docs/specs/veritas-build-analyze-performance-design-spec.md \
  docs/specs/veritas-build-analyze-round4-performance-design-spec.md
git commit -m "docs(perf): record round 4 equivalence and acceptance measurements"
```

- [ ] **Step 8: Repository hygiene**

```bash
git diff --check
git status --porcelain
git diff --stat main...HEAD
```

Run the license-header verification snippet from
`.claude/rules/license-header-policy.md`. Review `git diff main...HEAD` and
confirm it contains only this change, its tests, and its documentation.

---

## Plan Self-Review Record

**Spec coverage.** Task 1 implements §7.1 (`RowArena`). Task 2 implements §7.2
(payload retention). Task 3 implements §7.3 (batch assembly). Task 4, 5 and 6
implement §7.4 (id-keyed downstream: validation, canonicalizer, fact store).
Task 7 implements §9.1, §9.4 and §13 (equivalence instrument, acceptance
measurement, documentation repair). §7.5 (data flow) is the union of Tasks 2 and
3. §5's prohibition on releasing payloads is carried as a Global Constraint and
is what Task 2 deliberately does *not* do.

**Completeness scan.** Every code-changing step names its file, the symbol it
changes, and the shape of the result. Two steps tell the implementer to capture
expected values from the current implementation before rewriting it (Task 5
Steps 1-2, Task 4 Step 2); that is deliberate — the expected values are the
current behaviour, and deriving them by hand would be a guess.

**Type consistency.** `RowHandle`, `RowArena`, `AnalysisFactRange`,
`WitnessRange`, `Append`/`Decode`/`AppendKey`/`RowEquals`/`AppendFact`/
`DecodeFact`/`AppendWitness`/`DecodeWitness`, `fact_count()`, `witness_count()`,
`facts()`, `witnesses()`, `SetFacts`, `SetWitnesses` are named identically
wherever a later task uses them.

**Known risk to the reviewer.** Task 3 is the widest change: it touches the M9
handoff type and every construction site in tests. It is deliberately one task
because the batch type is one deliverable and a partial conversion does not
compile.

**Two sites are not mechanical, and both are integrity-relevant.** They are
called out in the tasks rather than left to the implementer to discover:

1. `ValidateStillRejectsEachTamperedIdentity` mutates rows in place. An
   append-only arena cannot support that, so every tamper case must rebuild the
   batch — and must still recompute the batch id after each rebuild, or the case
   passes vacuously at the batch-id gate. Task 3 Step 5 gives the shape.
2. Task 4 replaces exact key comparison with a hash bucket. Its Step 1-2 pair
   installs a constant-hash seam so every row collides, and asserts the tamper
   cases still fail with the same messages; that test passes before the rewrite
   and must pass after, which is what makes it a guard on the rewrite rather
   than on the old code.

**Verified against the tree, not assumed.** Every test path this plan names was
checked to exist, and every case it extends was checked to exist by name and
line: `ValidateStillRejectsEachTamperedIdentity` (658),
`SelectsShortestProofDeterministically` (181),
`RejectsCyclicUnrootedWitnesses` (162), and the `Root` / `Edge` / `Derivation` /
`RequestFor` helpers in `ResultCanonicalizerTest.cpp`. Two paths and one test
name in an earlier draft of this plan were wrong and were fixed here. The
round-3 plan's `veritas_unit_tests` target does **not** exist anywhere in the
build; that plan's test commands are unrunnable, and this one uses
`build/bin/<Binary>` and `ctest -R` instead.

**Not in this plan.** Streaming publication, the post-SVF floor, and Soufflé
program reuse (spec §10) are successor stages. The arena's own cell encoding is
not shared with the on-disk cache object format, which spec §5 excludes.
