# M10B Evidence Builder Input APIs and First Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose bounded, provenance-backed Evidence Builder input APIs and
produce one deterministic, immutable buffer-overflow `EvidenceBuildInput` for
M10C.

**Architecture:** Build `EvidenceQueryService` over one pinned M6 CPG projection
and one M9 fact/provenance read snapshot. Every flow or fact result carries
shared completeness metadata and an M9-backed query-completion certificate.
Bundle the results into one typed handoff; diagnostic JSON remains a public
debug representation and is never an internal M10C boundary.

**Tech Stack:** C++20, `veritas::Status`/`StatusOr<T>`, M6 CPG queries, M9
FactStore and ProvenanceStore, LLVM JSON output, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md`

**Executable test contract:** `docs/specs/milestones/m10b-m10c-api-to-evidence-ir-test-design-spec.md`

## Global Constraints

- M10A and M9 must be implemented and passing before Task 2 begins.
- M10B does not implement `EvidenceCase`, EIR levels, `EvidenceID`, EIR-T,
  Protobuf, or full-EIR JSON; M10C owns those boundaries.
- Every public query is bounded, deterministic, and returns
  `QueryResultMetadata`; `kUnspecified` is invalid at the public boundary.
- `max_facts_per_query` is positive, exact-boundary completion is distinct from
  overflow, and a `limit + 1` probe row is never exposed.
- Every query result carries one analysis run and one resolvable
  `evidence.query_completion.v1` fact with a selected M9 witness.
- Complete-empty and truncated-empty results remain distinguishable in typed
  values and diagnostic JSON.
- Complete empty open-world output remains unknown; truncated empty output
  never becomes negative evidence.
- `BuildEvidenceInput` executes against one pinned CPG/fact/provenance snapshot
  and never returns a mixed-run handoff.
- Facts retain semantic and epistemic state; supporting and contradicting facts
  remain separate.
- Summary edges remain expandable, and source text is referenced rather than
  copied by default.
- All 23 M10B-owned `AC`, `QRY`, `HND`, and `DEM-001` cases are mandatory; no
  catalog case is disabled or silently skipped.

---

### Task 1: Define the Query, Completion-Provenance, and Test Contracts

**Files:**
- Create: `include/veritas/evidence/SliceTypes.h`
- Create: `include/veritas/evidence/QueryCompletion.h`
- Create: `src/evidence/SliceTypes.cpp`
- Create: `src/evidence/QueryCompletion.cpp`
- Create: `src/evidence/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/support/evidence/EvidenceScenario.h`
- Create: `tests/support/evidence/EvidenceScenario.cpp`
- Modify: `tests/support/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceContractTest.cpp`
- Create: `tests/unit/evidence/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `QueryCompleteness`, `TruncationReason`, `QueryResultMetadata`
- Produces: `EvidenceQueryBudget`, `FlowSlice`, `EvidenceFactSet`
- Produces: `ClaimKind`, `Severity`, `ClaimSeed`, `EvidenceBuildInput`
- Produces: `QueryCompletionDescriptor`, `MakeQueryCompletionFact`,
  `ValidateQueryCompletion`
- Produces: deterministic `ToDiagnosticJson(const EvidenceFactSet&)` and
  `ToDiagnosticJson(const EvidenceBuildInput&)`
- Produces: test-only `veritas::testing::EvidenceScenarioBuilder`

- [ ] **Step 1: Write failing metadata tests for `AC-001` and `AC-002`**

Create parameterized tests proving complete-empty differs from truncated-empty
and rejecting every invalid public metadata shape:

```cpp
TEST(EvidenceContractTest, CompleteEmptyDiffersFromTruncatedEmpty) {
  const auto complete = EmptyFactSet(QueryCompleteness::kComplete, {});
  const auto truncated =
      EmptyFactSet(QueryCompleteness::kTruncated,
                   {TruncationReason::kMaxFacts});
  EXPECT_NE(complete.metadata, truncated.metadata);
  EXPECT_NE(ToDiagnosticJson(complete), ToDiagnosticJson(truncated));
}

TEST_P(InvalidQueryResultMetadataTest, RejectsInvalidResultMetadata) {
  EXPECT_FALSE(ValidateQueryResultMetadata(GetParam()).ok());
}
```

