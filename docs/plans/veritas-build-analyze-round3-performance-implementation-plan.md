# `veritas-build analyze` Round 3 Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the measured non-semantic costs from `veritas-build analyze` so
the LevelDB command completes within 375 s and 4 GiB peak RSS, without changing
one published row.

**Architecture:** Three layers change. The WPA execution boundary loses its
per-component filesystem round trip and its per-component program instantiation,
replaced by an in-memory Soufflé session behind the existing C ABI. The
fact-identity and ordering path loses redundant re-derivation, per-byte encoding,
per-component SQLite commits, and per-edge string sort keys. Memory lifetime
gains allocator relief at phase boundaries, and component payloads stop being
retained until assembly. Every step is proven content-neutral by an ordered-dump
A/B pair built in one tree.

**Tech Stack:** C++20; CMake 3.23+ with Ninja; LLVM/Clang 24.0.0git libraries
against the llvm@17 (clang 17.0.6) host compiler; vendored Soufflé at the pinned
revision with its in-memory relation API; SQLite; RocksDB; GoogleTest. No RTTI
and no exceptions in VERITAS code.

**Spec:** `docs/specs/veritas-build-analyze-round3-performance-design-spec.md`

## Global Constraints

Copied from the spec. Every task's requirements implicitly include this section.

- **No accuracy trade.** Epistemic states, the fact set, witness selection, and
  the set of executed `(SccId, WpaComponentKind)` components are unchanged.
- **No weakening of validation.** `Validate`, `ValidateSemanticRow`, and every
  identity check keep firing on exactly the inputs that trip them today. The
  spec's section 3.3 redundancy is removed by memoization and cheaper encoding,
  never by deleting a check.
- **No change to** SVF analysis configuration, `summary.v2`, `relations.v2`, the
  typed relation registry, rule bundles, model bundles, or fact identity.
- **No batching of multiple SCCs** into one Soufflé execution, and no parallel
  component execution. Task 3 reuses one program object across *sequential*
  components only.
- **No new CLI flag, no process-global cache.**
- Every expected `(SccId, WpaComponentKind)` remains independently materialized,
  cached, executed, canonicalized, and published, in reverse-topological order.
- `LogicalInputHash`, `FixpointHash`, and `ExternalHash` are byte-identical for
  unchanged input. `BatchId` is byte-identical for unchanged input **for a pair
  built in one tree** — it hashes `run_id`, which binds the toolchain identity,
  so it moves between revisions by construction.
- **No RTTI, no exceptions.** Use `veritas::Status` and `StatusOr<T>`; never
  `dynamic_cast`, `typeid`, `throw`, `try`, or `catch` in VERITAS code. The one
  exception is `src/wpa/SouffleRunner.cpp`, which is already the sanctioned
  RTTI/exceptions translation unit.
- **Apache-2.0 header** in the first 20 lines of every created or modified
  `.cpp`, `.h`, and `CMakeLists.txt`.
- All edits, builds, tests, and commits happen in the task worktree, never in the
  primary checkout and never on `main`.
- **Acceptance is measured, not asserted:** ≤375 s wall and ≤4 GiB peak RSS as
  the worst of three consecutive runs, and a full CTest pass with **no skips** (a
  `GTEST_SKIP` reports as passed, so the summary alone proves nothing).

## File Structure

| File | Responsibility in this change |
| --- | --- |
| `include/veritas/wpa/SouffleRunner.h` | C ABI for the in-memory session: open, insert, run, scan, reset, close. No Soufflé type appears in it. |
| `src/wpa/SouffleRunner.cpp` | Implements the session in the sanctioned RTTI/exceptions TU. Owns all `souffle::` usage. |
| `src/wpa/SouffleWpaExecutor.cpp` | Translates `facts::SemanticRow`/`WitnessEdge` rows to and from the ABI; stops creating temp directories and stops calling `RelationIo`. |
| `src/wpa/RelationIo.cpp`, `include/veritas/wpa/RelationIo.h` | Deleted once nothing calls them (Task 2). |
| `src/facts/AnalysisFact.cpp` | Reserve-and-write encoding in `AppendCell`/`AppendLenPrefixed`. |
| `src/facts/AnalysisFactBus.cpp` | Memoized identity in `Validate`; packed ranking keys in `MakeAnalysisFactBatch`. |
| `src/facts/FactStore.cpp` | Consume carried identities instead of re-deriving. |
| `src/wpa/WpaRunRepository.cpp` | Batched cache/state commits. |
| `src/wpa/WpaOrchestrator.cpp` | Retain only successor-support rows; reload for assembly. |
| `src/core/AllocatorRelief.h`, `src/core/AllocatorRelief.cpp` | New: the platform allocator-relief helper. |
| `src/analysis/ProjectAnalyzer.cpp` | Call relief at the phase boundaries it owns. |
| `tests/unit/wpa/SouffleSessionTest.cpp` | New: session ABI, reuse, and reset equivalence. |
| `tests/unit/facts/AnalysisFactBusTest.cpp` | Extended: packed ordering and memoized validation. |

