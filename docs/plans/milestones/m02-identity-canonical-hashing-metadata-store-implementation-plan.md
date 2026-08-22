# M2 Identity, Canonical Hashing, and Metadata Store Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement stable VERITAS IDs, canonical hash inputs, and the first SQLite metadata schema.

**Architecture:** Build one canonicalization and hashing library used by every later subsystem. Store repository, revision, build variant, translation unit, analyzer, source-anchor, and function-identity tables through a single `MetadataStore` abstraction.

**Tech Stack:** C++20, SQLite, SHA-256 provider selected in M0, GoogleTest.

**Spec:** `docs/specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md`

## Global Constraints

- Source line numbers are diagnostics, not semantic identity.
- Absolute local paths are rejected from semantic hash inputs unless tagged as external roots.
- Every ID string includes kind, algorithm, and digest.
- Metadata inserts must be idempotent.
- Public APIs return `veritas::Status` or `veritas::StatusOr<T>`.

---

### Task 1: Stable ID and Canonical Encoding

**Files:**
- Create: `include/veritas/core/Hash.h`
- Create: `include/veritas/core/Ids.h`
- Create: `include/veritas/core/CanonicalValue.h`
- Create: `src/core/Hash.cpp`
- Create: `src/core/Ids.cpp`
- Create: `src/core/CanonicalValue.cpp`
- Test: `tests/unit/core/HashTest.cpp`
- Test: `tests/unit/core/IdsTest.cpp`
- Test: `tests/unit/core/CanonicalValueTest.cpp`

**Interfaces:**
- Produces: `veritas::core::StableId`
- Produces: `MakeStableId(IdKind, bytes)`
- Produces: `CanonicalEncode(CanonicalValue)`

- [ ] **Step 1: Write canonicalization tests**

```cpp
TEST(CanonicalValueTest, SortsMapKeys) {
  auto left = Object({{"b", String("2")}, {"a", String("1")}});
  auto right = Object({{"a", String("1")}, {"b", String("2")}});
  EXPECT_EQ(CanonicalEncode(left), CanonicalEncode(right));
}
```

- [ ] **Step 2: Write ID round-trip tests**

```cpp
TEST(IdsTest, RoundTripsStableIdText) {
  auto id = MakeStableId(IdKind::Repository, Bytes("repo"));
  EXPECT_EQ(ParseStableId(ToString(id)).value(), id);
}
```

- [ ] **Step 3: Run focused tests**

Run: `ctest --test-dir build -R "HashTest|IdsTest|CanonicalValueTest" --output-on-failure`

Expected: fail because the APIs are missing.

- [ ] **Step 4: Implement canonical encoding**

Support null, bool, integer, string, array, object, and tagged external path values.

- [ ] **Step 5: Implement stable IDs**

Use ID strings like `repo:sha256:<digest>`. Reject malformed strings with `InvalidArgument`.

- [ ] **Step 6: Run focused tests again**

Expected: pass.

---

### Task 2: SQLite Schema and Metadata Store

**Files:**
- Create: `include/veritas/summarydb/MetadataStore.h`
- Create: `src/summarydb/MetadataStore.cpp`
- Create: `src/summarydb/schema/v1.sql`
- Test: `tests/unit/summarydb/MetadataStoreTest.cpp`

**Interfaces:**
- Produces: `MetadataStore::Open`
- Produces: `MetadataStore::ApplySchema`
- Produces: put methods for repository, revision, build variant, translation unit, analyzer run

- [ ] **Step 1: Write schema creation test**

```cpp
TEST(MetadataStoreTest, AppliesSchemaToFreshDatabase) {
  auto store = MetadataStore::Open(temp_db()).value();
  EXPECT_TRUE(store.ApplySchema().ok());
}
```

- [ ] **Step 2: Run metadata tests**

Run: `ctest --test-dir build -R MetadataStoreTest --output-on-failure`

Expected: fail because metadata store is missing.

- [ ] **Step 3: Add `v1.sql`**

Create tables named in the design spec: repositories, revisions, build_variants, translation_units, analyzer_runs, analysis_configurations, source_anchors, function_symbols, function_variants, function_bodies.

- [ ] **Step 4: Implement SQLite wrapper**

Open database, apply schema transactionally, and expose idempotent insert helpers.

- [ ] **Step 5: Add duplicate insert tests**

Insert the same repository and revision twice; assert one logical row.

- [ ] **Step 6: Run metadata tests again**

Expected: pass.

---

### Task 3: M1 Manifest Persistence

**Files:**
- Modify: `include/veritas/summarydb/MetadataStore.h`
- Modify: `src/summarydb/MetadataStore.cpp`
- Test: `tests/integration/summarydb/ManifestPersistenceTest.cpp`

**Interfaces:**
- Consumes: `veritas::build::AnalysisManifest`
- Produces: `MetadataStore::PutManifestContext(const AnalysisManifest&)`

- [ ] **Step 1: Write manifest persistence test**

Load the M1 smoke manifest and store it twice. Assert repository, revision, build variant, and translation unit rows exist once.

- [ ] **Step 2: Implement `PutManifestContext`**

Use one transaction for all rows from a manifest.

- [ ] **Step 3: Add rollback test**

Inject a bad translation unit row and assert no partial manifest context was committed.

- [ ] **Step 4: Run integration test**

Run: `ctest --test-dir build -R ManifestPersistenceTest --output-on-failure`

Expected: pass.

---

### Task 4: Milestone Verification

**Files:**
- Modify: none

**Interfaces:**
- Consumes: all M2 APIs
- Produces: verified M2 metadata foundation

- [ ] **Step 1: Run M2 tests**

Run: `ctest --test-dir build -R "Hash|Ids|CanonicalValue|MetadataStore|ManifestPersistence" --output-on-failure`

- [ ] **Step 2: Commit**

```bash
git add include/veritas/core include/veritas/summarydb src/core src/summarydb tests/unit/core tests/unit/summarydb tests/integration/summarydb
git commit -m "feat: add stable identity metadata"
```

