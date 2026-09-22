# `veritas-build analyze` Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the LevelDB `veritas-build analyze` workload complete within
375 seconds and 4 GB peak RSS without changing logical inputs, canonical
results, provenance, or engine selection.

**Architecture:** Construct a validated non-owning summary index once per WPA
run and reuse it for every SCC component. Keep component payloads single-owned
until a consuming fact-batch assembly moves the canonical owner of each fact
and witness into pre-keyed output vectors; also replace stream-based digest
formatting with fixed-width nibble encoding.

**Tech Stack:** C++20, LLVM/Clang 22+ libraries, pinned SVF, compiled Soufflé,
RocksDB, SQLite, CMake/Ninja, GoogleTest.

**Spec:**
[`docs/specs/veritas-build-analyze-performance-design-spec.md`](../specs/veritas-build-analyze-performance-design-spec.md)

**Tracking:** [GitHub issue #133](https://github.com/skg7on/VERITAS/issues/133)

## Global Constraints

- Preserve the production Soufflé engine and all four WPA domains.
- Preserve reverse-topological SCC execution and one immutable logical input
  for every `(SccId, WpaComponentKind)` pair.
- Preserve byte-identical EDB rows, mappings, `LogicalInputHash`,
  `FixpointHash`, `ExternalHash`, fact identity, witness selection, and
  `BatchId` for unchanged inputs.
- Use `veritas::Status` and `veritas::StatusOr<T>` for failures; do not use
  exceptions, RTTI, `dynamic_cast`, or `typeid`.
- Every modified C++, header, and CMake file retains the full Apache-2.0 header
  in its first 20 lines.
- Do not add process-global caches, new CLI flags, schema versions, relations,
  rules, models, dependencies, or engine behavior.
- Perform all edits, builds, tests, commits, pushes, and PR updates in the task
  worktree on `claude/improve-analyze-performance`.

## File Structure

| File | Responsibility in this change |
| --- | --- |
| `src/core/Hash.cpp` | Locale-independent fixed-width digest-to-hex encoding. |
| `tests/unit/core/HashTest.cpp` | Exact and locale-independent encoding regression coverage. |
| `include/veritas/wpa/WpaInputMaterializer.h` | Non-owning run-scoped `WpaSummaryIndex` and indexed build overload. |
| `src/wpa/WpaInputMaterializer.cpp` | Index construction/validation and indexed lookup during materialization. |
| `src/wpa/WpaOrchestrator.cpp` | Build the index once, reuse it for every component, and stop flattening duplicate payloads. |
| `include/veritas/wpa/WpaOrchestrator.h` | Remove redundant flattened result vectors from `WpaRunResult`. |
| `tests/unit/wpa/WpaInputMaterializerTest.cpp` | Indexed/fallback equivalence and index misuse tests. |
| `tests/unit/wpa/WpaOrchestratorTest.cpp` | Preserve component ordering, support, cache, and failure contracts after index reuse. |
| `include/veritas/facts/AnalysisFactBus.h` | Consuming `MakeAnalysisFactBatch(WpaRunResult)` contract and metadata-only completion payload documentation. |
| `src/facts/AnalysisFactBus.cpp` | Move-based ownership selection and precomputed canonical sort keys. |
| `src/analysis/ProjectAnalyzer.cpp` | Move the successful primary WPA result into batch assembly. |
| `tests/unit/facts/AnalysisFactBusTest.cpp` | Consuming assembly, payload stripping, canonical order, and identity tests. |
| `tests/integration/wpa/WpaEndToEndTest.cpp` | Use `AnalysisFactBatch` as the sole flattened result and preserve end-to-end publication checks. |

---

### Task 1: Make digest rendering fixed-width and locale-independent

**Files:**

- Modify: `tests/unit/core/HashTest.cpp:15-73`
- Modify: `src/core/Hash.cpp:140-147`

**Interfaces:**

- Consumes: `SHA256Digest`, `kSHA256DigestBytes`.
- Produces: unchanged `std::string DigestToHex(const SHA256Digest&)` with
  exactly 64 lowercase ASCII hexadecimal characters.

- [ ] **Step 1: Add a failing locale-independence test**

Add `<locale>` and a test-only numeric facet that corrupts stream-formatted
unsigned integers. Guard the process locale with RAII so it is restored even
when an expectation fails:

```cpp
class CorruptingNumPut : public std::num_put<char> {
 protected:
  iter_type do_put(iter_type out, std::ios_base&, char_type,
                   unsigned long) const override {
    *out++ = 'x';
    return out;
  }

  iter_type do_put(iter_type out, std::ios_base&, char_type,
                   unsigned long long) const override {
    *out++ = 'x';
    return out;
  }
};

class GlobalLocaleGuard {
 public:
  GlobalLocaleGuard()
      : previous_(std::locale()) {
    std::locale::global(std::locale(previous_, new CorruptingNumPut));
  }
  ~GlobalLocaleGuard() { std::locale::global(previous_); }

 private:
  std::locale previous_;
};

TEST(HashTest, DigestToHexIsIndependentOfTheGlobalNumericLocale) {
  SHA256Digest digest{};
  digest[0] = std::byte{0x01};
  digest[1] = std::byte{0xaf};
  digest[31] = std::byte{0xf0};
  GlobalLocaleGuard locale;
  EXPECT_EQ(DigestToHex(digest),
            "01af0000000000000000000000000000000000000000000000000000000000f0");
}
```

- [ ] **Step 2: Run the test and verify the stream implementation fails**

Run:

```bash
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target HashTest
./build/tests/unit/core/HashTest \
  --gtest_filter=HashTest.DigestToHexIsIndependentOfTheGlobalNumericLocale
```

Expected: the new test fails because `ostringstream` uses the corrupting
numeric facet and does not return the canonical 64-character string.

- [ ] **Step 3: Replace stream formatting with direct nibble encoding**

Replace the `ostringstream` implementation with:

```cpp
std::string DigestToHex(const SHA256Digest& digest) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string result(kSHA256DigestBytes * 2, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const auto byte = static_cast<std::uint8_t>(digest[i]);
    result[2 * i] = kHexDigits[byte >> 4];
    result[2 * i + 1] = kHexDigits[byte & 0x0f];
  }
  return result;
}
```

Remove now-unused `<iomanip>` and `<sstream>` includes from `Hash.cpp`.

- [ ] **Step 4: Run the complete hash and stable-ID tests**

Run:

```bash
cmake --build --preset default --target HashTest IdsTest
./build/tests/unit/core/HashTest
./build/tests/unit/core/IdsTest
```

Expected: both executables pass with zero failed tests; existing SHA-256 and
stable-ID strings remain unchanged.

- [ ] **Step 5: Commit the isolated primitive optimization**

```bash
git add src/core/Hash.cpp tests/unit/core/HashTest.cpp
git commit -m "perf(core): encode digests without stream formatting"
```

---

### Task 2: Reuse one validated summary index across WPA components

**Files:**

- Modify: `include/veritas/wpa/WpaInputMaterializer.h:15-42`
- Modify: `src/wpa/WpaInputMaterializer.cpp:298-340`
- Modify: `src/wpa/WpaOrchestrator.cpp:119-190`
- Modify: `tests/unit/wpa/WpaInputMaterializerTest.cpp:129-417`
- Verify: `tests/unit/wpa/WpaOrchestratorTest.cpp`

**Interfaces:**

- Consumes: one stable `std::span<const summary::SummaryArtifact>` owned by
  `WpaRunRequest` for the duration of `WpaOrchestrator::Run`.
- Produces: `WpaSummaryIndex::Build`, `WpaSummaryIndex::Covers`,
  `WpaSummaryIndex::Lookup`, `WpaSummaryIndex::Contains`, and
  `WpaInputMaterializer::Build(request, index)`.

- [ ] **Step 1: Add failing indexed-materialization tests**

Add tests with these exact behaviors:

```cpp
TEST(WpaInputMaterializerTest,
     PrebuiltSummaryIndexMatchesFallbackMaterialization) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();
  auto index = WpaSummaryIndex::Build(artifacts);
  ASSERT_TRUE(index.ok()) << index.status().message();

  const auto request =
      Request(artifacts, WpaComponentKind::kReachability, "caller");
  auto fallback = WpaInputMaterializer::Build(request);
  auto indexed = WpaInputMaterializer::Build(request, *index);
  ASSERT_TRUE(fallback.ok()) << fallback.status().message();
  ASSERT_TRUE(indexed.ok()) << indexed.status().message();
  EXPECT_EQ(indexed->edb, fallback->edb);
  EXPECT_EQ(indexed->local_roots, fallback->local_roots);
  EXPECT_EQ(indexed->successor_roots, fallback->successor_roots);
  EXPECT_EQ(indexed->logical_input_hash, fallback->logical_input_hash);
  EXPECT_EQ(indexed->mappings.functions.StableIds(),
            fallback->mappings.functions.StableIds());
}

TEST(WpaInputMaterializerTest, RejectsSummaryIndexForAnotherSpan) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();
  auto index = WpaSummaryIndex::Build(artifacts);
  ASSERT_TRUE(index.ok());
  const std::vector<summary::SummaryArtifact> copied = artifacts;
  auto result = WpaInputMaterializer::Build(
      Request(copied, WpaComponentKind::kReachability, "caller"), *index);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(result.status().message(),
            "summary index does not cover request summaries");
}

TEST(WpaInputMaterializerTest, RejectsDuplicateSummaryFunctionIdentity) {
  const auto summary = V2Summary("duplicate");
  const std::vector<summary::SummaryArtifact> artifacts = {summary, summary};
  auto index = WpaSummaryIndex::Build(artifacts);
  ASSERT_FALSE(index.ok());
  EXPECT_EQ(index.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(index.status().message(),
            "duplicate summary function identity");
}
```

If the current equality surface does not provide `operator==` for rooted input
facts, compare their `fact`, `provenance_ref`, `producer_id`,
`source_anchor_id`, `summary_id`, and `description` fields explicitly; do not
drop root-equivalence coverage.

- [ ] **Step 2: Run the test build and verify it fails on the missing API**

Run:

```bash
cmake --build --preset default --target WpaInputMaterializerTest
```

Expected: compilation fails because `WpaSummaryIndex` and the indexed `Build`
overload do not exist.

- [ ] **Step 3: Implement the non-owning index**

Add this public shape to `WpaInputMaterializer.h`:

```cpp
class WpaSummaryIndex {
 public:
  static StatusOr<WpaSummaryIndex> Build(
      std::span<const summary::SummaryArtifact> summaries);

  bool Covers(std::span<const summary::SummaryArtifact> summaries) const;
  const summary::SummaryArtifact* Lookup(core::StableId function_id) const;
  bool Contains(core::StableId function_id) const;

 private:
  const summary::SummaryArtifact* source_data_ = nullptr;
  std::size_t source_size_ = 0;
  std::map<core::StableId, const summary::SummaryArtifact*> by_function_;
};
```

Include `<cstddef>`, `<map>`, and `<span>` directly in the header. Implement
`Build` by parsing `summary::Identity(artifact).function_variant_id()` once,
requiring `IdKind::kFunctionVariant`, and rejecting duplicate map insertion.
`Covers` compares both `data()` and `size()`.

Add the overload:

```cpp
static StatusOr<WpaLogicalComponentInput> Build(
    const WpaMaterializationRequest& request,
    const WpaSummaryIndex& summaries);
```

The one-argument overload builds a local index and delegates. The indexed
overload validates `summaries.Covers(request.summaries)` and replaces local
`by_function.find`/`contains` calls with `Lookup`/`Contains`.

- [ ] **Step 4: Build one index in `WpaOrchestrator::Run`**

Immediately after the SCC graph succeeds, add:

```cpp
auto summary_index = WpaSummaryIndex::Build(request.summaries);
if (!summary_index.ok()) {
  repository_.MarkIncomplete(request.run);
  return summary_index.status();
}
```

Inside the component loop call:

```cpp
auto logical = WpaInputMaterializer::Build(materialization, *summary_index);
```

Do not change component order, expected component enumeration, successor
support, cache descriptors, executor selection, or result canonicalization.

- [ ] **Step 5: Run materializer and orchestrator tests**

Run:

```bash
cmake --build --preset default \
  --target WpaInputMaterializerTest WpaOrchestratorTest WpaEndToEndTest
ctest --test-dir build --output-on-failure \
  -R 'WpaInputMaterializerTest|WpaOrchestratorTest|WpaEndToEndTest'
```

Expected: all indexed/fallback equivalence, component ordering, cache,
successor-support, and failure-atomicity tests pass.

- [ ] **Step 6: Commit summary-index reuse**

```bash
git add include/veritas/wpa/WpaInputMaterializer.h \
  src/wpa/WpaInputMaterializer.cpp src/wpa/WpaOrchestrator.cpp \
  tests/unit/wpa/WpaInputMaterializerTest.cpp
git commit -m "perf(wpa): reuse the summary index across components"
```

---

### Task 3: Consume WPA results into a single canonical fact batch

**Files:**

- Modify: `include/veritas/wpa/WpaOrchestrator.h:50-64`
- Modify: `src/wpa/WpaOrchestrator.cpp:190-286`
- Modify: `include/veritas/facts/AnalysisFactBus.h:25-66`
- Modify: `src/facts/AnalysisFactBus.cpp:138-223`
- Modify: `src/analysis/ProjectAnalyzer.cpp:260-335`
- Modify: `tests/unit/facts/AnalysisFactBusTest.cpp:203-281`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp:203-251`

**Interfaces:**

- Consumes: one successful `wpa::WpaRunResult`, passed by move in production.
- Produces: `AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result)`
  whose component completions retain metadata/hashes but no duplicate result
  payload vectors.

- [ ] **Step 1: Add a failing consuming-assembly regression test**

Refactor the existing duplicate-proof setup in `AnalysisFactBusTest` into a
small `DuplicateProofRun()` helper, then add:

```cpp
TEST(AnalysisFactBusTest,
     ConsumesComponentPayloadIntoCanonicalBatchVectors) {
  auto run = DuplicateProofRun();
  const auto expected = MakeAnalysisFactBatch(run);
  auto consumed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(consumed.batch_id, expected.batch_id);
  EXPECT_EQ(consumed.facts, expected.facts);
  EXPECT_EQ(consumed.witnesses, expected.witnesses);
  ASSERT_FALSE(consumed.completed_components.empty());
  for (const auto& completion : consumed.completed_components) {
    EXPECT_TRUE(completion.result.facts.empty());
    EXPECT_TRUE(completion.result.witnesses.empty());
    EXPECT_TRUE(completion.result.diagnostics.empty());
    EXPECT_FALSE(completion.result.logical_input_hash.empty());
    EXPECT_FALSE(completion.result.fixpoint_hash.empty());
    EXPECT_FALSE(completion.result.external_hash.empty());
  }
}
```

- [ ] **Step 2: Run the regression and verify it fails**

Run:

```bash
cmake --build --preset default --target AnalysisFactBusTest
./build/tests/unit/facts/AnalysisFactBusTest \
  --gtest_filter=AnalysisFactBusTest.ConsumesComponentPayloadIntoCanonicalBatchVectors