---

### Task 1: In-memory Soufflé session in the runner's C ABI

**Files:**
- Modify: `include/veritas/wpa/SouffleRunner.h`
- Modify: `src/wpa/SouffleRunner.cpp`
- Test: `tests/unit/wpa/SouffleSessionTest.cpp` (create), `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**
- Consumes: `veritas_souffle_run(const char*, const char*, const char*, unsigned)` — the existing one-shot entry point, kept until Task 2 removes its last caller. `ProgramNameForComponent(std::string_view)` — already in `SouffleRunner.cpp`.
- Produces: the session ABI below, used by Task 2 and Task 3.

```c
/* A cell is either an interned symbol or a 64-bit number. The tag decides which
   member is meaningful, so no Souffle type crosses the boundary. */
enum VeritasSouffleCellKind {
  VERITAS_SOUFFLE_CELL_SYMBOL = 0,
  VERITAS_SOUFFLE_CELL_NUMBER = 1
};

typedef struct {
  unsigned char kind;
  unsigned long long number;
  const char* symbol;
} VeritasSouffleCell;

typedef struct VeritasSouffleSession VeritasSouffleSession;

/* Called once per row while scanning. Returns 0 to continue, non-zero to stop.
   The cell array is owned by the runner and valid only for the call. */
typedef int (*VeritasSouffleRowSink)(void* context,
                                     const VeritasSouffleCell* cells,
                                     unsigned long long arity);

int veritas_souffle_session_open(const char* component, unsigned jobs,
                                VeritasSouffleSession** out_session);
int veritas_souffle_session_insert(VeritasSouffleSession* session,
                                   const char* relation,
                                   const VeritasSouffleCell* cells,
                                   unsigned long long arity,
                                   unsigned long long row_count);
int veritas_souffle_session_run(VeritasSouffleSession* session);
int veritas_souffle_session_scan(VeritasSouffleSession* session,
                                const char* relation,
                                VeritasSouffleRowSink sink, void* context);
int veritas_souffle_session_reset(VeritasSouffleSession* session);
void veritas_souffle_session_close(VeritasSouffleSession* session);
```

Every `int` returns 0 on success and non-zero on failure, matching the existing
`veritas_souffle_run` convention.

- [ ] **Step 1: Write the failing equivalence test**

The test must show the session produces the same derived rows as the file path for
one real component. Use the smallest existing WPA fixture the tests already build
(see how `WpaExecutorConformanceTest` constructs a component in
`tests/unit/wpa/`), so the relations are the real `relations.v2` ones.

```cpp
// tests/unit/wpa/SouffleSessionTest.cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "veritas/wpa/SouffleRunner.h"

namespace veritas::wpa {
namespace {

struct ScannedRow {
  std::vector<std::string> symbols;
  std::vector<unsigned long long> numbers;
  std::vector<unsigned char> kinds;
};

int Collect(void* context, const VeritasSouffleCell* cells,
            unsigned long long arity) {
  auto* rows = static_cast<std::vector<ScannedRow>*>(context);
  ScannedRow row;
  for (unsigned long long i = 0; i < arity; ++i) {
    row.kinds.push_back(cells[i].kind);
    row.numbers.push_back(cells[i].number);
    row.symbols.push_back(cells[i].symbol == nullptr ? "" : cells[i].symbol);
  }
  rows->push_back(std::move(row));
  return 0;
}

TEST(SouffleSessionTest, InsertRunScanReturnsTheDerivedRows) {
  VeritasSouffleSession* session = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("reachability", 1, &session), 0);
  ASSERT_NE(session, nullptr);