The parameter set covers unspecified completeness, complete with a reason,
truncated without a reason, missing/wrong-kind analysis run, and
missing/wrong-kind query provenance.

- [ ] **Step 2: Run the contract test to verify it fails**

Run:

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build -R EvidenceContractTest --output-on-failure
```

Expected: FAIL because the public evidence contract does not exist.

- [ ] **Step 3: Define the exact public slice and budget types**

Use invalid sentinels for public enums and this shared metadata shape:

```cpp
enum class QueryCompleteness {
  kUnspecified,
  kComplete,
  kTruncated,
};

struct QueryResultMetadata {
  QueryCompleteness completeness = QueryCompleteness::kUnspecified;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t examined_items = 0;
  core::StableId analysis_run_id;
  core::StableId query_provenance_id;
  auto operator<=>(const QueryResultMetadata&) const = default;
};

struct EvidenceQueryBudget {
  std::size_t max_depth = 0;
  std::size_t max_nodes = 0;
  std::size_t max_paths = 0;
  std::size_t max_facts_per_query = 0;
  std::size_t max_provenance_depth = 0;
};
```

`FlowSlice` and `EvidenceFactSet` each contain `QueryResultMetadata metadata`.
`EvidenceBuildInput` contains the claim seed, flow, ranges, capacities, aliases,
dominating checks, unknowns, immutable summary/source-anchor references, and
explicit `query_completion_facts` and `query_completion_bindings` collections
plus the bounded M9 provenance closure. Add stable `ToString` and rejecting
parse helpers for every textual enum.

- [ ] **Step 4: Define and validate the M9-backed query completion fact**

`QueryCompletionDescriptor` contains the exact cells specified by the companion
contract: query kind, ordered scope refs, budget, query implementation version,
`input_snapshot_fingerprint`, completeness, ordered reasons, examined count, and
returned-member digest. `MakeQueryCompletionFact` publishes relation
`evidence.query_completion.v1` as `IdKind::kFact` through M9.

`ValidateQueryCompletion` verifies the completion fact, its `RunFactBinding`,
and selected `FactWitness` against the result metadata and canonical payload.
Reject mismatched scope, budget, run, completeness, reason order, count, digest,
producer, or implementation version. Do not synthesize a missing witness.

- [ ] **Step 5: Add the reusable typed scenario builder**

`EvidenceScenarioBuilder` assigns stable IDs from symbolic names, creates typed
facts, run bindings, witnesses, query completion facts, flow members, and
complete/truncated metadata. It exposes insertion-order reversal and run/build
mutation helpers. It never parses JSON or EIR-T. Add its source to
`veritas_test_support` and link that target privately to `veritas_evidence`.

- [ ] **Step 6: Implement `AC-003` through `AC-006`**

Test exact fact-budget completion, one-row overflow with canonical prefix and
`kMaxFacts`, duplicate/unsorted metadata rejection, and complete deterministic
`EvidenceBuildInput` JSON. The JSON assertion first validates typed content,
then verifies byte equality, every metadata field, and one final newline.

- [ ] **Step 7: Run and label the contract suite**

Register `EvidenceContractTest` with label `evidence-contract`, then run:

```bash
cmake --build --preset default
ctest --test-dir build -L evidence-contract --output-on-failure
```

Expected: all six `AC` cases pass.

- [ ] **Step 8: Commit the contract layer**

```bash
git add CMakeLists.txt include/veritas/evidence src/evidence \
  tests/support tests/unit/evidence tests/unit/CMakeLists.txt
git commit -m "feat: define evidence query contracts"
```

---

### Task 2: Implement Bounded Queries and the Immutable Handoff

**Files:**
- Create: `include/veritas/evidence/EvidenceQueryService.h`
- Create: `src/evidence/EvidenceQueryService.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceQueryServiceTest.cpp`
- Create: `tests/unit/evidence/EvidenceHandoffTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: one immutable `cpg::ThinCpg` projection
- Consumes: one M9 FactStore/ProvenanceStore read snapshot and analysis run
- Produces: `EvidenceQueryService::GetValueFlow`
- Produces: `GetRanges`, `GetCapacities`, `GetAliases`, `GetUnknowns`,
  `GetDominatingChecks`, and `Explain`
