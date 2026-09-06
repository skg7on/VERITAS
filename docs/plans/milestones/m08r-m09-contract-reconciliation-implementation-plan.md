# M8R–M9 Contract Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the shipped M8R.4/M9 production WPA path honor the audited contracts in the reconciliation design spec — exact identities, component-scoped facts, witness.v2, comprehensive batch validation, atomic idempotent persistence — and qualify the whole thing on the real `semantic_zoo` corpus.

**Architecture:** Seven ordered checkpoints, each landing a testable slice and its own commit. Checkpoints 1–2 fix the orchestrator and identities/cache; 3–4 fix the witness/batch/fact-store contracts; 5 wires the production pipeline end-to-end and adds the conformance oracle; 6 qualifies the corpus and expands the M9 gate; 7 reconciles documentation. Work is in a dedicated worktree on branch `claude/m8r-m9-contract-reconciliation`.

**Tech Stack:** C++20 (`-fno-rtti -fno-exceptions`), CMake 3.23+/Ninja, SQLite (embedded schema), RocksDB (object store), GoogleTest.

**Spec:** `docs/specs/milestones/m08r-m09-contract-reconciliation-design-spec.md` — the plan argues from this spec and pins every edit to its §-sections. The authoritative tracker is GitHub issue #109.

## Global Constraints