  // One DirectCall row expressed in the dense encoding relations.v2.dl declares.
  const VeritasSouffleCell cells[] = {
      {VERITAS_SOUFFLE_CELL_NUMBER, 1, nullptr},  // call_site_id
      {VERITAS_SOUFFLE_CELL_NUMBER, 1, nullptr},  // caller_id
      {VERITAS_SOUFFLE_CELL_NUMBER, 2, nullptr},  // callee_id
      {VERITAS_SOUFFLE_CELL_NUMBER, 0, nullptr},  // dispatch = DIRECT
      {VERITAS_SOUFFLE_CELL_NUMBER, 0, nullptr},  // epistemic = MUST
  };
  ASSERT_EQ(veritas_souffle_session_insert(session, "DirectCall", cells, 5, 1),
            0);
  ASSERT_EQ(veritas_souffle_session_run(session), 0);

  std::vector<ScannedRow> rows;
  ASSERT_EQ(veritas_souffle_session_scan(session, "Reachable", Collect, &rows),
            0);
  ASSERT_FALSE(rows.empty());

  veritas_souffle_session_close(session);
}

TEST(SouffleSessionTest, OpeningAnUnknownComponentFails) {
  VeritasSouffleSession* session = nullptr;
  EXPECT_NE(veritas_souffle_session_open("not-a-component", 1, &session), 0);
  EXPECT_EQ(session, nullptr);
}

}  // namespace
}  // namespace veritas::wpa
```

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.*'
```

Expected: link failure, `undefined symbol: veritas_souffle_session_open`. Record
the exact failure in the pull request; a test that fails for an unexpected reason
is not a red test.

- [ ] **Step 3: Implement the session in the runner**

In `SouffleRunner.cpp`, define the opaque struct to own the program and a
relation-name index, and implement each entry point. All `souffle::` usage stays
in this file.

```cpp
struct VeritasSouffleSession {
  std::string component;
  std::unique_ptr<souffle::SouffleProgram> program;
};

int veritas_souffle_session_open(const char* component, unsigned jobs,
                                VeritasSouffleSession** out_session) {
  if (component == nullptr || out_session == nullptr) {
    return 2;
  }
  *out_session = nullptr;
  const char* program_name = ProgramNameForComponent(component);
  if (program_name == nullptr) {
    return 2;
  }
  std::unique_ptr<souffle::SouffleProgram> program(
      souffle::ProgramFactory::newInstance(program_name));
  if (program == nullptr) {
    return 3;
  }
  program->setNumThreads(jobs);
  auto session = std::make_unique<VeritasSouffleSession>();
  session->component = component;
  session->program = std::move(program);
  *out_session = session.release();
  return 0;
}
```

`insert` looks the relation up by name, builds a `souffle::tuple`, pushes each cell
with `t << RamDomain(cell.number)` for `VERITAS_SOUFFLE_CELL_NUMBER` and
`t << std::string(cell.symbol)` for `VERITAS_SOUFFLE_CELL_SYMBOL`, then calls
`relation->insert(t)` — all inside a `try`/`catch (const std::exception&)` that
returns non-zero, matching the existing one-shot function's shape.

`scan` walks the relation with a `for (const auto& t : *relation)` loop, reading
each column with `t >> value`, filling a reusable `std::vector<VeritasSouffleCell>`
and calling `sink`. It must distinguish symbol from number columns the same way
the current text reader does; the reader's per-column knowledge lives in
`RelationIo::ReadOutput` today, so the runner needs the relation's column domains.
Pass them in: add a `const unsigned char* cell_kinds, unsigned long long arity`
pair to `veritas_souffle_session_scan` and have the caller pass the schema's
`ColumnDomain` tags, mapping `kString` to `VERITAS_SOUFFLE_CELL_SYMBOL` and
everything else to `VERITAS_SOUFFLE_CELL_NUMBER`.

`run` calls `program->runAll("", "", /*performIO=*/false, /*pruneImdtRels=*/true)`
inside the same try/catch and then returns 0. `reset` is Task 3. `close` deletes
the session.

- [ ] **Step 4: Run the test and verify it passes**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.*'
```

Expected: PASS, both tests.

- [ ] **Step 5: Commit**

```bash
git add include/veritas/wpa/SouffleRunner.h src/wpa/SouffleRunner.cpp \
        tests/unit/wpa/SouffleSessionTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "perf(wpa): add an in-memory Souffle session to the runner C ABI"