```

Expected: the test fails because the current const-reference assembler copies
full result payloads into `batch.completed_components`.

- [ ] **Step 3: Remove redundant flattened vectors from `WpaRunResult`**

Delete `facts`, `witnesses`, and `diagnostics` from `WpaRunResult`. In
`WpaOrchestrator::Run`, delete the block that appends each component result to
those vectors. Keep `completed_components`, rooted inputs, scheduled
predecessors, and `completed_facts` for successor support unchanged.

Update `WpaEndToEndTest` to construct `AnalysisFactBatch` and assert on
`batch.facts` and `batch.witnesses`; do not add a second flattening helper.

- [ ] **Step 4: Change batch assembly to consume by value**

Change the declaration and definition to:

```cpp
AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result);
```

Move run metadata into `AnalysisFactBatch`, sort completions by key, and walk
them with mutable references. Use these private temporary records:

```cpp
struct KeyedFact {
  std::string key;
  AnalysisFact fact;
};

struct KeyedWitness {
  std::string result_key;
  std::string rule_id;
  std::string input_key;
  std::uint32_t input_ordinal = 0;
  WitnessEdge edge;
};
```

For each component, encode every fact row once. Move the first owned fact into
`KeyedFact`; place duplicate result keys in that component's `overridden` set.
Encode each witness result/input once and move only non-overridden derivations
into `KeyedWitness`. Move component diagnostics into batch diagnostics, then
clear the component's three payload vectors.

Sort `KeyedFact` by `key`. Sort `KeyedWitness` lexicographically by
`(result_key, rule_id, input_key, input_ordinal)`. Move the ordered payloads
into `batch.facts` and `batch.witnesses`, preserving the existing uniqueness
and `DeriveBatchId` steps.

- [ ] **Step 5: Move the production run into assembly**

In `RunWpa`, keep conformance comparison before publication and replace:

```cpp
auto batch = facts::MakeAnalysisFactBatch(*wpa_result);
```

with:

```cpp
auto batch = facts::MakeAnalysisFactBatch(std::move(*wpa_result));
```

The `wpa_run_id` must already be copied into `ProjectAnalysisResult` before
this move. Do not access `wpa_result` afterward.

- [ ] **Step 6: Run fact-bus and end-to-end tests**

Run:

```bash
cmake --build --preset default \
  --target AnalysisFactBusTest WpaEndToEndTest ProjectAnalyzerWpaTest \
  FactStoreTest ProvenanceStoreTest
