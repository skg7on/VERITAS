# M7 Reverse Dependency Index and Incremental Scheduler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Schedule precise recomputation from summary component deltas through a reverse dependency index.

**Architecture:** Compute `SummaryDelta` and `ComponentDelta` from old/new summary component hashes, maintain a current reverse dependency index, and feed deduplicated work items into a scheduler.

**Tech Stack:** C++20, SQLite, GoogleTest.

**Spec:** `docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md`

## Global Constraints

- Do not invalidate all callers when a component-level dependency is available.
- Semantic changes and evidence-only changes schedule different consumers.
- Reverse index reconciliation is transactional with summary publication.
- Historical dependency rows remain explainable.
- Worklist items are deduplicated by semantic target.

---

### Task 1: Summary Delta Model

**Files:**
- Create: `include/veritas/summarydb/SummaryDelta.h`
- Create: `src/summarydb/SummaryDelta.cpp`
- Test: `tests/unit/summarydb/SummaryDeltaTest.cpp`

**Interfaces:**
- Produces: `ComponentDelta`
- Produces: `SummaryDelta`
- Produces: `DiffSummaries`

- [ ] **Step 1: Write delta tests**

```cpp
TEST(SummaryDeltaTest, DetectsRangeOnlySemanticChange) {
  auto delta = DiffSummaries(MakeRangeSummary(10), MakeRangeSummary(20));
  EXPECT_CHANGED_ONLY(delta, ComponentKind::RangeFacts);
}
```

- [ ] **Step 2: Implement `DiffSummaries`**

Compare semantic and evidence hashes from M3 component rows.

- [ ] **Step 3: Add evidence-only test**

Change provenance refs only and assert `semantic_changed = false`, `evidence_changed = true`.

- [ ] **Step 4: Run delta tests**

Run: `ctest --test-dir build -R SummaryDeltaTest --output-on-failure`

Expected: pass.

---

### Task 2: Reverse Dependency Index

**Files:**
- Create: `include/veritas/summarydb/DependencyIndex.h`
- Create: `src/summarydb/DependencyIndex.cpp`
- Modify: `src/summarydb/schema/v1.sql`
- Test: `tests/unit/summarydb/DependencyIndexTest.cpp`

**Interfaces:**
- Produces: `DependencyIndex::ReplaceCurrentDependencies`
- Produces: `DependencyIndex::UsersOf`
- Produces: `DependencyIndex::GetImpactSet`

- [ ] **Step 1: Write reverse lookup test**

Insert a dependency from `decode.value_flow` to `validate.range` and assert lookup by `validate.range` returns only `decode.value_flow`.

- [ ] **Step 2: Add schema tables**

Add `summary_dependencies`, `reverse_dependency_index`, `summary_deltas`, and `component_deltas`.

- [ ] **Step 3: Implement replacement**

Remove old current rows for the consumer and insert new current rows in one transaction.

- [ ] **Step 4: Add historical preservation test**

Assert old summary dependency rows remain readable after current index replacement.

- [ ] **Step 5: Run dependency tests**

Run: `ctest --test-dir build -R DependencyIndexTest --output-on-failure`

Expected: pass.

---

### Task 3: Worklist Scheduler

**Files:**
- Create: `include/veritas/runtime/WorklistScheduler.h`
- Create: `src/runtime/WorklistScheduler.cpp`
- Test: `tests/unit/runtime/WorklistSchedulerTest.cpp`

**Interfaces:**
- Produces: `WorkItem`
- Produces: `WorklistScheduler::Enqueue`
- Produces: `WorklistScheduler::PopNext`

- [ ] **Step 1: Write deduplication test**

```cpp
TEST(WorklistSchedulerTest, DeduplicatesSameSemanticTarget) {
  WorklistScheduler scheduler;
  scheduler.Enqueue(MakeRangeWork("decode"));
  scheduler.Enqueue(MakeRangeWork("decode"));
  EXPECT_EQ(scheduler.PendingCount(), 1);
}
```

- [ ] **Step 2: Implement scheduler**

Deduplicate by kind, target ID, revision ID, build variant ID, and consumer component.

- [ ] **Step 3: Add priority ordering test**

Assert local summaries run before dependent WPA items when both are present.

- [ ] **Step 4: Run scheduler tests**

Run: `ctest --test-dir build -R WorklistSchedulerTest --output-on-failure`

Expected: pass.

---

### Task 4: Diff CLI

**Files:**
- Modify: `src/tools/veritas-diff.cpp`
- Test: `tests/integration/summarydb/VeritasDiffTest.cpp`

**Interfaces:**
- Consumes: `SummaryDelta`
- Consumes: `DependencyIndex`
- Produces: `veritas-diff <old> <new>`

- [ ] **Step 1: Write CLI output test**

Create synthetic old/new summaries and assert `veritas-diff` prints changed components and scheduled consumers.

- [ ] **Step 2: Implement diff command**

Print source changed functions, summary changed functions, changed components, and affected consumers.

- [ ] **Step 3: Run diff test**

Run: `ctest --test-dir build -R VeritasDiffTest --output-on-failure`

Expected: pass.

---

### Task 5: Milestone Verification

- [ ] **Step 1: Run M7 tests**

Run: `ctest --test-dir build -R "SummaryDelta|DependencyIndex|WorklistScheduler|VeritasDiff" --output-on-failure`

- [ ] **Step 2: Commit**

```bash
git add include/veritas/summarydb include/veritas/runtime src/summarydb src/runtime src/tools/veritas-diff.cpp tests/unit/summarydb tests/unit/runtime tests/integration/summarydb
git commit -m "feat: add incremental dependency scheduler"
```