```

---

### Task 2: Execute components through the session and delete the file path

**Files:**
- Modify: `src/wpa/SouffleWpaExecutor.cpp:53-95`
- Delete: `src/wpa/RelationIo.cpp`, `include/veritas/wpa/RelationIo.h`
- Modify: `src/wpa/CMakeLists.txt`
- Test: `tests/unit/wpa/SouffleSessionTest.cpp`, plus the existing
  `WpaExecutorConformanceTest`

**Interfaces:**
- Consumes: the Task 1 session ABI; `facts::RelationsV2()` and
  `facts::RelationSchema` for per-column domains; `WpaLogicalComponentInput`
  with `.edb`, `.mappings`, `.component`, and `ComponentDomains(component)`.
- Produces: unchanged `SouffleWpaExecutor::Execute` signature returning
  `StatusOr<facts::RawWpaEvaluation>`. No caller changes.

- [ ] **Step 1: Write the failing test — file path and session agree**

Add a test that runs one fixture component through `SouffleWpaExecutor::Execute`
and asserts the resulting `RawWpaEvaluation` matches a row-set captured from the
pre-change binary. Capture that expectation **before** editing the executor, from
the current file-based implementation, and store it as a sorted row dump in the
test so the comparison is real.

```cpp
TEST(SouffleSessionTest, ExecutorRowsMatchTheCapturedBaseline) {
  // Fixture component built the same way WpaExecutorConformanceTest builds one.
  auto raw = ExecuteFixtureComponent();
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  EXPECT_EQ(RenderRowsSorted(*raw), kCapturedRowDump);
}
```

- [ ] **Step 2: Run it and verify it passes against the old implementation**

This is the one step in the plan that must pass *before* the change. It pins the
expectation to the implementation that exists.

```bash
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.ExecutorRowsMatchTheCapturedBaseline'
```

Expected: PASS. If it fails, the capture is wrong; fix the capture.

- [ ] **Step 3: Rewrite `Execute` to use the session**

Replace the `mkdtemp`/`DirCleanup`/`RelationIo` body with: open a session; group
`input.edb` rows by `row.relation`, and for each relation insert its rows with the
column kinds taken from `facts::RelationsV2().Get(row.relation).columns[i].domain`;
run; scan each `ComponentDomains(input.component)[*].derived` relation into
`raw.results`; scan the witness relation into `raw.witnesses`; close.

Mapping stays in `SouffleWpaExecutor`: numbers for `kFunctionId`, `kValueId`,
`kMemoryId`, `kCallSiteId`, `kFactId` and enums; symbols for `kString`; and the
reverse mapping through `input.mappings.*.ToStable` with the same
`ValidateSemanticRow` call the reader made. Delete `DirCleanup` and the
`<unistd.h>` include.

- [ ] **Step 4: Verify the executor still agrees with its baseline and with the C++ oracle**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.*:WpaExecutorConformanceTest.*'
```

Expected: PASS, including
`WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts`.

- [ ] **Step 5: Delete `RelationIo` and prove nothing references it**

```bash
grep -rn "RelationIo" src include tests | grep -v Binary || echo "unreferenced"
```

Expected: no matches. Then remove the two files and their `CMakeLists.txt` entries
and rebuild:

```bash
cmake --build --preset default --target veritas_unit_tests
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add -A src/wpa include/veritas/wpa tests/unit/wpa
git commit -m "perf(wpa): execute components in memory and delete the relation file path"
```

---

### Task 3: Reuse one session across components

**Files:**
- Modify: `src/wpa/SouffleRunner.cpp` (`veritas_souffle_session_reset`)
- Modify: `src/wpa/SouffleWpaExecutor.cpp`
- Test: `tests/unit/wpa/SouffleSessionTest.cpp`

**Interfaces:**
- Produces: `veritas_souffle_session_reset(session)` returning 0 on success, with
  the semantics "indistinguishable from a freshly opened session for the same
  component". Task 2's `Execute` gains an optional session parameter it reuses.

- [ ] **Step 1: Write the failing reset-equivalence test**

```cpp
TEST(SouffleSessionTest, ResetMakesAReusedSessionMatchAFreshOne) {
  VeritasSouffleSession* reused = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("reachability", 1, &reused), 0);

  ASSERT_EQ(InsertDirectCall(reused, /*call_site=*/1, /*caller=*/1, /*callee=*/2), 0);
  ASSERT_EQ(veritas_souffle_session_run(reused), 0);
  const auto first = ScanAll(reused, "Reachable");

  ASSERT_EQ(veritas_souffle_session_reset(reused), 0);
  // The same insert on the reused session must reproduce the same rows, which
  // only holds if the reset emptied the input and derived relations.
  ASSERT_EQ(InsertDirectCall(reused, /*call_site=*/1, /*caller=*/1, /*callee=*/2), 0);
  ASSERT_EQ(veritas_souffle_session_run(reused), 0);
  EXPECT_EQ(ScanAll(reused, "Reachable"), first);

  ASSERT_EQ(veritas_souffle_session_reset(reused), 0);
  // A different input on the same session must produce only its own rows.
  ASSERT_EQ(InsertDirectCall(reused, /*call_site=*/9, /*caller=*/9, /*callee=*/8), 0);
  ASSERT_EQ(veritas_souffle_session_run(reused), 0);
  EXPECT_NE(ScanAll(reused, "Reachable"), first);

  veritas_souffle_session_close(reused);
}
```