- Produces: `EvidenceQueryService::BuildEvidenceInput`

- [ ] **Step 1: Write failing synthetic query tests**

Use `EvidenceScenarioBuilder` to implement `QRY-002`, `QRY-003`, `QRY-004`,
`QRY-008`, and `QRY-010`. Parameterize depth, node, path, fact, and provenance
limits. Every case asserts the canonical returned members, exact completeness,
ordered reasons, examined count, matching completion fact, and forbidden
epistemic conversions.

- [ ] **Step 2: Write failing handoff tests `HND-001` through `HND-005`**

Test a fully bundled handoff, a backend binding change during assembly,
separate supporting/contradicting facts, complete-empty dominating-check
metadata, and truncated-empty dominating-check metadata. The snapshot-change
case accepts only an input wholly bound to the pinned first snapshot or a
stable retryable failure.

- [ ] **Step 3: Run the focused tests to verify they fail**

Run:

```bash
ctest --test-dir build \
  -R "EvidenceQueryServiceTest|EvidenceHandoffTest" \
  --output-on-failure
```

Expected: FAIL because `EvidenceQueryService` does not exist.

- [ ] **Step 4: Implement one pinned read snapshot**

At construction, bind the immutable CPG projection. At the start of
`BuildEvidenceInput`, open one M9 read transaction for the requested
`analysis_run_id`; use that same transaction for every fact and provenance
lookup. Capture repository, revision, build variant, analysis configuration,
analysis run, CPG projection, and fact snapshot in the snapshot descriptor.
Return `Status::FailedPrecondition` with stable text
`evidence snapshot changed; retry` if a backend cannot retain that binding.
Never retry individual subqueries against a new current run.

- [ ] **Step 5: Implement deterministic bounded flow and fact queries**

Sort candidate results by canonical semantic ID before applying limits. Probe
at most one additional matching result; exact end-of-input is `kComplete`, and
an additional match is `kTruncated` with the exact limit reason. Build and
publish one query completion fact for every complete or truncated result.
Return the fact-limit budget independently for range, capacity, alias, unknown,
and dominating-check queries.

- [ ] **Step 6: Preserve semantic and provenance state**

Return all four alias kinds unchanged, keep fact epistemic state independent,
retain supporting and contradicting collections separately, and keep opaque
external semantics unknown. `Explain` may truncate provenance independently
without removing the semantic fact; it adds its own stable reason.

- [ ] **Step 7: Implement `BuildEvidenceInput` without analysis recomputation**

Execute each bounded query exactly once against the pinned snapshot, retain the
supplied `ClaimSeed`, and bundle all results, summaries, anchors, completion
facts, run bindings, selected witnesses, and bounded provenance. Validate every
result and cross-result snapshot binding before returning. Do not perform EIR
mapping, validation, projection, identity, or serialization.

- [ ] **Step 8: Run and label the focused suites**

Register both tests with `evidence-unit`; also label `EvidenceHandoffTest` as
`evidence-contract`. Run:

```bash
cmake --build --preset default
ctest --test-dir build \
  -R "EvidenceQueryServiceTest|EvidenceHandoffTest" \
  --output-on-failure
```

Expected: the synthetic `QRY` cases and `HND-001` through `HND-005` pass.

- [ ] **Step 9: Commit the query and handoff layer**

```bash
git add include/veritas/evidence/EvidenceQueryService.h \
  src/evidence/EvidenceQueryService.cpp src/evidence/CMakeLists.txt \
  tests/unit/evidence
git commit -m "feat: build immutable evidence query inputs"
```

---

### Task 3: Add the Comprehensive Overflow Fixture Family