- C++20; **no RTTI, no exceptions** — use `veritas::Status`/`StatusOr`, `llvm::isa/cast/dyn_cast` never `dynamic_cast`/`typeid`.
- Every new in-scope file (`.h/.cpp/.sql/.cmake/.py`) opens with the Apache-2.0 header ("Licensed under the Apache License, Version 2.0" in the first 20 lines).
- Identity IDs are `kind:sha256:<64-hex-lowercase>` via `core::MakeStableId`/`core::StableId`; never file-line based.
- Deterministic length-prefixed canonical encoding everywhere a hash is derived (no raw `|`-concatenation that can collide).
- Schema changes go through `ApplySchema()` (`src/summarydb/MetadataStore.cpp:122`), which applies embedded `schema_v1..vN.h` in order; add `v4` the same way.
- The Soufflé provenance manifest is generated at `${CMAKE_BINARY_DIR}/souffle-provenance.json` by `cmake/WriteSouffleProvenance.cmake`; the analyzer target must depend on the `veritas_souffle_provenance` target.
- M9 gate (`tools/check_m9_entry.py`) enforces exact CTest-label membership — every new/changed test must be registered in `EXPECTED_TESTS_BY_LABEL`.
- Do **not** restore the removed `FixpointEngine`/`FactTuple` paths (#94); do **not** weaken deterministic IDs, fail-closed publication, or the serial qualification baseline.

## File Structure

| File | Role | Change |
|---|---|---|
| `src/wpa/WpaOrchestrator.cpp` | orchestration loop | key `completed_facts` by `WpaComponentKey` (CP1) |
| `src/analysis/ProjectAnalyzer.cpp` | production pipeline | wire SCC repo + identities + oracle + batch publish (CP1, CP2, CP5) |
| `src/wpa/WpaRunRepository.{h,cpp}` | run state + cache | `ResultCacheDescriptor` v2 + strict reuse (CP2) |
| `src/wpa/SouffleProvenance.{h,cpp}` | **new** | runtime provenance loader/validator (CP2) |
| `src/wpa/SouffleWpaExecutor.cpp` | executor identity | digest-derived toolchain identity (CP2) |
| `include/veritas/facts/Witness.h` | witness vocabulary | structured `RootedInputFact`, `derivation_id` (CP3) |
| `src/facts/ResultCanonicalizer.cpp` | canonicalization | `RuleSpec` arity + `derivation_id` (CP3) |
| `src/facts/FactStore.cpp` | persistence | v4 receipt + witness-dependent `witness_id` (CP3, CP4) |
| `src/facts/AnalysisFactBus.cpp` | batch | v2 `BatchId` + comprehensive `Validate` (CP4) |
| `src/facts/ProvenanceStore.cpp` | explain | current-binding selection (CP4) |
| `src/summarydb/schema/v4.sql`, `schema_v4.h.in`, `MetadataStore.cpp` | schema | `(run_id,batch_id)` receipt (CP4) |
| `cmake/VeritasSouffle.cmake` | build | runtime provenance path + target dep (CP2) |
| `tools/check_m9_entry.py` | gate | register new tests (CP6) |
| `tests/**` | tests | one new test per checkpoint (CP1–CP6) |
| `docs/**` | docs | reconcile status (CP7) |

---

### Task 1: Orchestration correctness (design §5)

**Files:**
- Modify: `src/wpa/WpaOrchestrator.cpp:37-62` (`SuccessorSupport`), `:146`, `:261` (`completed_facts`)
- Modify: `src/analysis/ProjectAnalyzer.cpp:117-119,148,159` (construct + pass `SccStateRepository`)
- Test: `tests/wpa/WpaOrchestratorTest.cpp`

**Interfaces:**
- Consumes: `WpaComponentKey` (`include/veritas/wpa/WpaRunRepository.h:56`), `SccGraph::Successors`, `SccStateRepository` (existing public ctor taking `summarydb::MetadataStore&`).
- Produces: `completed_facts` becomes `std::map<WpaComponentKey, std::vector<facts::AnalysisFact>>`; `SuccessorSupport` filters successors by the *same* `WpaComponentKind`.

- [ ] **Step 1: Change `completed_facts` key type.**

In `WpaOrchestrator::Run`, replace the SCC-only map (line 146) with a component-keyed map and update the two write sites (line 261 store; line 52/264 lookups via `SuccessorSupport`):

```cpp
std::map<WpaComponentKey, std::vector<facts::AnalysisFact>> completed_facts;
// ...
completed_facts[key] = std::move(component_result.facts);
```

`SuccessorSupport` takes `component` and looks up `{successor, component}` instead of `successor`:

```cpp
auto it = completed_facts.find(WpaComponentKey{successor, component});
```

- [ ] **Step 2: Run `WpaOrchestratorTest` — existing tests must still pass.**

Run: `ctest --test-dir build -R WpaOrchestratorTest --output-on-failure`
Expected: PASS (behavior unchanged for the single-component cases they cover).

- [ ] **Step 3: Write the failing two-component multi-SCC test.**

In `tests/wpa/WpaOrchestratorTest.cpp`, add `TwoComponentsPreserveReachabilitySupport`: a call graph `A → B → C` (three SCCs), run `{kReachability, kMemoryEffects}`, and assert that B's Reachability facts are present in the successor support materialized for A's Reachability component even though B's MemoryEffects component completed after it.

- [ ] **Step 4: Verify the new test passes.**

Run: `ctest --test-dir build -R WpaOrchestratorTest --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Wire `SccStateRepository` in `ProjectAnalyzer::RunWpa`.**

Open it from the same metadata store as `WpaRunRepository` and pass it to both orchestrator construction sites (lines 148 and 159):

```cpp
auto repo = wpa::WpaRunRepository::Open(request.output_root / "metadata.db");
// ... existing repo checks ...
wpa::SccStateRepository scc_state(repo->metadata_store());
wpa::WpaOrchestrator orchestrator(executor, *repo, &scc_state);
```

Change the repository path from `output_root / "wpa"` to `output_root` so the WPA run state, SCC state, facts, and provenance all share `output_root/metadata.db` (design §3, §5). Add a test seam (`ProjectAnalyzerWpaTest`) asserting the shared DB now carries `wpa_component_states_v2` **and** `wpa_sccs`/`wpa_scc_members`/`wpa_scc_edges` rows.

- [ ] **Step 6: Run the WPA integration test.**

Run: `ctest --test-dir build -R ProjectAnalyzerWpaTest --output-on-failure`
Expected: PASS with SCC topology persisted.

- [ ] **Step 7: Commit.**

```bash
git add src/wpa/WpaOrchestrator.cpp src/analysis/ProjectAnalyzer.cpp tests/wpa/WpaOrchestratorTest.cpp tests/...
git commit -m "fix(wpa): key completed facts by component and wire SCC state"
```

---

### Task 2: Exact identities and cache validation (design §4, §6)

**Files:**
- Create: `src/wpa/SouffleProvenance.h`, `src/wpa/SouffleProvenance.cpp`
- Modify: `src/wpa/SouffleWpaExecutor.cpp:123` (identity)
- Modify: `src/analysis/ProjectAnalyzer.cpp:104-112` (hashes)
- Modify: `src/wpa/WpaRunRepository.{h,cpp}:76,395,471` (cache key + reuse)
- Modify: `cmake/VeritasSouffle.cmake` (runtime path define)
- Test: `tests/wpa/SouffleProvenanceTest.cpp`, `tests/wpa/WpaRunRepositoryTest.cpp`, `tests/analysis/ProjectAnalyzerWpaTest.cpp`

**Interfaces:**
- Consumes: `souffle-provenance.json` schema produced by `WriteSouffleProvenance.cmake` (`source_revision`, `executable_sha256`, artifact digests).
- Produces: `SouffleProvenance SouffleProvenance::Load(path) -> StatusOr<SouffleProvenance>`; `std::string ToolchainIdentity()` (versioned digest); `StatusOr<std::string> SfvConfigHash(const svf::SvfConfig&)`; `StatusOr<std::string> WpaConfigHash(...)`; `struct ResultCacheDescriptor` with `std::string Encode() const` and `core::StableId Key() const`.

- [ ] **Step 1: Add the runtime provenance loader.**

`SouffleProvenance::Load` parses the manifest, requires `source_revision == "5682a9f12e2668ecdd26348fe63cc508bc0fcf47"`, verifies every recorded artifact digest, recomputes the canonical provenance digest, and exposes `ToolchainIdentity()` = `"souffle-" + digest`. Any missing/malformed/mismatched artifact returns `FailedPrecondition`.

- [ ] **Step 2: Write the failing provenance tests.**

`SouffleProvenanceTest` gains: (a) valid manifest loads and yields a stable identity; (b) wrong revision fails; (c) tampered `executable_sha256` fails; (d) truncated/malformed JSON fails. Run: `ctest --test-dir build -R SouffleProvenanceTest` → expected FAIL on (a) until the loader exists.

- [ ] **Step 3: Replace the executor identity label.**

`SouffleWpaExecutor` takes a loaded `SouffleProvenance` (or its `ToolchainIdentity()`), not the literal `"souffle-2.5-pinned"`. Update the ctor and `ProjectAnalyzer` to pass it.

- [ ] **Step 4: Derive real configuration hashes.**

Replace the `'a'`/`'b'` placeholders (`ProjectAnalyzer.cpp:104-105`): `svf_configuration_hash = Hash(SvfConfig::CanonicalAnalyzerConfig())`, `wpa_configuration_hash = Hash(length-prefixed canonical component set + limits + relation/rule-bundle settings)`. Add a `AnalysisRunTest` case that each descriptor field change flips the RunId.

- [ ] **Step 5: Add `ResultCacheDescriptor` v2 and replace `DeriveResultCacheKey`.**

The descriptor holds engine identity, toolchain identity, logical-input hash, SCC, component, summary/relation schema versions, rule/model bundle versions, and SVF/WPA hashes; `Encode()` is length-prefixed and `Key()` returns `core::MakeStableId(kFact, sha256(Encode()))`. `DeriveResultCacheKey` becomes `Descriptor(run, key, logical_input_hash).Key()`.

- [ ] **Step 6: Strict reuse validation.**

`LoadReusableComponent` takes the full descriptor (not a raw string), and after deserializing revalidates: metadata row, object key, SCC/component match, logical-input hash, recomputed fixpoint/external hashes over the decoded facts, and every fact's `FactID`. Any mismatch is a hard `FailedPrecondition` cache-integrity error, never a hit. Write `WpaRunRepositoryTest` cases for a tampered object and a digest mismatch.

- [ ] **Step 7: Make the analyzer target depend on the provenance artifact.**

Add `add_dependencies(veritas_build_analyzer veritas_souffle_provenance)` (or the equivalent in the tools `CMakeLists.txt`) and define the runtime path into `ProjectAnalyzer`.

- [ ] **Step 8: Run the affected tests.**

Run: `ctest --test-dir build -R 'SouffleProvenanceTest|WpaRunRepositoryTest|AnalysisRunTest|ProjectAnalyzerWpaTest' --output-on-failure`
Expected: PASS.

- [ ] **Step 9: Commit.**

```bash
git add src/wpa/SouffleProvenance.* src/wpa/SouffleWpaExecutor.cpp src/analysis/ProjectAnalyzer.cpp src/wpa/WpaRunRepository.* cmake/VeritasSouffle.cmake tests/...
git commit -m "feat(wpa): runtime provenance identity and v2 cache descriptor"
```

---

### Task 3: witness.v2 and structured root evidence (design §7)

**Files:**
- Modify: `include/veritas/facts/Witness.h:65` (`RootedInputFact`)
- Modify: `src/facts/ResultCanonicalizer.cpp:105` (`Canonicalize`)
- Modify: `src/facts/FactStore.cpp:182,202` (`witness_id`)
- Modify: `include/veritas/facts/RuleRegistry.h` (`RuleSpec` arity) if not already present
- Test: `tests/facts/ResultCanonicalizerTest.cpp`, `tests/facts/FactStoreTest.cpp`

**Interfaces:**
- Consumes: `RuleSpec` (add `std::uint32_t arity`), `WitnessEdge.derivation_key` (already present from #105).
- Produces: `RootedInputFact { AnalysisFact fact; std::string producer_id; std::string provenance_ref; std::string source_anchor_id; std::string summary_id; std::string description; }`; `CanonicalizedResult` gains a persisted, witness-dependent `derivation_id` per selected proof and `witness_id` distinct from `FactID`.

- [ ] **Step 1: Extend `RootedInputFact` with structured evidence.**

Add the five evidence fields above; thread them through `WpaRunResult`, `AnalysisFactBatch`, and `FactStore::Publish`. Keep `provenance_ref` for back-compat but stop reducing roots to IDs.

- [ ] **Step 2: Add `RuleSpec::arity` and reject malformed ordinals.**

In `ResultCanonicalizer::Canonicalize`, after grouping by `(result, rule, derivation_key)`, reject a derivation whose ordinal set is not `{0..arity-1}` (missing, duplicate, or out-of-range).

- [ ] **Step 3: Persist a witness-dependent `derivation_id` / `witness_id`.**

Compute `derivation_id = Hash(result semantic key + rule_id + ordered input semantic keys)` for the selected proof; store it as the `witness_id` in `FactStore::Publish` (replacing `selected_witness_id = FactID` at line 182 and `witness_id = FactID` at line 202), so two derivations of the same fact produce two `provenance_nodes` rows.

- [ ] **Step 4: Write tests.**

`ResultCanonicalizerTest` gains: malformed arity rejection (missing/duplicate/out-of-range), deterministic multi-input/alternative selection. `FactStoreTest` gains: re-deriving one fact via a different proof retains both witness records while the current binding selects exactly one. Run the two suites → PASS.

- [ ] **Step 5: Commit.**

```bash
git add include/veritas/facts/Witness.h include/veritas/facts/RuleRegistry.h src/facts/ResultCanonicalizer.cpp src/facts/FactStore.cpp tests/facts/*
git commit -m "feat(facts): witness.v2 derivation identity and structured root evidence"
```

---

### Task 4: Batch and Fact Store correctness (design §8)

**Files:**
- Modify: `src/facts/AnalysisFactBus.cpp:37,144`
- Modify: `src/facts/FactStore.cpp:137`
- Modify: `src/facts/ProvenanceStore.cpp:78`
- Create: `src/summarydb/schema/v4.sql`, `src/summarydb/schema/schema_v4.h.in`
- Modify: `src/summarydb/MetadataStore.cpp:122` (`ApplySchema`)
- Test: `tests/facts/AnalysisFactBusTest.cpp`, `tests/facts/FactStoreTest.cpp`, `tests/facts/ProvenanceStoreTest.cpp`

**Interfaces:**
- Consumes: `AnalysisFactBatch` fields (completed components incl. `result_object_key`/hashes), `FactStore::Publish`, `ProvenanceStore::Explain`.
- Produces: `DeriveBatchId` v2 over every immutable field; `AnalysisFactBus::Validate` full closure/acyclicity/root-reachability; v4 `fact_batch_receipts (run_id, batch_id)` with atomic transaction; `Explain` current-binding.

- [ ] **Step 1: v2 `BatchId`.**

`DeriveBatchId` (version `"veritas.analysis-fact-batch.v2"`) additionally folds each completed component's `result_object_key`, `fixpoint_hash`, `external_hash`, and the flattened `diagnostics`. Add a `AnalysisFactBusTest` case where mutating any one of those fields changes the batch id.

- [ ] **Step 2: Comprehensive `Validate`.**

Extend `Validate` to: recompute and compare `batch_id`; reject duplicate expected/completed keys; verify each completion matches its key and recomputed result hashes; assert flattened facts/witnesses/roots/diagnostics equal the canonical union of completions; verify every witness result is a published fact; verify every published fact has exactly one selected, finite, acyclic proof reaching declared roots. Add negative tests (extra witness output, cycle, unrooted subgraph, mixed runs, incomplete run).

- [ ] **Step 3: v4 schema receipt.**

Add `schema/v4.sql` (`INSERT OR IGNORE INTO schema_version (version) VALUES (4); CREATE TABLE IF NOT EXISTS fact_batch_receipts (run_id TEXT NOT NULL, batch_id TEXT NOT NULL, wpa_run_id TEXT NOT NULL, PRIMARY KEY (run_id, batch_id));`), generate `schema_v4.h.in`, and apply it in `ApplySchema()`.

- [ ] **Step 4: Atomic idempotent `FactStore::Publish`.**

In one transaction: insert the receipt (or return `Ok` as a no-op if `(run_id, batch_id)` already exists), facts, bindings, root evidence, and witness records. Redelivery of the same receipt is a successful no-op; a different batch in the same run may create a new current binding and preserve the prior as history.

- [ ] **Step 5: Current-binding `Explain`.**

`ProvenanceStore::Explain` selects `is_current = 1`, follows the binding's `selected_witness_id`, and orders retained alternatives deterministically; a missing current binding or selected witness returns `NotFound`. Add `ProvenanceStoreTest` for stale-binding and deterministic-alternative ordering.

- [ ] **Step 6: Run the facts suites.**

Run: `ctest --test-dir build -R 'AnalysisFactBusTest|FactStoreTest|ProvenanceStoreTest' --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit.**

```bash
git add src/facts/AnalysisFactBus.cpp src/facts/FactStore.cpp src/facts/ProvenanceStore.cpp src/summarydb/schema/v4.sql src/summarydb/schema/schema_v4.h.in src/summarydb/MetadataStore.cpp tests/facts/*
git commit -m "feat(facts): comprehensive batch validation and atomic idempotent receipt"
```

---

### Task 5: Production pipeline integration and conformance (design §3, §4.3)

**Files:**
- Modify: `src/analysis/ProjectAnalyzer.cpp:86-174` (`RunWpa`)
- Test: `tests/analysis/ProjectAnalyzerWpaTest.cpp`

**Interfaces:**
- Consumes: `run_cpp_conformance_oracle` (`AnalysisConfig`), `SouffleWpaExecutor`, `CppConformanceExecutor`, `AnalysisFactBus`, `FactStore`.
- Produces: a fully-wired `RunWpa` that opens `output_root/metadata.db`, runs the primary engine, optionally runs the `kCppConformance` engine under a distinct RunId and compares canonical results, builds `AnalysisFactBatch`, and publishes via `AnalysisFactBus` + a `FactStore` sink.

- [ ] **Step 1: Build and publish the batch.**

After `WpaOrchestrator::Run` succeeds, call `MakeAnalysisFactBatch(result)`, `AnalysisFactBus bus(repo); bus.AddSink("fact-store", fact_store); bus.Publish(batch)`. `RunWpa` returns the batch id and fact count on the result.

- [ ] **Step 2: Honor the conformance oracle.**

When `config.run_cpp_conformance_oracle` is true, run a second `kCppConformance` execution over the same logical inputs with a distinct RunId; compare per-component canonical facts, `ExternalHash`, and `FixpointHash`; on mismatch mark both runs incomplete, record a diagnostic, return failure, and publish nothing.

- [ ] **Step 3: Write the integration test.**

`ProjectAnalyzerWpaTest` asserts: (a) normal `analyze` produces explainable facts in `output/metadata.db`; (b) `run_cpp_conformance_oracle=true` runs both engines and publishes on agreement; (c) a forced mismatch prevents publication. Run: `ctest --test-dir build -R ProjectAnalyzerWpaTest` → PASS.

- [ ] **Step 4: Commit.**

```bash
git add src/analysis/ProjectAnalyzer.cpp tests/analysis/ProjectAnalyzerWpaTest.cpp
git commit -m "feat(analysis): wire WPA end-to-end with conformance oracle"
```

---

### Task 6: `semantic_zoo` qualification (design §10)

**Files:**
- Modify: `tests/qualification/*` (differential/determinism/persistence/publication/explain over the corpus)
- Modify: `tools/check_m9_entry.py` (`EXPECTED_TESTS_BY_LABEL`)

**Interfaces:**
- Consumes: the `semantic_zoo` fixture tree and the `ProjectAnalyzer` + `FactStore` + `ProvenanceStore` stack.
- Produces: one new qualification test binary exercising the full corpus, registered under the correct M9 label.

- [ ] **Step 1: Add corpus end-to-end qualification.**

Exercise every `semantic_zoo` translation unit through Soufflé/C++ differential, determinism (two identical runs → identical hashes), SummaryDB persistence/reload, FactStore publication, and explain traversal. Fail on any mismatch.

- [ ] **Step 2: Register in the M9 gate.**

Add the new test to `EXPECTED_TESTS_BY_LABEL` under `souffle-production` (and, if split, `witness-closure`/`failure-atomicity`), so a label is no longer accepted as a substitute for production-path coverage.

- [ ] **Step 3: Run the gate.**

Run: `python3 tools/check_m9_entry.py --build-dir build`
Expected: all ten criteria pass.

- [ ] **Step 4: Commit.**

```bash
git add tests/qualification/* tools/check_m9_entry.py
git commit -m "test(qualification): semantic_zoo end-to-end coverage and gate expansion"
```

---

### Task 7: Documentation reconciliation (design §11)

**Files:**
- Modify: `docs/plans/README.md`, `docs/plans/milestones/README.md`, `docs/specs/milestones/README.md`
- Modify: `docs/plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md` (rewrite stale)
- Modify: `docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md` (M8R.3–M8R.5 status)
- Modify: `tests/qualification/M9DocumentationConsistencyTest.py`

**Interfaces:**
- Consumes: the shipped implementation and its verified test results.
- Produces: no stale "this PR"/"Outstanding" markers; `M9DocumentationConsistencyTest` asserts milestone status, `FactID` terminology, and the operational CLI statement.

- [ ] **Step 1: Reconcile status claims (only after all tests pass).**

Mark M8R.3–M8R.5 and M9 delivered with exact PR/commit evidence; replace the M8R "this PR" marker with reviewed merges; rewrite the M9 plan to match `FactID`/v4/Fact Bus/actual APIs; refresh the corpus and SummaryDB-guide status.

- [ ] **Step 2: Expand the doc-consistency test.**

`M9DocumentationConsistencyTest` asserts milestone status, plan/spec identity terminology, and the operational CLI statement; stale delivery/status claims fail the gate.

- [ ] **Step 3: Run the gate and full suite.**

Run: `python3 tools/check_m9_entry.py --build-dir build` and `ctest --test-dir build --output-on-failure -j1`
Expected: all pass.

- [ ] **Step 4: Commit.**

```bash
git add docs/** tests/qualification/M9DocumentationConsistencyTest.py
git commit -m "docs: reconcile M8R/M9 status with shipped implementation"
```

---

## Final Verification (all tasks)

```bash
rm -rf build && cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/path/to/llvm-project/build
cmake --build --preset default
ctest --test-dir build --output-on-failure -j1
python3 tools/check_m9_entry.py --build-dir build
git diff --check
```

Then confirm the license-header check, `git status --porcelain` empty, review the diff against `main`, and open the PR against #109.

## Self-Review Notes

- **Spec coverage:** §2 findings 1–11 map to Tasks 1–5 + 7; §10 corpus to Task 6; §4.2 provenance to Task 2; §8 receipt to Task 4. No finding is left uncovered.
- **Placeholder scan:** all edits cite concrete files/line anchors; no TBD/TODO.
- **Type consistency:** `WpaComponentKey`, `ResultCacheDescriptor`, `RootedInputFact`, `derivation_id`, `witness_id`, and `fact_batch_receipts` are used identically across tasks.