- [ ] **Step 2: Run it and verify it fails**

```bash
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.ResetMakesAReusedSessionMatchAFreshOne'
```

Expected: FAIL — the stub returns non-zero, or the second scan still carries the
first run's rows.

- [ ] **Step 3: Implement reset**

Call `program->purgeInputRelations()`, `purgeOutputRelations()`, and
`purgeInternalRelations()` inside the try/catch, and return 0. If the pinned
revision's purge set proves insufficient — the test will say so — the fallback is
to close and reopen the session, which is still one instantiation per run rather
than per component only if reopening is cheap; measure before choosing. Do not
proceed past Step 4 with a failing test.

- [ ] **Step 4: Verify reset equivalence, then reuse the session in the executor**

```bash
./build/bin/veritas_unit_tests --gtest_filter='SouffleSessionTest.*'
```

Expected: PASS. Then have `SouffleWpaExecutor` hold one session per component kind
for the duration of a run and reset it between components, and re-run
`WpaExecutorConformanceTest` to confirm the results are unchanged.

- [ ] **Step 5: Commit**

```bash
git add src/wpa tests/unit/wpa
git commit -m "perf(wpa): reuse one Souffle session across sequential components"
```

---

### Task 4: Memoized fact identity and reserve-and-write encoding

**Files:**
- Modify: `src/facts/AnalysisFact.cpp:50,206-215`
- Modify: `src/facts/AnalysisFactBus.cpp:289-370`
- Modify: `src/facts/FactStore.cpp`
- Test: `tests/unit/facts/AnalysisFactBusTest.cpp`

**Interfaces:**
- Produces: unchanged `DeriveFactId(const SemanticRow&) -> StatusOr<StableId>`. An
  internal `RowIdentityMemo` keyed on the encoded row, local to the validation
  pass. No public signature changes.

- [ ] **Step 1: Write the failing memo test — the same checks still fire**

```cpp
TEST(AnalysisFactBusTest, ValidateStillRejectsEachTamperedIdentity) {
  AnalysisFactBatch batch = MakeSmallValidBatch();

  // Baseline: a well-formed batch validates.
  ASSERT_TRUE(bus.Validate(batch).ok());

  // A mutated fact id is still rejected.
  AnalysisFactBatch bad_fact = batch;
  bad_fact.facts[0].fact_id = core::ParseStableIdOrDie("fact:sha256:" + std::string(64, '0'));
  EXPECT_FALSE(bus.Validate(bad_fact).ok());

  // A mutated witness result row is still rejected.
  AnalysisFactBatch bad_result = batch;
  bad_result.witnesses[0].result.row.cells[0] = core::ParseStableIdOrDie("function:sha256:" + std::string(64, 'a'));
  EXPECT_FALSE(bus.Validate(bad_result).ok());

  // A mutated witness input row is still rejected.
  AnalysisFactBatch bad_input = batch;
  bad_input.witnesses[0].input.row.cells[0] = core::ParseStableIdOrDie("function:sha256:" + std::string(64, 'b'));
  EXPECT_FALSE(bus.Validate(bad_input).ok());
}
```

- [ ] **Step 2: Run it and verify it passes before the change**

```bash
./build/bin/veritas_unit_tests --gtest_filter='AnalysisFactBusTest.ValidateStillRejectsEachTamperedIdentity'
```

Expected: PASS. This test is the guard that memoization does not weaken anything;
it must pass both before and after.

- [ ] **Step 3: Add the memo and reserve the encoding buffer**

In `AnalysisFactBus::Validate`, build one `std::map<std::string, core::StableId>`
keyed on the encoded row and consult it at lines 319, 348, and 352 instead of
calling `DeriveFactId` each time. In `AnalysisFact.cpp`, give `AppendCell` and
`AppendLenPrefixed` a `reserve` for the encoded length and write into the buffer
directly rather than `push_back` per byte. In `FactStore`, consume the batch's
carried `fact_id` where it re-derives the same value.