**Files:**
- Create: `tests/fixtures/projects/evidence_overflow_unsafe/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_unsafe/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_unsafe/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_safe/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_safe/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_safe/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_non_dominating/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_non_dominating/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_non_dominating/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_mixed_paths/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_mixed_paths/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_mixed_paths/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_opaque_validator/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_opaque_validator/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_opaque_validator/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_alias_uncertain/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_alias_uncertain/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_alias_uncertain/main.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_summary/compile_commands.json`
- Create: `tests/fixtures/projects/evidence_overflow_summary/packet.h`
- Create: `tests/fixtures/projects/evidence_overflow_summary/entry.cpp`
- Create: `tests/fixtures/projects/evidence_overflow_summary/copy.cpp`
- Create: `tests/integration/evidence/OverflowEvidenceFixtureTest.cpp`
- Create: `tests/integration/evidence/EvidenceHandoffIntegrationTest.cpp`
- Create: `tests/integration/evidence/CMakeLists.txt`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Produces: real M6/M9/M10A-backed overflow query scenarios
- Verifies: `QRY-001`, `QRY-005`, `QRY-006`, `QRY-007`, `QRY-009`, `HND-006`

- [ ] **Step 1: Create the shared unsafe and safe semantic shapes**

Every fixture uses a 16-bit unsigned packet length and a 2048-byte destination:

```cpp
struct Packet {
  const unsigned char* payload;
  std::uint16_t length;
};

struct Buffer {
  unsigned char data[2048];
};
```

The unsafe project passes `packet.length` directly to `memcpy`; the safe project
guards it with `packet.length <= sizeof(buffer.data)` on every path to the sink.

- [ ] **Step 2: Create the distinguishing negative and uncertainty fixtures**

The non-dominating project places the check on a sibling branch. The mixed-path
project has one checked and one unchecked path. The opaque-validator project
guards the sink with an unmodeled external predicate. The alias-uncertain
project preserves `MAY_ALIAS`/`UNKNOWN_ALIAS`. The summary project crosses two
translation units and retains expandable summary references.

- [ ] **Step 3: Write failing real-pipeline assertions**

Implement the six owned real-fixture cases. Assert typed range `[0,65535]`,
capacity `2048`, precise sink/value references, check dominance rather than
lexical existence, visible checked and unchecked paths, blocking external
unknowns, and cross-root deterministic summary/anchor references. Each case
asserts its forbidden output before comparing any presentation.

- [ ] **Step 4: Run the fixture tests to verify they fail**

Run:

```bash
ctest --test-dir build \
  -R "OverflowEvidenceFixtureTest|EvidenceHandoffIntegrationTest" \
  --output-on-failure
```

Expected: FAIL until the real query adapters satisfy the fixture contract.

- [ ] **Step 5: Complete the real CPG, FactStore, and provenance adapters**

Resolve each claim seed against the materialized project, query the pinned CPG
and M9 snapshot through Task 2, and return the same typed records used by the
synthetic tests. No fixture injects handwritten semantic facts after analysis.

- [ ] **Step 6: Run and label the integration cases**

Register both tests with `evidence-integration`, run the command from Step 4,
and require all six real-fixture cases to pass.

- [ ] **Step 7: Commit the fixture family**

```bash
git add tests/fixtures/projects/evidence_overflow_* \
  tests/integration/CMakeLists.txt tests/integration/evidence
git commit -m "test: add comprehensive overflow evidence fixtures"
```

---

### Task 4: Expose Deterministic Slice JSON Through the Public CLI

