# M10B Evidence Builder Input APIs and First Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose Evidence Builder input APIs and produce the first compact,
completeness-aware memory-safety input for M10C.

**Architecture:** Build `EvidenceQueryService` over M6 CPG queries and M9
fact/provenance stores. Return semantic slices with flow paths, supporting
facts, unknowns, provenance refs, source anchors, and explicit truncation.
Bundle all query results into one typed, immutable `EvidenceBuildInput`; M10C
consumes that value directly instead of parsing diagnostic JSON.

**Tech Stack:** C++20, JSON diagnostic output, CPG query layer, FactStore, ProvenanceStore, GoogleTest.

**Spec:** `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md`

## Global Constraints

- M10B does not implement the Evidence IR semantic model or serialization;
  M10C owns that boundary.
- A slice is not proof by itself.
- Missing evidence is represented as unknown.
- Summary edges are marked expandable.
- Source text is referenced, not copied by default.
- Every fact lookup distinguishes complete-empty from truncated-empty output.

---

### Task 1: Evidence Slice Types

**Files:**
- Create: `include/veritas/evidence/SliceTypes.h`
- Create: `src/evidence/SliceTypes.cpp`
- Create: `src/evidence/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/evidence/SliceTypesTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `FlowSlice`
- Produces: `EvidenceQueryBudget`
- Produces: `ClaimKind`, `Severity`, `ClaimSeed`, `QueryCompleteness`, and `EvidenceFactSet`
- Produces: `EvidenceBuildInput`
- Produces: deterministic diagnostic JSON serialization

- [ ] **Step 1: Write serialization test**

```cpp
TEST(SliceTypesTest, SerializesDeterministically) {
  EvidenceBuildInput input = MakeSmallEvidenceBuildInput();
  EXPECT_EQ(ToDiagnosticJson(input), ToDiagnosticJson(input));
}
```

- [ ] **Step 2: Define slice structs**

Include nodes, edges, supporting facts, contradicting facts, unknowns,
provenance refs, and truncation. Add `ClaimSeed`, `QueryCompleteness`,
`EvidenceFactSet`, and `EvidenceBuildInput` exactly as specified by the M10B
design. Include capacity results and the complete provenance graph in the
immutable input. Add invalid-sentinel `ClaimKind` and `Severity` enums with
stable `ToString` and rejecting parse helpers; M10C will reuse them directly.

- [ ] **Step 3: Implement JSON serializer**

Keep field order stable and include truncation and per-query completeness
explicitly. Add a test proving complete-empty and truncated-empty fact sets
serialize differently.

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
- Produces: `GetRanges`, `GetCapacities`, `GetAliases`, `GetUnknowns`, `GetDominatingChecks`, `Explain`
- Produces: `EvidenceQueryService::BuildEvidenceInput`
- Consumes: `cpg::CpgQuery`
- Consumes: `facts::FactStore`
- Consumes: `facts::ProvenanceStore`

- [ ] **Step 1: Write service test with fakes**

Use fake CPG and fake FactStore to assert `GetValueFlow` returns path edges plus
supporting facts. Add capacity and completeness cases, including complete-empty
and truncated-empty results.

- [ ] **Step 2: Implement constructor injection**

Inject CPG query, FactStore, and ProvenanceStore dependencies.

- [ ] **Step 3: Implement value-flow query**

Call `CpgQuery::GetValueFlow`, attach supporting facts, and preserve budget truncation.

- [ ] **Step 4: Implement fact lookup methods**

Implement ranges, capacities, aliases, unknowns, and dominating checks through
FactStore queries. Every method returns `EvidenceFactSet`, including explicit
completeness and truncation reasons.

- [ ] **Step 5: Implement the typed M10C handoff**

Implement `BuildEvidenceInput` by executing the bounded queries once, retaining
the supplied `ClaimSeed`, and bundling the flow, all fact sets, and provenance
graph. Do not perform EIR validation, level projection, or serialization here.

- [ ] **Step 6: Run service tests**

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

Include claim seed, source value, sink callsite, flow path, range facts,
capacity facts, dominating checks, unknowns, provenance refs, truncation, and
per-query completeness. This remains M10B slice JSON; do not emit EIR-T,
Protobuf, or full-EIR JSON.

- [ ] **Step 4: Run CLI test**

Run: `ctest --test-dir build -R VeritasQueryEvidenceTest --output-on-failure`

Expected: pass.

---

### Task 5: M10B Milestone Verification

- [ ] **Step 1: Run M10B tests**

Run: `ctest --test-dir build -R "SliceTypes|EvidenceQueryService|OverflowEvidence|VeritasQueryEvidence" --output-on-failure`

- [ ] **Step 2: Run demo command**

Run:

```bash
./build/bin/veritas-query evidence overflow \
  --sink memcpy \
  --format json \
  --max-depth 8 \
  --max-paths 5
```

Verify that the output carries one typed claim seed, the capacity result, the
provenance graph, and explicit completeness for every fact lookup. Confirm that
the same service result is available as `EvidenceBuildInput` without JSON
round-tripping.

- [ ] **Step 3: Verify the M10C handoff contract**

Cross-check the public types against
`docs/specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md`
section 5. The M10B public header must expose every value M10C consumes, while
remaining free of EIR model and codec types.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt include/veritas/evidence src/evidence \
  src/tools/veritas-query.cpp tests/fixtures/cpp/overflow \
  tests/unit/evidence tests/integration/evidence tests/golden/evidence
git commit -m "feat: add evidence query slices"
```
