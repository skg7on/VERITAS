# M8 SCC-Aware WPA and Souffle Fact Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compute whole-program facts through SCC-aware fixpoint propagation and Souffle-backed recursive relations.

**Architecture:** Build the call graph from current summaries, collapse recursion into SCCs, run monotone C++ fixpoint domains first, then export normalized tuple-ID relations to Souffle. Keep Souffle output as an execution artifact and publish VERITAS facts through M9.

**Tech Stack:** C++20, SQLite, optional Souffle, GoogleTest.

**Spec:** `docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md`

## Global Constraints

- `UNKNOWN_CALL` must not connect to every function.
- SCC state includes fixpoint hash, external hash, iteration count, and status.
- Domains are monotone or explicitly bounded.
- Derived tuples carry tuple IDs and provenance inputs.
- Souffle disabled builds still run C++ fixpoint tests.

---

### Task 1: Call Graph and SCC Builder

**Files:**
- Create: `include/veritas/wpa/CallGraph.h`
- Create: `include/veritas/wpa/SccGraph.h`
- Create: `src/wpa/CallGraph.cpp`
- Create: `src/wpa/SccGraph.cpp`
- Test: `tests/unit/wpa/SccGraphTest.cpp`

**Interfaces:**
- Produces: `CallGraph`
- Produces: `SccGraph::Build`
- Produces: `SccGraph::SccForFunction`

- [ ] **Step 1: Write SCC tests**

```cpp
TEST(SccGraphTest, BuildsMutualRecursionAsOneScc) {
  CallGraph graph;
  graph.AddMayCall("A", "B");
  graph.AddMayCall("B", "A");
  auto scc = SccGraph::Build(graph);
  EXPECT_EQ(scc.SccForFunction("A"), scc.SccForFunction("B"));
}
```

- [ ] **Step 2: Implement call graph loader**

Load `MUST_CALL` and `MAY_CALL` edges from current summaries.

- [ ] **Step 3: Implement SCC algorithm**

Use Tarjan or Kosaraju. Keep member order deterministic.

- [ ] **Step 4: Add unknown-call test**

Assert `UNKNOWN_CALL` produces an unknown external marker and does not create edges to all functions.

- [ ] **Step 5: Run SCC tests**

Run: `ctest --test-dir build -R SccGraphTest --output-on-failure`

Expected: pass.

---

### Task 2: C++ Fixpoint Engine

**Files:**
- Create: `include/veritas/wpa/FixpointEngine.h`
- Create: `include/veritas/wpa/FixpointDomain.h`
- Create: `src/wpa/FixpointEngine.cpp`
- Create: `src/wpa/FixpointDomain.cpp`
- Test: `tests/unit/wpa/FixpointEngineTest.cpp`

**Interfaces:**
- Produces: `FixpointEngine::Compute`
- Produces: `SccResult`
- Produces: domains for transitive calls and may-write

- [ ] **Step 1: Write transitive call fixpoint test**

Graph A calls B, B calls C. Assert A's transitive call set contains B and C.

- [ ] **Step 2: Write may-write fixpoint test**

Function A calls B, B writes memory X. Assert `MayWrite(A, X)`.

- [ ] **Step 3: Implement set-union domains**

Start with transitive calls, may-read, and may-write.

- [ ] **Step 4: Persist SCC state**

Add tables for SCCs, members, edges, and component state.

- [ ] **Step 5: Add external hash stop test**

Change an internal fact without changing the external SCC hash and assert predecessors are not scheduled.

- [ ] **Step 6: Run fixpoint tests**

Run: `ctest --test-dir build -R FixpointEngineTest --output-on-failure`

Expected: pass.

---

### Task 3: Souffle Exporter

**Files:**
- Create: `include/veritas/facts/FactSchema.h`
- Create: `include/veritas/facts/SouffleExporter.h`
- Create: `src/facts/FactSchema.cpp`
- Create: `src/facts/SouffleExporter.cpp`
- Create: `src/facts/rules/reachability.dl`
- Create: `src/facts/rules/memory_effects.dl`
- Test: `tests/integration/facts/SouffleExporterTest.cpp`

**Interfaces:**
- Produces: `FactTuple`
- Produces: `SouffleExporter::WriteBaseRelations`
- Produces: `SouffleExporter::ReadDerivedRelations`

- [ ] **Step 1: Write relation export test**

Export a tiny call graph and assert `DirectCall.facts` contains tuple ID, caller, callee, and epistemic field.

- [ ] **Step 2: Implement base relation writer**

Write TSV or Souffle-compatible fact files with deterministic row order.

- [ ] **Step 3: Add reachability rules**

Create `reachability.dl` with tuple IDs and epistemic fields.

- [ ] **Step 4: Add memory effect rules**

Create `memory_effects.dl` for direct and transitive `MayWrite`.

- [ ] **Step 5: Implement derived relation reader**

Read Souffle output into `FactTuple` values.

- [ ] **Step 6: Run Souffle exporter tests**

Run: `ctest --test-dir build -R SouffleExporterTest --output-on-failure`

Expected: pass when Souffle is enabled; skip with explicit message when disabled.

---

### Task 4: Milestone Verification

- [ ] **Step 1: Run C++ WPA tests**

Run: `ctest --test-dir build -R "SccGraph|FixpointEngine" --output-on-failure`

- [ ] **Step 2: Run Souffle tests when enabled**

Run: `ctest --test-dir build -R SouffleExporter --output-on-failure`

- [ ] **Step 3: Commit**

```bash
git add include/veritas/wpa include/veritas/facts src/wpa src/facts tests/unit/wpa tests/integration/facts
git commit -m "feat: add SCC WPA fact engine"
```

