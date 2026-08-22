# M9 Provenance-Aware Fact Store and Explain API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store current facts and provenance DAGs, then expose a budgeted `veritas-explain fact` command.

**Architecture:** Publish M8 fact tuples into SQLite with exact `FactID` plus cross-revision `semantic_fact_hash`. Store derivation nodes and edges as a provenance DAG and expand explanations with explicit budget truncation.

**Tech Stack:** C++20, Protobuf, SQLite, GoogleTest.

**Spec:** `docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md`

## Global Constraints

- Every non-trivial derived fact has provenance.
- Epistemic state is separate from confidence.
- `INFERRED` inputs cannot become `MUST` without verifier evidence.
- Historical facts remain readable after current fact replacement.
- Explanation truncation must be explicit.

---

### Task 1: Fact and Provenance Protobuf

**Files:**
- Create: `proto/veritas/fact/v1/fact.proto`
- Create: `include/veritas/facts/Epistemic.h`
- Create: `src/facts/Epistemic.cpp`
- Test: `tests/unit/facts/EpistemicTest.cpp`

**Interfaces:**
- Produces: `EpistemicState`
- Produces: `JoinEpistemic`
- Produces: fact and provenance Protobuf messages

- [ ] **Step 1: Write epistemic tests**

```cpp
TEST(EpistemicTest, InferredDoesNotBecomeMust) {
  EXPECT_EQ(JoinEpistemic(EpistemicState::Inferred,
                          EpistemicState::Must,
                          RuleSoundness::Sound),
            EpistemicState::Inferred);
}
```

- [ ] **Step 2: Define `fact.proto`**

Include `Fact`, `ProvenanceNode`, `ProvenanceEdge`, `ExplainBudget`, and `ProvenanceGraph`.

- [ ] **Step 3: Implement epistemic joins**

Follow the M9 design spec's conservative propagation rules.

- [ ] **Step 4: Run epistemic tests**

Run: `ctest --test-dir build -R EpistemicTest --output-on-failure`

Expected: pass.

---

### Task 2: Fact Store

**Files:**
- Create: `include/veritas/facts/FactStore.h`
- Create: `src/facts/FactStore.cpp`
- Modify: `src/summarydb/schema/v1.sql`
- Test: `tests/unit/facts/FactStoreTest.cpp`

**Interfaces:**
- Produces: `FactStore::PublishFacts`
- Produces: `FactStore::GetFactsBySubject`
- Produces: `FactID` and `semantic_fact_hash`

- [ ] **Step 1: Write fact publication test**

Publish a fact, replace it with a new current fact, and assert the historical fact remains readable.

- [ ] **Step 2: Add fact schema**

Add fields from the M9 design spec, including `semantic_fact_hash` and `is_current`.

- [ ] **Step 3: Implement fact hashing**

Compute `FactID` with revision/build/provenance and `semantic_fact_hash` without revision/provenance.

- [ ] **Step 4: Implement current replacement**

Mark old current row non-current and insert new row transactionally.

- [ ] **Step 5: Run fact store tests**

Run: `ctest --test-dir build -R FactStoreTest --output-on-failure`

Expected: pass.

---

### Task 3: Provenance Store and Explain API

**Files:**
- Create: `include/veritas/facts/ProvenanceStore.h`
- Create: `src/facts/ProvenanceStore.cpp`
- Test: `tests/unit/facts/ProvenanceStoreTest.cpp`

**Interfaces:**
- Produces: `ProvenanceStore::PutNode`
- Produces: `ProvenanceStore::PutEdge`
- Produces: `ProvenanceStore::Explain`

- [ ] **Step 1: Write budgeted explanation test**

Build a chain of provenance nodes longer than the budget. Assert the returned graph includes a truncation marker.

- [ ] **Step 2: Add provenance schema**

Add `provenance_nodes` and `provenance_edges` tables.

- [ ] **Step 3: Implement insertion APIs**

Deduplicate nodes and edges by ID.

- [ ] **Step 4: Implement recursive explanation**

Traverse input edges until `max_depth` or `max_nodes` is reached.

- [ ] **Step 5: Run provenance tests**

Run: `ctest --test-dir build -R ProvenanceStoreTest --output-on-failure`

Expected: pass.

---

### Task 4: Explain CLI

**Files:**
- Modify: `src/tools/veritas-explain.cpp`
- Test: `tests/integration/facts/VeritasExplainTest.cpp`

**Interfaces:**
- Consumes: `FactStore`
- Consumes: `ProvenanceStore`
- Produces: `veritas-explain fact <fact_id>`

- [ ] **Step 1: Write CLI test**

Create a small fact/provenance database and assert `veritas-explain fact` prints Fact, Epistemic, Producer, Rule, Inputs, and Source anchors sections.

- [ ] **Step 2: Implement CLI parsing**

Support `--max-depth` and `--max-nodes`.

- [ ] **Step 3: Implement text output**

Print explicit truncation notice when the graph is budget-limited.

- [ ] **Step 4: Run CLI test**

Run: `ctest --test-dir build -R VeritasExplainTest --output-on-failure`

Expected: pass.

---

### Task 5: Milestone Verification

- [ ] **Step 1: Run M9 tests**

Run: `ctest --test-dir build -R "Epistemic|FactStore|ProvenanceStore|VeritasExplain" --output-on-failure`

- [ ] **Step 2: Commit**

```bash
git add proto/veritas/fact include/veritas/facts src/facts src/tools/veritas-explain.cpp tests/unit/facts tests/integration/facts
git commit -m "feat: add provenance fact store"
```

