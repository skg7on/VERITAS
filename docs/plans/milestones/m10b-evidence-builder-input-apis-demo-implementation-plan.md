# M10 Evidence Builder Input APIs and First Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose Evidence Builder input APIs and produce the first compact memory-safety evidence slice.

**Architecture:** Build `EvidenceQueryService` over M6 CPG queries and M9 fact/provenance stores. Return semantic slices with flow paths, supporting facts, unknowns, provenance refs, source anchors, and explicit truncation.

**Tech Stack:** C++20, JSON diagnostic output, CPG query layer, FactStore, ProvenanceStore, GoogleTest.

**Spec:** `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md`

## Global Constraints

- M10 does not implement full Evidence IR serialization.
- A slice is not proof by itself.
- Missing evidence is represented as unknown.
- Summary edges are marked expandable.
- Source text is referenced, not copied by default.

---

### Task 1: Evidence Slice Types

**Files:**
- Create: `include/veritas/evidence/SliceTypes.h`
- Create: `src/evidence/SliceTypes.cpp`
- Test: `tests/unit/evidence/SliceTypesTest.cpp`

**Interfaces:**
- Produces: `FlowSlice`
- Produces: `EvidenceQueryBudget`
- Produces: deterministic diagnostic JSON serialization

- [ ] **Step 1: Write serialization test**

```cpp
TEST(SliceTypesTest, SerializesDeterministically) {
  FlowSlice slice = MakeSmallFlowSlice();
  EXPECT_EQ(ToDiagnosticJson(slice), ToDiagnosticJson(slice));
}
```

- [ ] **Step 2: Define slice structs**

Include nodes, edges, supporting facts, contradicting facts, unknowns, provenance refs, and truncation.

- [ ] **Step 3: Implement JSON serializer**

Keep field order stable and include truncation explicitly.

- [ ] **Step 4: Run slice tests**

Run: `ctest --test-dir build -R SliceTypesTest --output-on-failure`

Expected: pass.

---

### Task 2: Evidence Query Service

**Files:**
- Create: `include/veritas/evidence/EvidenceQueryService.h`
- Create: `src/evidence/EvidenceQueryService.cpp`
- Test: `tests/unit/evidence/EvidenceQueryServiceTest.cpp`

**Interfaces:**
- Produces: `EvidenceQueryService::GetValueFlow`
- Produces: `GetRanges`, `GetAliases`, `GetUnknowns`, `GetDominatingChecks`, `Explain`
- Consumes: `cpg::CpgQuery`
- Consumes: `facts::FactStore`
- Consumes: `facts::ProvenanceStore`

- [ ] **Step 1: Write service test with fakes**

Use fake CPG and fake FactStore to assert `GetValueFlow` returns path edges plus supporting facts.

- [ ] **Step 2: Implement constructor injection**

Inject CPG query, FactStore, and ProvenanceStore dependencies.

- [ ] **Step 3: Implement value-flow query**

Call `CpgQuery::GetValueFlow`, attach supporting facts, and preserve budget truncation.

- [ ] **Step 4: Implement fact lookup methods**

Implement ranges, aliases, unknowns, and dominating checks through FactStore queries.

- [ ] **Step 5: Run service tests**

Run: `ctest --test-dir build -R EvidenceQueryServiceTest --output-on-failure`

Expected: pass.

---

### Task 3: Overflow Demo Fixtures

**Files:**
- Create: `tests/fixtures/cpp/overflow/packet.h`
- Create: `tests/fixtures/cpp/overflow/unsafe_packet.cpp`
- Create: `tests/fixtures/cpp/overflow/safe_packet.cpp`
- Test: `tests/integration/evidence/OverflowEvidenceFixtureTest.cpp`

**Interfaces:**
- Produces: unsafe and safe evidence fixture inputs

- [ ] **Step 1: Add unsafe fixture**

```cpp
void copy_payload(Packet* p, Buffer* b) {
  memcpy(b->data, p->payload, p->length);
}
```

- [ ] **Step 2: Add safe fixture**

```cpp
void copy_payload(Packet* p, Buffer* b) {
  if (p->length <= b->capacity) {
    memcpy(b->data, p->payload, p->length);
  }
}
```

- [ ] **Step 3: Write fixture integration test**

Index both fixtures and assert the unsafe fixture has value flow to `memcpy.size`, while the safe fixture has a dominating check fact.

- [ ] **Step 4: Run fixture test**

Run: `ctest --test-dir build -R OverflowEvidenceFixtureTest --output-on-failure`

Expected: pass.

---

### Task 4: Evidence Query CLI

**Files:**
- Modify: `src/tools/veritas-query.cpp`
- Test: `tests/integration/evidence/VeritasQueryEvidenceTest.cpp`

**Interfaces:**
- Produces: `veritas-query evidence overflow --sink memcpy --format json`

- [ ] **Step 1: Write CLI golden test**

Run the evidence command and compare output to `tests/golden/evidence/overflow_unsafe.json`.

- [ ] **Step 2: Implement CLI parsing**

Support `--sink`, `--format json`, `--max-depth`, and `--max-paths`.

- [ ] **Step 3: Emit JSON diagnostic output**

Include claim seed, source value, sink callsite, flow path, range facts, capacity facts, dominating checks, unknowns, provenance refs, and truncation.

- [ ] **Step 4: Run CLI test**

Run: `ctest --test-dir build -R VeritasQueryEvidenceTest --output-on-failure`

Expected: pass.

---

### Task 5: Milestone Verification

- [ ] **Step 1: Run M10 tests**

Run: `ctest --test-dir build -R "SliceTypes|EvidenceQueryService|OverflowEvidence|VeritasQueryEvidence" --output-on-failure`

- [ ] **Step 2: Run demo command**

Run:

```bash
./build/src/tools/veritas-query evidence overflow \
  --sink memcpy \
  --format json \
  --max-depth 8 \
  --max-paths 5
```

- [ ] **Step 3: Commit**

```bash
git add include/veritas/evidence src/evidence src/tools/veritas-query.cpp tests/fixtures/cpp/overflow tests/unit/evidence tests/integration/evidence tests/golden/evidence
git commit -m "feat: add evidence query slices"
```