- [ ] **Step 4: Verify the guards still fire and the bus tests pass**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='AnalysisFactBusTest.*:FactStoreTest.*:AnalysisFactTest.*'
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/facts tests/unit/facts
git commit -m "perf(facts): memoize row identity and reserve the encoding buffer"
```

---

### Task 5: Batched component cache commits

**Files:**
- Modify: `src/wpa/WpaRunRepository.cpp:566-625`
- Modify: `include/veritas/wpa/WpaRunRepository.h`
- Test: `tests/unit/wpa/WpaRunRepositoryTest.cpp`

**Interfaces:**
- Consumes: the existing `MetadataStore::BeginTransaction`, `CommitTransaction`,
  `RollbackTransaction`, and `BulkInsertBatcher`.
- Produces: `Status WpaRunRepository::FlushComponentCache()` and a
  `BeginRun`/`CompleteRun` pair that opens and closes the batch window. The
  content-addressed `PutIfAbsent` stays per component and is not batched.

- [ ] **Step 1: Write the failing durability test**

```cpp
TEST(WpaRunRepositoryTest, CompletedRunLeavesEveryComponentQueryable) {
  // After a run whose cache commits are batched, every component stored during
  // the run is still loadable, and a second run of the same input reuses them.
  ASSERT_TRUE(StoreThreeComponents().ok());
  ASSERT_TRUE(repository.FlushComponentCache().ok());
  EXPECT_EQ(LoadReusableComponentCount(), 3);

  // A run that fails before its final flush still leaves the store consistent.
  ASSERT_FALSE(StoreThreeComponentsThenFail().ok());
  EXPECT_TRUE(Query("SELECT COUNT(*) FROM wpa_component_states_v2 WHERE status = -1;").ok());
}
```

- [ ] **Step 2: Run it and verify it fails**

```bash
./build/bin/veritas_unit_tests --gtest_filter='WpaRunRepositoryTest.CompletedRunLeavesEveryComponentQueryable'
```

Expected: FAIL — `FlushComponentCache` does not exist.

- [ ] **Step 3: Batch the commits**

Accumulate the two INSERT statement argument sets into the batcher and commit
every N components (start with 256) and once more at `CompleteRun`. Keep the
per-component `PutIfAbsent` where it is, so every committed cache row references
an object that is already present. On any failure, roll back the open batch.

- [ ] **Step 4: Verify and record the durability change in the spec's status section**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='WpaRunRepositoryTest.*'
```

Expected: PASS. Add a line to the spec's implementation-status section stating the
batch size and the crash window it implies.

- [ ] **Step 5: Commit**

```bash
git add src/wpa include/veritas/wpa tests/unit/wpa
git commit -m "perf(wpa): commit component cache state in batches"
```

---

### Task 6: Order-preserving packed ordering keys

**Files:**
- Modify: `src/facts/AnalysisFactBus.cpp:107-118,170-280`
- Test: `tests/unit/facts/AnalysisFactBusTest.cpp`

**Interfaces:**
- Produces: internal `struct FactRanks` with `uint32_t Rank(std::string_view encoded_key)`,
  built once per assembly call; `KeyedFact` carries `uint32_t key_rank` and
  `KeyedWitness` carries `result_rank`, `rule_rank`, `input_rank`, `input_ordinal`.
  `MakeAnalysisFactBatch`'s signature is unchanged.

- [ ] **Step 1: Write the failing order-equivalence test**

```cpp
TEST(AnalysisFactBusTest, PackedRanksPreserveStringOrderIncludingTies) {
  // Four witnesses chosen so that string order differs from insertion order and
  // so that each tiebreaker level is exercised: same result with different rule,
  // then different input, then different ordinal.
  AnalysisFactBatch batch = MakeBatchWithWitnessTiebreakers();
  const auto expected = CanonicalWitnessOrderByStringKeys(batch);  // the old comparator, kept in the test
  auto packed = MakeAnalysisFactBatch(std::move(batch));
  EXPECT_EQ(RenderWitnessKeys(packed), expected);
  EXPECT_EQ(packed.batch_id, DeriveBatchId(packed));
}
```

Keep `CanonicalWitnessOrderByStringKeys` in the test file as the independent
implementation of the old comparator, so the two orderings are compared rather
than assumed equal.

- [ ] **Step 2: Run it and verify it fails**

```bash
./build/bin/veritas_unit_tests --gtest_filter='AnalysisFactBusTest.PackedRanksPreserveStringOrderIncludingTies'
```

Expected: FAIL — the helper does not compile yet.

- [ ] **Step 3: Implement the ranks**

