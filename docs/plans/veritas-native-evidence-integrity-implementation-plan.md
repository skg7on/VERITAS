# Native Evidence Integrity Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for inline execution,
> or superpowers:subagent-driven-development when explicitly selected. Track the
> checkbox steps; delivered foundations are not instructions to recreate them.

**Goal:** Close the native authority, ownership, coverage and handoff gaps that
would otherwise invalidate a new review consumer.
**Architecture:** Refine existing validation and projection boundaries; add
versioned coverage/summary handoff data without changing native engine ownership.
**Tech Stack:** C++20, Clang/LLVM 22+, required pinned SVF/Soufflé, GoogleTest,
CMake 3.23+, `Status`/`StatusOr`, existing Protobuf/EIR codecs.
**Spec:** [native integrity](../specs/veritas-native-evidence-integrity-design-spec.md).

**Work issues:** [RDX01 / #155](https://github.com/skg7on/VERITAS/issues/155), [RDX02 / #156](https://github.com/skg7on/VERITAS/issues/156), [RDX03 / #157](https://github.com/skg7on/VERITAS/issues/157), [RDX04 / #158](https://github.com/skg7on/VERITAS/issues/158).

## Global constraints and review focus

No RTTI/exceptions; Apache-2.0 headers on all authored code. Preserve native
`summary.v2`/`relations.v2` IDs for unchanged valid facts and `eir.v1` grammar.
Use a dedicated worktree, RED/GREEN regression evidence, full clean build and
CTest without hidden skips before every push. Do not weaken #124's producer
oracles or silently mask corrected CPG/Evidence identities.

Review focus: relation-valid but forbidden epistemic states (Task 1); foreign
flow endpoints mistaken for ownership (Task 2); absent versus unexamined scope
(Task 3); provenance truncated before its root (Task 4); stale summary bindings
and vacuous universal domains (Tasks 5–6). Each receives a discriminating test.

## Task 1: Enforce relation authority on stable and dense rows

**Files:** Modify `src/facts/AnalysisFact.cpp` and
`tests/unit/facts/AnalysisFactTest.cpp`; also `src/wpa/WpaInputMaterializer.cpp`,
`src/analysis/ProjectAnalyzer.cpp`, `tests/unit/wpa/WpaInputMaterializerTest.cpp`,
`tests/unit/wpa/SouffleSessionTest.cpp`, and `tests/unit/evidence/EvidenceHandoffTest.cpp`
for the upstream producer/policy identity and invalid-fixture correction.
The existing registry stays authoritative.
**Consumes:** `RelationSchema::allowed_epistemic`, existing domain validation.
**Produces:** unchanged `ValidateSemanticRow`, `ValidateExecutionRow`, `MakeFact`
and `DeriveFactId` signatures, with invalid-state refusal.

- [x] Write `UnknownCallRejectsStatesItsProducerCannotEstablish`,
  `ExecutionRowsEnforceTheSameUnknownCallAuthority`,
  `ExistentialFlowCannotStateMustNot`, and
  `CoverageRejectsPossibleAndNegativeCertificates`. Literal oracles: UnknownCall
  accepts MAY/UNKNOWN only; ReachableCall rejects MUST_NOT; coverage accepts
  MUST/UNKNOWN only. Assert `InvalidArgument` at actual public gates, including
  `MakeFact`/`DeriveFactId`; do not merely compare two registry lookups.
- [x] Build `AnalysisFactTest`, run the four named cases; observed 4 failures
  against the baseline because forbidden states were admitted.
- [x] Check relation admission after cell-domain validation at both stable and
  dense boundaries; return an error without coercing the state.
- [x] Reproduce the upstream unresolved-call failure with
  `UnresolvedTargetsDoNotInheritDefiniteCallAuthority`; conservatively project
  producer states, advance the WPA config identity and replace the invalid
  negative-flow fixture with admitted Alias evidence. Targeted run: 42/42 pass.
- [x] Run targeted fact/proto/registry/WPA checks and the full native suite;
  require all tests pass and valid identity goldens stay exact.
- [x] Complete final clean build (655 steps) and CTest (904/904, 339.90 s, zero
  failures/errors/skips); commit this runtime slice with its regressions.

Task 1 was merged in #171. Task 2 is now implemented below; Tasks 3–6 remain
tracked work and are not claimed complete by these checked steps.

## Task 2: Produce compiler-owned containment and exact callsites

**Files:** Create `src/analysis/cpg/EntityOwnership.{h,cpp}`; modify
`src/analysis/cpg/CpgProjectionStage.{h,cpp}`, OriginMap extraction, the local
value/callsite identity producer, and their CMake target lists. Tests:
`tests/integration/analysis/cpg/CpgProjectionStageTest.cpp` and
`tests/integration/evidence/VeritasQueryEvidenceTest.cpp`.
**Execution amendment:** persistence/canonicalization (`CpgTypes`, `ThinCpg`,
`CpgRepository`, `CpgCanonicalizer`), optional exact CLI/seed context
(`veritas-query`, `SliceTypes`, `EvidenceQueryService`) and real two-caller
fixtures are required to make compiler bindings survive the public query. This
is an additive CPG extension, not the Task 3 coverage-sidecar migration. Existing
StableValueMapper encodings are retained; capture a pointer-free CompilerProjection
in ProjectAnalyzer's M4 span and reuse it after SVF normalization. Recomputing
callsite/operand identities from post-SVF instructions is forbidden.

**Consumes:** live `ProgramIr` and OriginMap; no guessed ownership from summary
flow endpoints. **Produces:** `CollectEntityOwners(const ProgramIr&) ->
StatusOr<std::vector<EntityOwner>>`, `thin-cpg.v2` containment and callsite bindings.

- [x] Add `OwnsLocalValuesButNotForeignFlowEndpoints`: local value has exactly
  its compiler owner, imported/global endpoint has no invented owner. Add
  `TwoCallersKeepDistinctMemcpyArguments` with two external functions; inserting
  an unrelated lower-sorting caller must not change existing seed operands.
- [x] Run `ctest --test-dir build -R 'CpgProjectionStageTest|VeritasQueryEvidenceTest' --output-on-failure`;
  demonstrate missing containment/callsite-local resolution on baseline.
- [x] Implement the owner collector and caller-local argument-index binding.
  Emit `kContains` only for declared uniquely owned local entities; reject
  conflicts. Version the projection producer; keep old projection readers.
- [x] Run the same tests plus `CpgCanonicalizerTest`; require deterministic
  reorderings, explicit old-projection coverage and no whole-program exit guess.
- [x] Review intentional projection/golden changes, update only explained
  expectations, and commit without enabling producer-gated multi-claim fan-out.

Task 2 regression record: missing ownership/calls, lost persisted bindings,
external-exit-dependent seed selection, foreign caller operands, incomplete
bindings, missing CLI selection and constant-extent scope were observed RED.
Runtime commit `3c636e2` passes a final 656-step clean build and full CTest
908/908 in 340.16 s, with zero failures/errors/skips including internal GoogleTest
skip inspection. The program plan records the remaining scope and review minor.

## Task 3: Make unsupported/open/truncated coverage explicit

**Files:** Create `include/veritas/evidence/QueryCoverage.h`,
`src/evidence/QueryCoverage.cpp`; modify `SliceTypes.h`, `EvidenceQueryService.cpp`,
`EvidenceCaseBuilder.cpp` and evidence CMake lists. Test existing
`EvidenceContractTest.cpp`, `EvidenceCaseBuilderTest.cpp`, and
`tests/integration/evidence/EvidenceHandoffIntegrationTest.cpp`.
**Consumes:** completion FactIDs and immutable read snapshot. **Produces:**
`QueryCoverage`, `ValidateQueryCoverage(const QueryCoverage&) -> Status`,
versioned handoff digest and claim-scoped unknown/omission members.

- [x] Add table-driven `UnsupportedEmptyIsNotNegative`,
  `CompleteOpenScopeRemainsUnknown`, `TruncatedEmptyRetainsReason`, and
  `MissingOwnerIsUnresolvedScope`; assert concrete EIR unknown/omission members
  and forbid `MUST_NOT` for unsupported/open/truncated inputs.
- [x] Run `ctest --test-dir build -R 'EvidenceContractTest|EvidenceCaseBuilderTest|EvidenceHandoffIntegrationTest' --output-on-failure`;
  observe the current incomplete/empty handoff loses distinctions.
- [x] Implement the exact truth table in the spec and bind coverage to the
  completion ID/digest. Treat absent legacy sidecars explicitly; do not change
  existing completion-fact cells under their old IDs.
- [x] Run those tests plus EIR codecs; require L0/L1/L2 preserve the new omissions
  and single-snapshot refusal remains effective.
- [x] Commit the coverage amendment, its schema/producer version and reviewed
  fixture changes together.

Task 3 execution: versioned coverage/digest, native conservative support profile,
explicit unresolved ownership, claim-scoped gaps and legacy admission are
implemented. RED reproduced false native absence, missing flow truncation and
unresolved scope; five coverage/builder regressions failed before the gate.
The reviewed native artifacts remove the unsupported negative derivation and
retain all real flow facts. Evidence/codec suite: 412/412 passing, including
L0/L1/L2 truth-table round trips, unchanged completion rows, mixed-run/snapshot
refusal and the existing single-snapshot retry contract. Final clean build,
whole-suite verification: 572-step clean `homebrew-clang` build and 926/926
CTest tests in 323.72 s, with zero failures/errors/skips, including internal
GoogleTest skip inspection. Independent review's malformed-row hashing and
confidence-precision findings have four RED→GREEN regressions; semantic rows
are checked before hashing, and modern confidence is finite and round-trip safe.
Legacy diagnostic formatting remains intact.

The acceptance run follows CI's serial CTest command. A parallel experiment
exposed the existing shared temporary database in `CpgQueryTest::OpenQuery`;
`ReportsCallees` passed ten serial repeats and the complete serial suite. The
parallel test harness needs isolated temporary paths; no test was disabled.

## Task 4: Carry fact provenance with enforced depth and member limits

**Files:** Modify `EvidenceQueryService.cpp`, `FactStoreEvidenceBackend.h`,
`EvidenceReadBackend.h`, and evidence integration tests; reuse
`src/facts/ProvenanceStore.cpp` rather than implement another traversal.
**Consumes:** `EvidenceReadSnapshot::Explain(fact_id, ExplainBudget)`.
**Produces:** immutable union of selected fact/certificate provenance plus
explicit per-fact frontier and aggregate truncation in `EvidenceBuildInput`.

- [ ] Add `DepthOneAndTwoYieldDifferentRootedFrontiers` and
  `CompletionWitnessDoesNotReplaceFactWitness`; use a real three-level rooted
  derivation, budgets 1 and 2, and assert the exact missing premise IDs.
- [ ] Run `ctest --test-dir build -R 'EvidenceHandoffIntegrationTest|ProvenanceStoreTest' --output-on-failure`;
  show that the existing assembly ignores the requested depth.
- [ ] Pass depth/member limits into actual Explain calls, canonical-union the
  returned graphs and retain each truncated frontier. Never query a newer run.
- [ ] Verify exact-boundary completion, repeated shared premises, cycles/errors,
  and a backend whose binding changes midway; require explicit error/incomplete.
- [ ] Commit the handoff and tests with no unbounded per-fact accumulation.

## Task 5: Supply real summary references in the immutable handoff

**Files:** Modify `SliceTypes.h`, `EvidenceReadBackend.h`,
`FactStoreEvidenceBackend.h`, `EvidenceQueryService.cpp`, `EvidenceCaseBuilder.cpp`;
test `EvidenceHandoffIntegrationTest.cpp`, `VeritasQueryEirTest.cpp`.
**Consumes:** current selected `summary::SummaryArtifact` plus component hashes.
**Produces:** `ReadSummaries(function_refs)` on the pinned backend and actual
EIR `SummaryReference` mappings; no extra database read in M10C.

- [ ] Add `L2CarriesCurrentSummaryComponents` and `RejectsStaleSummaryBinding`;
  assert the real summary ID/component hashes, and forbid summary-copy expansion
  or an old run's artifact masquerading as current.
- [ ] Run `ctest --test-dir build -R 'EvidenceHandoffIntegrationTest|VeritasQueryEirTest' --output-on-failure`;
  baseline emits only `summary not supplied` omissions.
- [ ] Extend immutable assembly, validate the reference set, and map L2 members.
  L0/L1 retain expansion handles and omissions when no summary is available.
- [ ] Verify source/summary reordering preserves identity and a semantic
  component change moves it; rerun codec round trips.
- [ ] Commit without falsely marking the remaining #124 producers delivered.

## Task 6: Qualify the repaired native boundary

**Files:** Update the M10B/M10C executable test contract and focused native
fixture/golden records; record results in the redesign program plan.
**Consumes:** Tasks 1–5. **Produces:** checked acceptance record, not a new verifier.

- [ ] Add `UnknownFeasibilityDoesNotProveEmptyUniversalDomain` at the authority
  consumer; preserve unknowns for absent/mixed paths and unsupported producers.
- [ ] Run all native fixture families, unchanged identity goldens and conformance;
  inspect assertion-level outcomes rather than merely CTest's green summary.
- [ ] Document every changed CPG/Evidence identity and map each #149 gap to its
  issue, delivered commit or explicit deferral.
- [ ] Run a clean configure/build, full CTest with JUnit skip inspection,
  `git diff --check`, license and worktree checks before pushing.
- [ ] Commit the acceptance record. Only mark an issue implemented when its
  exact required/forbidden outcomes are exercised by real producers.
