# M6 Thin VERITAS CPG Projection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a thin function- and object-centric CPG projection over SummaryDB data.

**Architecture:** Store typed graph nodes and adjacency indexes using VERITAS stable IDs. Treat the graph as a query projection, not the source of truth. Keep instruction-level detail out of persistent V1 storage.

**Tech Stack:** C++20, Protobuf, SQLite or RocksDB adjacency indexes, GoogleTest.

**Spec:** `docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md`

## Global Constraints

- The persistent graph is not full compiler IR.
- Node IDs are VERITAS stable IDs.
- Summary edges must be marked expandable.
- Unknown calls must not create full graph fanout.
- No Joern runtime dependency is introduced in V1.

---

### Task 1: CPG Schema and In-Memory Graph

**Files:**
- Create: `proto/veritas/cpg/v1/cpg.proto`
- Create: `include/veritas/cpg/ThinCpg.h`
- Create: `src/cpg/ThinCpg.cpp`
- Test: `tests/unit/cpg/ThinCpgTest.cpp`

**Interfaces:**
- Produces: `CpgNode`
- Produces: `CpgEdge`
- Produces: `ThinCpg::AddNode`, `ThinCpg::AddEdge`

- [ ] **Step 1: Write node and edge deduplication tests**

```cpp
TEST(ThinCpgTest, DeduplicatesFunctionNodes) {
  ThinCpg graph;
  graph.AddNode(FunctionNode("funcvar:sha256:abc"));
  graph.AddNode(FunctionNode("funcvar:sha256:abc"));
  EXPECT_EQ(graph.NodeCount(), 1);
}
```

- [ ] **Step 2: Define CPG Protobuf**

Include node kind, edge kind, IDs, revision/build context, source anchor refs, and expandable summary edge flag.

- [ ] **Step 3: Implement in-memory graph**

Provide deterministic insertion and edge deduplication.

- [ ] **Step 4: Run unit tests**

Run: `ctest --test-dir build -R ThinCpgTest --output-on-failure`

Expected: pass.

---

### Task 2: CPG Builder from Summaries

**Files:**
- Create: `include/veritas/cpg/CpgBuilder.h`
- Create: `src/cpg/CpgBuilder.cpp`
- Test: `tests/integration/cpg/CpgBuilderTest.cpp`

**Interfaces:**
- Consumes: current summary bindings
- Produces: `CpgBuilder::BuildForRevision`

- [ ] **Step 1: Write builder test**

Publish synthetic summaries with calls, reads, writes, and flows. Build CPG and assert expected nodes and edges.

- [ ] **Step 2: Implement summary traversal**

Read current summaries for a revision/build and map components into CPG nodes and edges.

- [ ] **Step 3: Emit summary edges**

Mark interprocedural `FLOWS_TO` edges with `summarized_by` and `expandable = true`.

- [ ] **Step 4: Run builder test**

Run: `ctest --test-dir build -R CpgBuilderTest --output-on-failure`

Expected: pass.

---

### Task 3: Query API and CLI

**Files:**
- Create: `include/veritas/cpg/CpgQuery.h`
- Create: `src/cpg/CpgQuery.cpp`
- Test: `tests/unit/cpg/CpgQueryTest.cpp`
- Modify: `src/tools/veritas-query.cpp`

**Interfaces:**
- Produces: `CpgQuery::GetCallees`
- Produces: `CpgQuery::GetCallers`
- Produces: `CpgQuery::GetWriters`
- Produces: `CpgQuery::GetValueFlow`

- [ ] **Step 1: Write query tests**

```cpp
TEST(CpgQueryTest, TraversesValueFlowWithBudget) {
  auto path = query.GetValueFlow(src, dst, 4);
  EXPECT_FALSE(path.empty());
  EXPECT_FALSE(path[0].truncated);
}
```

- [ ] **Step 2: Implement adjacency indexes**

Index outgoing edges by `(source_node_id, edge_kind)` and incoming edges by `(target_node_id, edge_kind)`.

- [ ] **Step 3: Implement budgeted traversal**

Return explicit truncation when `max_depth`, `max_nodes`, or `max_paths` is exceeded.

- [ ] **Step 4: Add CLI commands**

Support:

```text
veritas-query callees <function>
veritas-query flow <src> <dst> --max-depth <n>
```

- [ ] **Step 5: Run query tests**

Run: `ctest --test-dir build -R CpgQueryTest --output-on-failure`

Expected: pass.

---

### Task 4: Milestone Verification

- [ ] **Step 1: Run CPG tests**

Run: `ctest --test-dir build -R "ThinCpg|CpgBuilder|CpgQuery" --output-on-failure`

- [ ] **Step 2: Check graph scope**

Run: `rg -n "Instruction|llvm::Instruction" include/veritas/cpg src/cpg`

Expected: no persistent instruction-node API.

- [ ] **Step 3: Commit**

```bash
git add proto/veritas/cpg include/veritas/cpg src/cpg src/tools/veritas-query.cpp tests/unit/cpg tests/integration/cpg
git commit -m "feat: add thin CPG projection"
```