**Files:**
- Modify: `src/tools/veritas-query.cpp`
- Modify: `src/tools/CMakeLists.txt`
- Create: `tests/integration/evidence/VeritasQueryEvidenceTest.cpp`
- Create: `tests/golden/evidence/overflow_unsafe.slice.json`
- Modify: `tests/integration/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `veritas-query evidence overflow --sink memcpy --format json`
- Produces: `--max-depth`, `--max-nodes`, `--max-paths`, `--max-facts`, and
  `--max-provenance-depth`
- Verifies: `DEM-001`

- [ ] **Step 1: Write the failing compatibility and golden test**

Use the established `CliResult`, shell-quoting, temporary-output, and
`RunVeritasQuery` pattern from
`tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`. Run the unsafe fixture
through the public command, parse its JSON into the typed oracle, then compare
bytes with `overflow_unsafe.slice.json`.

- [ ] **Step 2: Run the CLI test to verify it fails**

Run: `ctest --test-dir build -R VeritasQueryEvidenceTest --output-on-failure`

Expected: FAIL because the evidence subcommand is unavailable.

- [ ] **Step 3: Implement exact CLI parsing and validation**

Accept `--sink`, `--format json`, and every shared budget flag. Reject unknown
formats, missing/duplicate values, zero budgets, overflowed integers, and
unsupported options with stable non-zero exits. Fixture/store selection belongs
to integration setup and is not exposed as a public fixture-selection option.

- [ ] **Step 4: Emit complete deterministic slice JSON**

Call `BuildEvidenceInput` once and serialize that value. Include the claim seed,
flow, range, capacity, alias, dominating-check, unknown, summary, anchor,
completion-provenance, examined-count, and truncation fields. Append one final
newline. Do not emit EIR-T, Protobuf, or full-EIR JSON.

- [ ] **Step 5: Verify `DEM-001` and repeated-run determinism**

Run the same materialized fixture twice from fresh stores and once after reverse
backend insertion. Parse and validate each output, then require byte-identical
JSON. Register the test with labels `evidence-integration` and `evidence-cli`.

- [ ] **Step 6: Run the CLI suite**

Run: `ctest --test-dir build -L evidence-cli --output-on-failure`

Expected: `DEM-001` passes.

- [ ] **Step 7: Commit the public M10B demo**

```bash
git add src/tools/veritas-query.cpp src/tools/CMakeLists.txt \
  tests/integration/evidence tests/golden/evidence
git commit -m "feat: expose deterministic evidence slice JSON"
```

---

### Task 5: Qualify the M10B Milestone and M10C Handoff

**Files:**
- Verify only; no intended source changes

**Interfaces:**
- Verifies: all 23 M10B-owned companion-contract cases
- Verifies: M10C section 5 consumes the public handoff without JSON
- Verifies: repository clean-build and full-suite policy

- [ ] **Step 1: Audit stable case registration and labels**

Confirm exactly `AC-001`–`AC-006`, `QRY-001`–`QRY-010`, `HND-001`–`HND-006`,
and `DEM-001` are registered, enabled, and mapped to the test names in the
companion contract. Run:

```bash
ctest --test-dir build -L evidence-contract --output-on-failure
ctest --test-dir build -L evidence-integration --output-on-failure
ctest --test-dir build -L evidence-cli --output-on-failure
```

Expected: all 23 owned cases pass with no skips.

- [ ] **Step 2: Run the bounded public demonstration**

```bash
./build/bin/veritas-query evidence overflow \
  --sink memcpy \
  --format json \
  --max-depth 8 \
  --max-nodes 256 \
  --max-paths 5 \
  --max-facts 64 \
  --max-provenance-depth 8
```

Parse the output and verify one typed claim seed, all query result categories,
one matching completion fact per result, one analysis snapshot, and explicit
completeness. Confirm that M10C receives the same `EvidenceBuildInput` directly
without JSON round-tripping or a second query.

- [ ] **Step 3: Audit the M10C public handoff**

Cross-check `include/veritas/evidence/SliceTypes.h` against M10C design section
5 and the companion `HND` catalog. The header exposes every M10C input while
remaining free of EIR model/codec types and installed third-party native types.

- [ ] **Step 4: Run the mandatory clean build**

```bash
rm -rf build
cmake --preset default
cmake --build --preset default
```

Expected: configure and build complete with zero errors.

- [ ] **Step 5: Run focused and complete test suites**

```bash
ctest --test-dir build \
  -R "EvidenceContract|EvidenceQueryService|EvidenceHandoff|OverflowEvidence|VeritasQueryEvidence" \
  --no-tests=error --output-on-failure
ctest --test-dir build --output-on-failure
```

Expected: focused Evidence tests and the complete repository suite pass with no
unapproved skips.

- [ ] **Step 6: Run formatting, license, and branch-state checks**

```bash
git diff --check main...HEAD
git status --porcelain
git diff --stat main...HEAD
git log --oneline main..HEAD
```

Inspect every new C++, CMake, and Protobuf file for the required Apache-2.0
header. Expected: no whitespace/license failure and a clean worktree containing
only the reviewed M10B implementation.
