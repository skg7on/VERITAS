# M3 Summary IR and Immutable CAS Object Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Define Function Summary IR and store immutable summaries with component hashes and atomic current bindings.

**Architecture:** Represent summaries in Protobuf, compute canonical summary and component hashes, write immutable bytes into RocksDB through an `ObjectStore` interface, and publish current bindings through SQLite metadata transactions.

**Tech Stack:** C++20, Protobuf, RocksDB, SQLite, GoogleTest.

**Spec:** `docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md`

## Global Constraints

- Summary objects are immutable.
- Current summary selection is a metadata binding.
- Component semantic hashes and evidence hashes are separate.
- Historical summaries remain readable after rebinding.
- M3 uses synthetic summaries; real extraction begins in M4.

---

### Task 1: Summary Protobuf and Component Types

**Files:**
- Create: `proto/veritas/summary/v1/summary.proto`
- Create: `include/veritas/summary/FunctionSummary.h`
- Create: `include/veritas/summary/ComponentHash.h`
- Create: `src/summary/FunctionSummary.cpp`
- Create: `src/summary/ComponentHash.cpp`
- Test: `tests/unit/summary/ComponentHashTest.cpp`

**Interfaces:**
- Produces: `veritas::summary::v1::FunctionSummary`
- Produces: `ComputeComponentDigests`
- Produces: `ComputeFunctionSummaryId`

- [ ] **Step 1: Write component hash tests**

```cpp
TEST(ComponentHashTest, RangeOnlyChangeChangesOnlyRangeDigest) {
  auto before = MakeSyntheticSummaryWithRange(0, 1024);
  auto after = MakeSyntheticSummaryWithRange(0, 2048);
  EXPECT_CHANGED_ONLY(before, after, ComponentKind::RangeFacts);
}
```

- [ ] **Step 2: Run summary tests**

Run: `ctest --test-dir build -R ComponentHashTest --output-on-failure`

Expected: fail because Protobuf and hash code are missing.

- [ ] **Step 3: Define `summary.proto`**

Include header, identity, component digests, calls, memory effects, value flows, ranges, aliases, unknowns, dependencies, and provenance refs.

- [ ] **Step 4: Wire Protobuf generation in CMake**

Generate C++ sources and link them into `veritas_summary`.

- [ ] **Step 5: Implement component digest computation**

Hash canonical bytes for each component separately.

- [ ] **Step 6: Run summary tests again**

Expected: pass.

---

### Task 2: RocksDB ObjectStore

**Files:**
- Create: `include/veritas/summarydb/ObjectStore.h`
- Create: `src/summarydb/ObjectStoreRocksDb.cpp`
- Create: `tests/unit/summarydb/ObjectStoreTest.cpp`

**Interfaces:**
- Produces: `ObjectStore::PutIfAbsent`
- Produces: `ObjectStore::Get`
- Produces: in-memory fake for tests

- [ ] **Step 1: Write CAS behavior tests**

```cpp
TEST(ObjectStoreTest, PutIfAbsentDeduplicatesByKey) {
  InMemoryObjectStore store;
  EXPECT_TRUE(store.PutIfAbsent("summary:sha256:abc", Bytes("one")).ok());
  EXPECT_TRUE(store.PutIfAbsent("summary:sha256:abc", Bytes("one")).ok());
  EXPECT_EQ(store.ObjectCount(), 1);
}
```

- [ ] **Step 2: Implement `ObjectStore` interface and fake**

Use the fake in unit tests before RocksDB is wired.

- [ ] **Step 3: Implement RocksDB-backed store**

Return `Internal` on corrupt or mismatched bytes.

- [ ] **Step 4: Run object store tests**

Run: `ctest --test-dir build -R ObjectStoreTest --output-on-failure`

Expected: pass.

---

### Task 3: SummaryRepository Publication

**Files:**
- Create: `include/veritas/summarydb/SummaryRepository.h`
- Create: `src/summarydb/SummaryRepository.cpp`
- Modify: `src/summarydb/schema/v1.sql`
- Test: `tests/unit/summarydb/SummaryRepositoryTest.cpp`

**Interfaces:**
- Produces: `SummaryRepository::PublishSummary`
- Produces: `SummaryRepository::GetSummary`
- Produces: summary_objects, summary_components, summary_bindings tables

- [ ] **Step 1: Write publication test**

Publish two summaries for the same function variant and assert the second is current while the first remains readable.

- [ ] **Step 2: Add schema tables**

Add `summary_objects`, `summary_components`, and `summary_bindings`.

- [ ] **Step 3: Implement transactional publication**

Write object first, then publish metadata in one transaction.

- [ ] **Step 4: Add injected failure test**

Fail after object write and before metadata commit. Assert no current binding was created.

- [ ] **Step 5: Run repository tests**

Run: `ctest --test-dir build -R SummaryRepositoryTest --output-on-failure`

Expected: pass.

---

### Task 4: Milestone Verification

**Files:**
- Modify: none

**Interfaces:**
- Consumes: all M3 APIs
- Produces: verified immutable summary storage

- [ ] **Step 1: Run M3 tests**

Run: `ctest --test-dir build -R "ComponentHash|ObjectStore|SummaryRepository" --output-on-failure`

- [ ] **Step 2: Commit**

```bash
git add proto/veritas/summary include/veritas/summary include/veritas/summarydb src/summary src/summarydb tests/unit/summary tests/unit/summarydb
git commit -m "feat: add immutable summary store"
```