Build the distinct encoded keys as `std::string_view` into the existing encoded
storage, sort them byte-wise with `std::less<>`, and assign `uint32_t` ranks in
that order. Replace the `std::string` members of `KeyedFact` and `KeyedWitness`
with their ranks and change both comparators to compare the rank tuples. Rank the
rule id the same way, over the distinct rule ids.

- [ ] **Step 4: Verify ordering, uniqueness, and identity**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='AnalysisFactBusTest.*'
```

Expected: PASS, including the duplicate-fact ownership tests and the
`BatchId`-under-reordered-completions test.

- [ ] **Step 5: Commit**

```bash
git add src/facts tests/unit/facts
git commit -m "perf(facts): order facts and witnesses by packed ranks"
```

---

### Task 7: Allocator relief at phase boundaries

**Files:**
- Create: `include/veritas/core/AllocatorRelief.h`, `src/core/AllocatorRelief.cpp`
- Modify: `src/core/CMakeLists.txt`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Test: `tests/unit/core/AllocatorReliefTest.cpp`

**Interfaces:**
- Produces: `void veritas::core::ReleaseFreedMemory()`, best-effort, never fails,
  no return value, valid to call at any time.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AllocatorReliefTest, ReleaseIsSafeWithNothingToReleaseAndAfterAllocation) {
  core::ReleaseFreedMemory();  // must not crash or abort on a clean heap
  {
    std::vector<std::byte> big(64u * 1024u * 1024u);
    (void)big[0];
  }
  core::ReleaseFreedMemory();  // must not crash after a large free
  SUCCEED();
}
```

- [ ] **Step 2: Run it and verify it fails**

Expected: link failure — the symbol does not exist.

- [ ] **Step 3: Implement it and wire the calls**

Implement with `malloc_zone_pressure_relief(nullptr, 0)` under `__APPLE__` and
`malloc_trim(0)` under `__GLIBC__`, with an empty body otherwise. Call it in
`ProjectAnalyzer` after the SVF stage returns and after WPA results are consumed,
and in the publication path after the batch is published. Each call is a no-op if
the platform offers nothing.

- [ ] **Step 4: Measure that it actually releases something**

Run the motivating command with the allocator-relief calls instrumented to log
RSS before and after each call, and record the four deltas in the spec's
implementation-status section. A call that releases nothing must be reported as a
finding, not silently kept.

- [ ] **Step 5: Commit**

```bash
git add src/core include/veritas/core src/analysis tests/unit/core
git commit -m "perf(core): return emptied allocator arenas at phase boundaries"
```

---

### Task 8: Reload component payloads for assembly instead of retaining them

**Files:**
- Modify: `src/wpa/WpaOrchestrator.cpp:44-71,172-284`
- Modify: `src/facts/AnalysisFactBus.cpp:170-200`
- Test: `tests/unit/wpa/WpaOrchestratorTest.cpp`

**Interfaces:**
- Consumes: `WpaRunRepository::LoadReusableComponent` for the reload path.
- Produces: `SuccessorSupport` reads from a retained support-only projection
  rather than from full completed results. `MakeAnalysisFactBatch` gains an
  overload taking a loader callback with signature
  `StatusOr<wpa::WpaComponentResult>(const wpa::WpaComponentKey&)`.

- [ ] **Step 1: Write the failing test — assembly output is unchanged**

```cpp
TEST(WpaOrchestratorTest, ReloadedAssemblyMatchesRetainedAssembly) {
  auto retained = MakeAnalysisFactBatch(BuildRunRetainingPayloads());
  auto reloaded = MakeAnalysisFactBatchWithLoader(BuildRunReleasingPayloads());
  EXPECT_EQ(RenderBatch(retained), RenderBatch(reloaded));
  EXPECT_EQ(retained.batch_id, reloaded.batch_id);
}
```

- [ ] **Step 2: Run it and verify it fails**

Expected: FAIL — the loader overload does not exist.

- [ ] **Step 3: Retain only support rows and reload for assembly**

Change `SuccessorSupport` to read from a per-completed-component vector holding
only rows whose relation is a `derived` relation of a domain with
`support.has_value()`. Drop the full `WpaComponentResult` from the orchestrator's
retained set once it has been stored. Implement the loader overload to fetch each
component through `LoadReusableComponent` during assembly and release it after its
facts and witnesses are keyed.

- [ ] **Step 4: Verify, then measure the wall-time cost of the reloads**

```bash
cmake --build --preset default --target veritas_unit_tests
./build/bin/veritas_unit_tests --gtest_filter='WpaOrchestratorTest.*:AnalysisFactBusTest.*:WpaExecutorConformanceTest.*'
```

