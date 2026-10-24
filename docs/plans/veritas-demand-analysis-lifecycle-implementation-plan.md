# Demand Analysis and Lifecycle Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline or
> superpowers:subagent-driven-development when selected. P2/P3 implementation is
> conditional on P1 evidence and review of these versioned contracts.

**Goal:** Make semantic work, retained facts and repeated-review cost follow the
admitted question and invalidation cone while preserving checkable evidence.
**Architecture:** Add indexed reads, scoped execution/artifacts and staged
publication beside native M9; then add dependency-aware reuse and GC.
**Tech Stack:** C++20, SQLite additive migrations, RocksDB CAS, pinned Soufflé/
SVF precision workers, GoogleTest/CTest, existing M7/M13 instruments.
**Spec:** [demand analysis/lifecycle](../specs/veritas-demand-analysis-lifecycle-design-spec.md).

**Work issues:** [RDX12 / #166](https://github.com/skg7on/VERITAS/issues/166), [RDX13 / #167](https://github.com/skg7on/VERITAS/issues/167), [RDX14 / #168](https://github.com/skg7on/VERITAS/issues/168), [RDX15 / #169](https://github.com/skg7on/VERITAS/issues/169).

## Global constraints and review focus

No partial query may impersonate a whole-run receipt. Keep baseline native IDs
and conformance; version new scope, artifacts and publication. Preserve all
required premises and epistemic weakening. Do not repeat the reverted arena's
decode/render tax or remove identity verification for speed. Respect #143's
full native dependencies and #125's portability gate.

Review focus: wrong-snapshot cursors (Task 1), oversized recursive scopes (Task 2),
lost witness premises (Task 3), crash/current-binding races (Task 4), negative
dependency additions and SCC topology changes (Task 5), live-reader GC (Task 6).

## Task 1: Add indexed snapshot-scoped fact pages

**Files:** Modify `include/veritas/evidence/EvidenceReadBackend.h`,
`FactStoreEvidenceBackend.h`, `include/veritas/facts/FactStore.h`,
`src/facts/FactStore.cpp`; add the derived `fact_lookup.v1.db` index/backfill and
`tests/integration/facts/FactPageTest.cpp`.
**Consumes:** pinned native read snapshots and validated semantic cells.
**Produces:** `ReadFacts(const FactSelector&, const PageRequest&) const ->
StatusOr<FactPage>`, typed selectors/cursors and `fact_lookup_v1` index.

- [ ] Write `RejectsCursorFromAnotherSnapshotOrSelector`,
  `ExactLimitDoesNotMeanTruncated`, `CollisionDoesNotReturnWrongValue`, and
  `UnrelatedRowsDoNotIncreaseExaminedCount`; assert indexed work stays fixed
  when 10x unrelated rows are added and the probe row is not returned.
  Add sparse-intersection and all-collision pages with `max_examined=3`; assert
  at most three candidates are examined, an empty incomplete page has a last-
  consumed continuation, and the next page makes progress without rescanning.
  Two pages with lookahead must have disjoint returned members whose union is
  the complete expected set: an examined-but-unreturned probe cannot disappear.
- [ ] Build/run `ctest --test-dir build -R FactPageTest --output-on-failure`;
  demonstrate RED on current whole-run reads.
- [ ] Implement typed value indexes, collision validation, keyset cursor binding,
  positive row/work/byte limits and atomic auxiliary backfill/version behavior.
  Do not consume native schema-v5 for a rebuildable index. Keep the legacy API for
  baseline callers; migrate Evidence fact slots to the scoped path.
- [ ] Verify real evidence queries, Explain budgets, backfill interruption and
  legacy-store refusal/progress. Require all new page tests pass.
- [ ] Commit with index recovery and documented old-store behavior. M13 compares
  unchanged authoritative metadata tables; validate the derived index separately
  instead of masking semantic columns in its existing projection.

## Task 2: Plan immutable closed and open semantic scopes

**Files:** Create `include/veritas/query/QueryTypes.h`,
`src/query/{QueryTypes,QueryPlanner}.cpp`, `include/veritas/query/QueryPlanner.h`,
and `tests/unit/query/QueryPlannerTest.cpp`; register CMake targets.
**Consumes:** ReviewBinding/ProgramScope, dependency snapshot and requested refs.
**Produces:** `PlanQuery(const QueryDescriptor&, const DependencySnapshot&) ->
StatusOr<QueryPlan>`, QueryId and explicit SCC/frontier selection.

- [ ] Test `DoesNotCutOversizedScc`, `UnresolvedCallbackKeepsFrontierOpen`,
  `BudgetsAndProducerVersionsMoveQueryIdentity`, `TimingDoesNotMoveQueryIdentity`,
  and a local closed query with disconnected functions. Require INCOMPLETE,
  not a negative fact, for an SCC exceeding 64 functions or 8 SCCs.
- [ ] Run `ctest --test-dir build -R QueryPlannerTest --output-on-failure` for RED.
- [ ] Implement deterministic relevance selection and conservative boundary
  coverage. Define `boundary-summary.v1` identity/reader separately from summary.v2.
- [ ] Verify shared memory, callback/indirect targets, source/config changes,
  wrong ID kinds and exact budget boundaries; compare planned dependencies.
- [ ] Commit scoped identity and coverage before implementing a new executor.

## Task 3: Execute query-seeded kernels with rooted proofs

**Files:** Create `include/veritas/query/QueryExecutor.h`,
`src/query/{QueryExecutor,ProofSelector}.cpp`, `logic/query/` registered bundles,
and `tests/integration/query/QueryExecutionTest.cpp`; adapt existing WPA/SVF
executor interfaces without replacing baseline bundles.
**Consumes:** Task 2 QueryPlan, boundary summaries, registered producer config.
**Produces:** `ExecuteQuery(const QueryPlan&, QueryExecutor&) ->
StatusOr<QueryArtifact>`, selected proof DAG and coverage outcome.

- [ ] Write `ScopedAnswersMatchClosedBaseline`, `MissingPremisePreventsSeal`,
  `RecursiveTimeoutIsIncomplete`, `UnrelatedCodeDoesNotExpandFixedQuery`, and
  `WorkerDeathDoesNotPublish`. Assert exact tuple/epistemic/proof-root agreement
  on supported closed fixtures, not just equal tuple counts.
- [ ] Run QueryExecutionTest for RED and record baseline cost with the same build.
- [ ] Implement seeded kernels, bounded worker ownership and canonical finite
  proof selection. Keep global SVF bootstrap explicit in cost/coverage metadata.
- [ ] Run reorderings, dense recursion, external boundaries, corruption and
  10x-disconnected-code controls; require no hidden all-pairs precomputation.
- [ ] Commit the registered capability only for the predicates it qualifies.

## Task 4: Stage, seal and atomically publish query artifacts

**Files:** Create `include/veritas/query/QueryRepository.h`,
`src/query/QueryRepository.cpp`, schema-v5 migration and
`tests/integration/query/QueryPublicationTest.cpp`; use existing object store.
**Consumes:** validated QueryDescriptor/artifact chunks and metadata transactions.
**Produces:** `BeginQuery`, `AppendChunk`, `SealQuery`, `PublishQuery` signatures
and tables exactly as specified in the lifecycle design.

- [ ] Write `CrashAtEveryTransitionExposesNoPartialCurrent`,
  `WrongChunkOrderOrDigestCannotSeal`, `RepeatPublicationIsIdempotent`, and
  `StaleGenerationCannotReplaceCurrent`. Inspect reopened stores, not in-memory
  flags; incomplete chunks must remain unreachable from complete current reads.
  Add two snapshots of one registered question sharing a slot and two distinct
  questions sharing no slot; changing budget does not let weaker coverage answer
  a complete request. Verify staging ownership before final ArtifactId exists.
- [ ] Run QueryPublicationTest for RED before adding schema/publication code.
- [ ] Implement 4096-row/4-MiB chunking, bounded spill, closure validation and
  generation-CAS publication; leave native whole-run receipts unchanged.
- [ ] Verify restart recovery, simultaneous publishers, oversized row refusal,
  v4/v5 compatibility and aggregate memory including validation/spill.
- [ ] Commit the migration plus explicit rollback/export/read compatibility.

## Task 5: Make invalidation and Evidence reuse operational

**Files:** Modify `src/wpa/{WpaOrchestrator,WpaRunRepository,SccStateRepository}.cpp`,
the M7 scheduler and native tests; create `src/query/EvidenceCache.cpp`,
`include/veritas/query/EvidenceCache.h`, `tests/integration/query/EvidenceReuseTest.cpp`.
**Consumes:** dependency manifests, component hashes and sealed artifacts.
**Produces:** `Revalidate(const QueryArtifact&, const DependencySnapshot&) ->
ReuseDecision`, durable deduplicated native work and explicit freshness states.

- [ ] Write no-op, one-function edit, new caller/guard, model/doc change,
  SCC split/merge/removal and corrupt-cache controls. Assert zero semantic
  executions on no-op; relevant edits match a clean recomputation; additions
  invalidate negative answers even if their old positive premise set is unchanged.
- [ ] Run EvidenceReuseTest and WpaOrchestratorTest for RED; record executed,
  reused, revalidated and published counts independently.
- [ ] Implement review dependency revalidation and #143's separately qualified
  durable worklist. Keep full native identity prerequisites and corruption
  policing explicit; do not claim exact-snapshot reuse proves portable reuse.
- [ ] Verify crash/restart and race cases against clean recomputation, then run
  M13 baseline-equivalence checks where semantics are unchanged.
- [ ] Commit each existing issue's work under its acceptance contract; do not
  close #143 because only the review-level cache works.

## Task 6: Bound physical growth and preserve retained proof roots

**Files:** Modify `src/summarydb/ObjectStoreRocksDb.cpp` under #144; create
`src/query/ArtifactRetention.cpp`, corresponding header and
`tests/integration/query/ArtifactRetentionTest.cpp`.
**Consumes:** current/retained report manifests, reader epochs and staging leases.
**Produces:** bounded unchanged writes, rooted mark/sweep and byte accounting.

- [ ] Add concurrent equal/different-content put-if-absent tests; repeated no-op
  publications must not append payload operands. Add `ReaderLeaseProtectsProof`,
  `RetainedReportPinsTransitiveInputs`, and restart-between-mark/sweep tests.
- [ ] Run object-store and ArtifactRetentionTest for RED before changing Merge.
- [ ] Implement verified unchanged-write avoidance and epoch/lease-aware GC.
  Preserve collision refusal and active/retained roots; reclaim abandoned staging.
- [ ] Measure logical/table/index/WAL/SST/spill bytes before/after compaction and
  GC; compare aggregate RSS and query answers/proofs as well as disk totals.
- [ ] Run the full qualification/build/test gates and commit an acceptance record
  with #144 and Evidence-lifecycle completion assessed independently.