ctest --test-dir build --output-on-failure \
  -R 'AnalysisFactBusTest|WpaEndToEndTest|ProjectAnalyzerWpaTest|FactStoreTest|ProvenanceStoreTest'
```

Expected: all tests pass; reversed completion order still produces the same
facts, witnesses, and `BatchId`; publication validation and explanation
persistence remain intact.

- [ ] **Step 7: Commit single-owner batch assembly**

```bash
git add include/veritas/wpa/WpaOrchestrator.h src/wpa/WpaOrchestrator.cpp \
  include/veritas/facts/AnalysisFactBus.h src/facts/AnalysisFactBus.cpp \
  src/analysis/ProjectAnalyzer.cpp tests/unit/facts/AnalysisFactBusTest.cpp \
  tests/integration/wpa/WpaEndToEndTest.cpp
git commit -m "perf(facts): consume WPA results during batch assembly"
```

---

### Task 4: Prove semantic equivalence and performance acceptance

**Files:**

- Verify: all files changed in Tasks 1-3
- Verify: `docs/specs/veritas-build-analyze-performance-design-spec.md`
- Verify: `docs/plans/veritas-build-analyze-performance-implementation-plan.md`

**Interfaces:**

- Consumes: the optimized `veritas-build` and the existing LevelDB checkout at
  `/Users/skg7on/Workspace/Projects/leveldb`.
- Produces: fresh targeted/full verification evidence and a machine-specific
  LevelDB timing/RSS result attached to issue #133 or the implementation PR.

- [ ] **Step 1: Run the targeted semantic regression suite**

```bash
cmake --build --preset default \
  --target HashTest IdsTest WpaInputMaterializerTest WpaOrchestratorTest \
  AnalysisFactBusTest WpaEndToEndTest WpaExecutorConformanceTest \
  ProjectAnalyzerWpaTest FactStoreTest ProvenanceStoreTest