Expected: PASS. Then run the motivating command and compare its wall time against
the pre-Task-8 build. If the reload cost reverses the wall-time gain, switch to
the spec's stated fallback — retain facts, reload only witnesses — and re-measure.
Record which variant was chosen and why.

- [ ] **Step 5: Commit**

```bash
git add src/wpa src/facts tests/unit/wpa tests/unit/facts
git commit -m "perf(wpa): reload component payloads for assembly"
```

---

### Task 9: Prove equivalence and measure acceptance

**Files:**
- Modify: `docs/specs/veritas-build-analyze-round3-performance-design-spec.md`
  (implementation-status section)
- Test: the full suite

**Interfaces:**
- Consumes: everything above. Produces: the recorded evidence.

- [ ] **Step 1: Build the pre-change member of the A/B pair in this tree**

```bash
git stash push -u -m "round3-ab"     # or a WIP commit; never a bare stash pop
git checkout <pre-change-revision>
cmake --build --preset default --target veritas-build
./build/bin/veritas-build analyze --project /Users/skg7on/Workspace/Projects/leveldb \
  --output /tmp/ab-before/store
```

Record `engine_toolchain_identity`, the run id, and the four table dumps.

- [ ] **Step 2: Build the after member in the same tree**

```bash
git checkout <task-branch>
cmake --build --preset default --target veritas-build
./build/bin/veritas-build analyze --project /Users/skg7on/Workspace/Projects/leveldb \
  --output /tmp/ab-after/store
```

- [ ] **Step 3: Apply the spec's section 9.1 instrument**

Compare `engine_toolchain_identity` first. If unchanged, require all four digests
and all four hashes equal. If it moved, require `analysis_facts` equality, equal
row counts, and equality of the identity-bearing tables under the exclusion set —
**which this step must determine empirically** rather than assume, per the caveat
the spec records. Write the determined exclusion set into the spec.

- [ ] **Step 4: Measure acceptance, worst of three runs**

```bash
for i in 1 2 3; do
  rm -rf /tmp/acc-$i
  /usr/bin/time -lp ./build/bin/veritas-build analyze \
    --project /Users/skg7on/Workspace/Projects/leveldb --output /tmp/acc-$i/store
done
```

Record all three wall times and peak RSS values. State the worst of each against
375 s and 4 GiB. If the 4 GiB limit is missed, report the measured floor and its
attribution per the spec's risk 1 rather than describing the limit as met.

- [ ] **Step 5: Run the full pre-push verification**

```bash
rm -rf build && cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
cd build && ctest --output-on-failure
cd .. && git diff --check
```

Confirm 100% pass with **no skips**, run the M9 entry gate and the
`wpa-qualification` label, verify the license headers on every modified file, and
confirm `git status --porcelain` is empty after committing.

- [ ] **Step 6: Commit the recorded results**

```bash
git add docs/specs/veritas-build-analyze-round3-performance-design-spec.md
git commit -m "docs(perf): record the round-3 equivalence proof and measurements"
```

---

## Plan Self-Review Record

**Spec coverage.** Every spec section maps to a task: §7.1 → Tasks 1–2; §7.2 →
Task 3; §7.3 → Task 4; §7.4 → Task 5; §7.5 → Task 6; §7.6 → Task 7; §7.7 → Task 8;
§9.1–§9.5 → Task 9. Goals 1–7 map to Tasks 1–8 in order; goal 8 is measured in
Task 9. §3.1–§3.6 root causes are each addressed by the task above them.

**Known gap carried deliberately.** Task 1 Step 3 needs the relation column
domains at scan time and therefore changes the scan signature relative to the
sketch in the spec's section 7.1, which fixed only the constraint that no Soufflé
type crosses the boundary. The plan's signature is the binding one.

**Placeholder scan.** No "TBD", "handle edge cases", or "similar to Task N"
remains. Two steps deliberately defer a *choice* to measurement rather than to
the reader — Task 3 Step 3's purge-versus-reopen fallback, and Task 8 Step 4's
facts-versus-witnesses retention — and both name the measurement that decides and
where the result is recorded.

**Type consistency.** `VeritasSouffleCell`, `VeritasSouffleSession`,
`VeritasSouffleRowSink`, and the six session entry points are defined once in Task
1 and used with the same names and arities in Tasks 2 and 3.
`ReleaseFreedMemory` is defined and called in Task 7 only. `FactRanks` and the
`KeyedFact`/`KeyedWitness` rank members are introduced in Task 6 and not
referenced elsewhere.