ctest --test-dir build --output-on-failure \
  -R 'HashTest|IdsTest|WpaInputMaterializerTest|WpaOrchestratorTest|AnalysisFactBusTest|WpaEndToEndTest|WpaExecutorConformanceTest|ProjectAnalyzerWpaTest|FactStoreTest|ProvenanceStoreTest'
```

Expected: every selected test passes with zero skips and the conformance test
executes rather than reporting `GTEST_SKIP`.

- [ ] **Step 2: Run a clean full build and full test suite**

```bash
rm -rf build
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
ctest --test-dir build --output-on-failure
```

Expected: configuration and build exit 0; CTest reports 100% passed with no
crashes, timeouts, failures, or skips.

- [ ] **Step 3: Run the executable qualification and no-skips gates**

```bash
python3 tests/qualification/M9EntryGateTest.py
ctest --test-dir build -L wpa-qualification --no-tests=error \
  --output-on-failure --output-junit build/wpa-qualification.xml
python3 tests/qualification/check_no_skips.py \
  build/wpa-qualification.xml --label wpa-qualification
python3 tools/check_m9_entry.py --build-dir build
```

Expected: the gate unit tests pass, all five WPA qualification aggregates run
without missing, disabled, or skipped tests, and all ten exact M9 criteria
pass. Also inspect the full CTest log for `SKIP` and `Not Run`; either string
blocks completion.

- [ ] **Step 4: Run the LevelDB acceptance benchmark**

Create a fresh, issue-specific output directory and run:

```bash
VERITAS_BENCH_OUTPUT=/tmp/veritas-leveldb-optimized-issue-133
test ! -e "$VERITAS_BENCH_OUTPUT"
mkdir "$VERITAS_BENCH_OUTPUT"
/usr/bin/time -lp ./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output "$VERITAS_BENCH_OUTPUT"
```

If that issue-specific directory already exists, choose a new explicit
issue-specific name and rerun the existence check; never reuse an old result
directory. Capture the full command output and `/usr/bin/time` statistics in
the issue or PR report.

Expected:

- exit code 0;
- wall time at most 375 seconds;
- peak resident memory at most 4 GB;
- 3,429 SCCs and 13,716 completed WPA components for the unchanged fixture;
- a non-empty WPA run ID and successfully published facts;
- no semantic truncation introduced by the optimization.

- [ ] **Step 5: Run repository hygiene checks**

```bash
git diff --check main...HEAD
git status --porcelain
git diff --stat main...HEAD
```

Run the exact license-header verification snippet from
`.claude/rules/license-header-policy.md`. Review `git diff main...HEAD` and
confirm it contains only the performance change, tests, spec, plan, and index
links.

Expected: no whitespace errors, missing headers, unrelated changes, or
uncommitted files.

- [ ] **Step 6: Commit any benchmark-report documentation update**

If the implementation records benchmark results in the design specification,
commit only that evidence:

```bash
git add docs/specs/veritas-build-analyze-performance-design-spec.md
git commit -m "docs(perf): record analyze benchmark results"
```

If no repository file changed after the preceding commit, skip this commit.

---

## Plan Self-Review Record

- **Spec coverage:** Tasks 1-3 implement all four root-cause remedies; Task 4
  covers semantic, qualification, memory, timing, formatting, licensing, and
  cleanliness acceptance criteria.
- **Completeness scan:** All code-changing steps provide exact files, symbols,
  code shapes, commands, and expected outcomes.
- **Type consistency:** `WpaSummaryIndex`, both `Build` overloads,
  `MakeAnalysisFactBatch(wpa::WpaRunResult)`, `KeyedFact`, and `KeyedWitness`
  are named consistently across producers, consumers, and tests.
- **Scope check:** Soufflé batching, parallel execution, SVF changes, schema
  changes, and CLI changes remain outside this plan.
