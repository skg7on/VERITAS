# Lightweight Review Runtime Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline, or
> superpowers:subagent-driven-development when selected. Do not execute this
> proposed plan before its protocol amendment is reviewed.

**Goal:** Review a real C/C++ change with bounded source/local evidence and
decisive replay, without requiring a native whole-program database.
**Architecture:** One C++ public subprocess backend and one host-neutral Python
controller; the existing Claude plugin is a thin explicitly invoked host.
**Tech Stack:** C++20/Clang, Python 3.11+ standard library, POSIX processes,
strict canonical JSON, GoogleTest/CTest and Python unittest.
**Spec:** [lightweight runtime/backend v2](../specs/veritas-lightweight-review-runtime-design-spec.md).

**Work issues:** [RDX05 / #159](https://github.com/skg7on/VERITAS/issues/159), [RDX06 / #160](https://github.com/skg7on/VERITAS/issues/160), [RDX07 / #161](https://github.com/skg7on/VERITAS/issues/161), [RDX08 / #162](https://github.com/skg7on/VERITAS/issues/162).

## Global constraints and review focus

Keep native SVF/Soufflé defaults and `eir.v1` unchanged. New review artifact IDs
are not FactIDs. No arbitrary shell/SQL/file operations, live-tree mutation,
model authority, ambient runner tools or fake production fallback. Preserve
explicit invocation, positive aggregate budgets, immutable snapshots and full
repository verification. Use the exact fields/limits in the spec.

Review focus: captured bytes versus live edits (Task 2); ambiguous link membership
and includes (Tasks 2–3); fake M9 identities for local evidence (Task 4); repeated
requests bypassing aggregate limits (Task 5); invented references or authority
from model/source text (Tasks 5–7). These are adversarial acceptance cases.

## Task 1: Define v2 models, IDs and strict decoding

Baseline `99d3130` has no `plugins/` tree. The following Python/package/host
paths are **Create**, not assumptions about delivered v1 code. Reuse #79's
approved design constraints; do not rerun its EIR-first workflow. This task
also creates package/test `__init__.py` files and import/test entry setup; later
tasks create the backend, controller and host implementation.

**Files:** Create `include/veritas/review/ReviewTypes.h`,
`src/review/ReviewTypes.cpp`, `plugins/veritas-evidence-review/schemas/veritas-review-backend-v2.schema.json`,
and `scripts/veritas_review/review_types.py` beneath that plugin; create
`tests/unit/review/ReviewTypesTest.cpp` and plugin `tests/test_review_types.py`.
Register native test/source CMake directories as part of this task.
**Consumes:** the spec's exact ReviewBinding/ProgramScope/Context/Candidate types.
**Produces:** C++ tagged artifact IDs and Python frozen dataclasses; canonical
serialization and strict `decode_v2(data: bytes) -> BackendEnvelope`.

- [ ] Write `LocalBindingHasNoNativeRun`, `RejectsDuplicateAndUnknownKeys`,
  `ArtifactKindsCannotBeSubstituted`, and `CanonicalIdsIgnoreAuditTiming`.
  Assert local `native_binding is None`; changing program/config changes digest;
  audit timing does not; malformed/authority-bearing extra fields are rejected.
  Add `HandshakeHasNoBinding` and `CaptureUsesTrustedSelectionKeysBeforeRefs`;
  reject a model-provided arbitrary manifest path or post-capture handle as an
  initial operator selection.
- [ ] Run `python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p test_review_types.py`
  and `ctest --test-dir build -R ReviewTypesTest --output-on-failure`; require RED.
- [ ] Implement the exact tagged models, v2 schema and canonical hash codec.
  Reject booleans where integers are required and invalid UTF-8 before parsing.
- [ ] Run the same tests plus v1 schema fixtures; require explicit version
  rejection/negotiation rather than silently accepting v2 in a v1 decoder.
- [ ] Commit the wire contract and fixtures as one reviewable delivery.

## Task 2: Capture immutable snapshots and declared program membership

**Files:** Create `include/veritas/review/SnapshotCapture.h`,
`src/review/SnapshotCapture.cpp`, `ProgramScope.{h,cpp}` in corresponding include/
source directories, and `tests/integration/review/SnapshotCaptureTest.cpp`.
**Consumes:** Task 1 types and existing project-input parsing.
**Produces:** `CaptureReviewSnapshot(const SnapshotRequest&) ->
StatusOr<ReviewSnapshot>`, immutable blob/materialization handles and ProgramScope.

- [ ] Add `DirtySourceIsCapturedOnce`, `UntrackedRequiresExplicitSelection`,
  `OutputHintsDoNotCloseMembership`, `SharedLibraryDependencyIsNotDropped`,
  `TraversalAndSymlinkEscapeAreRejected`. Literal oracle: a captured `before`
  excerpt remains `before` after live content becomes `after`.
- [ ] Build/run `SnapshotCaptureTest`; prove RED against the missing capture path.
- [ ] Capture content/build/include artifacts, validate manifests and record
  OPEN/UNRESOLVED scope where membership is only inferred. Materialize only
  captured blobs for compiler/replay workers.
- [ ] Test library-only, mixed executable/test compile databases, missing headers,
  generated include changes and a file changing during capture; require either
  one coherent captured version or retryable refusal, never a mixed snapshot.
- [ ] Commit with context-artifact identity and no existing ID-chain migration.

## Task 3: Build the structural index and narrow local bounds producer

**Files:** Create `include/veritas/review/{StructuralIndex,LocalEvidence}.h`,
`src/review/{StructuralIndex,LocalEvidence}.cpp`,
`tests/integration/review/LocalEvidenceTest.cpp`, and regression fixtures under
`tests/fixtures/review/local_bounds/`. Reuse existing Clang TU acquisition.
**Consumes:** immutable ReviewSnapshot and ProgramScope from Task 2.
**Produces:** `BuildStructuralIndex(snapshot, budget)` and
`InvestigateLocal(const LocalRequest&) -> StatusOr<LocalEvidence>`.

- [ ] Write `ArrayExtentIsBytes`, `KeepsTwoMemcpySitesSeparate`,
  `SiblingGuardDoesNotDominate`, `MissingHeaderIsPartial`, and
  `PointerExtentIsUnsupported`. Assert 32 bytes for the fixture, operand indices
  0/1/2, and no inferred extent for an ambiguous pointer.
- [ ] Run `ctest --test-dir build -R LocalEvidenceTest --output-on-failure`;
  require RED before implementing the provider.
- [ ] Parse selected TUs without project link/SVF/WPA; implement only the
  registered `copy_bound.v1` forms and emit typed source/provenance/coverage refs.
- [ ] Verify a wchar/multi-byte array uses bytes, macro/indirect forms are
  supported explicitly or refused, and compiler setup/child RSS are counted.
  Assert counters `svf_invocations=0` and `wpa_components_executed=0` on this path.
  Add plugin-load/response-file/unknown-semantic-flag tests: unsupported captured
  flags must refuse execution rather than load code or be silently dropped.
- [ ] Commit the narrow provider without claiming #124 native completion.

## Task 4: Expose the public backend and pinned bounded context

**Files:** Create `src/tools/veritas-review-backend.cpp`,
`include/veritas/review/ReviewBackend.h`, `src/review/ReviewBackend.cpp`,
`tests/integration/review/ReviewBackendTest.cpp`; create plugin `backend.py`
using the approved bounded-process design, including argv/env/group/deadline/
stream-size enforcement rather than assuming that implementation exists.
**Consumes:** Tasks 1–3; verification registry from the qualification plan.
**Produces:** the eight named v2 subcommands and `Backend.call(operation,
parameters, binding) -> BackendResult`; existing v1 EIR adapter stays explicit.

- [ ] Add `HandshakeDoesNotRequireSummaryDb`, `ContextUsesAdvertisedSnapshot`,
  `NoCapabilityDoesNotSelectPractice`, `V1ClientCannotReadV2LocalEvidence`, and
  `OutputOverflowKillsProcessGroup`. Assert exact request/refusal semantics.
- [ ] Run native `ReviewBackendTest` and plugin `test_backend.py`; require RED.
- [ ] Implement strict envelope dispatch, bounded context handles and configured
  native/precision operations using argv arrays and the frozen snapshot.
- [ ] Test wrong request ID/binding, invalid operation, missing native run,
  stdout/stderr floods, backend timeout and a descendant process holding stdout.
  Every failed request must leave published artifacts unchanged.
- [ ] Commit the public service and transport contract; no model-facing MCP
  server is used as the native backend transport.

## Task 5: Implement candidate admission, budgets and durable controller

**Files:** Create plugin `scripts/veritas_review/{models,controller,validation,capabilities}.py`;
create `context.py`, `candidates.py`, `budgets.py`, and
`tests/{test_candidates,test_review_controller,test_budgets}.py`.
**Consumes:** frozen wire models, Backend, ReviewPolicy and semantic annotation
schema from the qualification plan. **Produces:** `start_review`,
`admit_candidates`, `advance_review` and durable ReviewState per the spec.

- [ ] Add tests for LLM/deterministic duplicate admission, an unregistered
  semantic annotation without fake EIR, stale context, invented ID, model
  `VERIFIED_SAFE`, 9th turn after limit 8, and retry of an already executed request.
  Assert backend side effects do not occur on rejection or duplicate retry.
  Add a novel LLM source-span proposal with no entity ref: resolve exactly one
  advertised callsite, retain unresolved/ambiguous alternatives, and refuse an
  invented index ordinal before any local-evidence request.
- [ ] Run `python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p 'test_*controller*.py'`
  plus candidate/budget tests; require RED on the missing behavior.
- [ ] Implement the exact state machine, pre-dispatch reservation, aggregate
  accounting and failure-atomic audit/checkpoint with source text treated as data.
- [ ] Verify 16-candidate/64-KiB response limits, missing tokenizer/usage policy,
  crash after backend completion, denied/deferred coverage and capability reuse.
- [ ] Commit one controller implementation shared by all host adapters.

## Task 6: Integrate the plugin host and deterministic reports

**Files:** Create the approved plugin manifest, `.mcp.json`, executable wrapper,
isolated runner and manual skill/references, plus `cli.py`, `mcp_server.py`,
`reporting.py`, response schemas and process tests. Use baseline #79 Tasks 1,
5–7 for packaging/capability/process/host requirements where compatible; the v2
task owns their real implementation and tests, not a second controller.
**Consumes:** Task 5 controller; no duplicate host-specific admission logic.
**Produces:** amended `start`/`advance`, `review-report.v1`, Markdown renderer and
explicit v1/v2 host readiness.

- [ ] Test a real start/advance subprocess transcript with pre-case context,
  an admitted semantic annotation and local evidence. Assert exactly the two
  plugin tools, no ambient shell/files, and identical semantic JSON for identical
  admitted input despite different audit occurrence IDs.
- [ ] Run plugin unittest/process suites and manual-skill behavioral evaluation;
  show the old forced-EIR-first flow cannot satisfy this transcript.
- [ ] Implement host delegation and report categories/coverage; use existing
  runner capability and output path restrictions, retaining visible practice mode.
- [ ] Test unsafe Markdown/source content, empty truncated discovery, no candidate,
  relocation and unsupported host capability; none certifies safety or falls back.
- [ ] Commit the adapter after schema/skill/runner tool names agree.

## Task 7: Qualify the first real review

**Files:** `tests/integration/review/ReviewReleaseTest.cpp`, plugin production
contract tests, and the frozen corpus/evaluation artifacts from the qualification
plan. **Consumes:** Tasks 1–6 plus real replay, annotation and corpus deliveries.
**Produces:** a release acceptance record for the explicit capability profile.

- [ ] Run the pinned real base/head review: require a deciding analytical replay,
  base rejection, a cited release/callback observation and correct negative
  controls. Practice fixtures or an all-inconclusive report do not satisfy it.
- [ ] Run A/B/C discovery and fixed-candidate qualification before tuning deeper
  analyzers; record missed candidates and model-run variance.
- [ ] Verify local-only mode needs no native store; native-EIR mode additionally
  requires the native integrity gate. Unsupported predicates remain unavailable.
- [ ] Run clean native build/CTest, plugin tests, schema/protocol/host evaluation,
  documentation and license checks; inspect skips explicitly.
- [ ] Commit the acceptance record and update only the issues actually delivered.

### RDX07 / #161 execution qualification (2026-10-30)

Runtime Task 4 and verification-plan Task 1 are delivered. The public C++
resident subprocess supports strict v2 handshake/capture/context/seed/local/
native/verify/release dispatch. Capture resolves trusted operator selection keys,
keeps immutable source/index handles, and publishes only after output validation.
The Python transport uses no shell, scrubs its environment, bounds live streams,
charges per-call/aggregate wall time and samples RSS/processes. POSIX session
cleanup includes the compiler's separate process group. No SummaryDB is required
for local mode, and no local artifact is converted into a native FactID/run.

Verification codecs and C++/Python authority validation bind the obligation,
request/result identities, exact snapshot, typed operands, domain, admitted
assumptions and immutable input/witness bytes. Producer names alone cannot
promote a disposition; a separately admitted exact-result receipt is required.
Concrete PROVED means no defect observed on that input, never universal safety.

Observed RED before implementation: missing public backend and verification
headers/modules. Adversarial RED→GREEN regressions also cover lowered structural
work limits, non-null revision targets, separate compiler groups surviving a
transport deadline, echoed requests being mistaken for responses, repeated
handshake dropping active capabilities and release inventing a capability.
An independent whole-branch review found three Important issues (group cleanup,
response-envelope discrimination, repeated negotiation), all reproduced and fixed;
no Critical or Minor findings remain deferred.

Final clean Debug verification used Homebrew Clang 20.1.8 with LLVM/Clang 24 and
pinned SVF/Souffle. Configure/build:

```sh
cmake --preset default -DCMAKE_C_COMPILER=/opt/homebrew/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/clang++ -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --clean-first -j 8
ctest --test-dir build --output-on-failure --output-junit /tmp/rdx07-final-ctest.xml
python3 -m unittest discover -s plugins/veritas-evidence-review/tests -v
```

The clean build completed **589/589 steps**, zero errors and the existing
`libveritas_analysis_semantic.a` duplicate-library linker warning. Full CTest
passed **999/999** in **372.68 s**, zero failures/errors/skips. JUnit and internal
GoogleTest skip-marker inspection found zero skips; the native engine-agreement
test ran. The Python suite passed **37/37**. Modified/new source license headers
and staged/unstaged whitespace checks passed. The runtime process tests require
OS process-table access; sandbox-denied sampling was not accepted as flood or
timeout evidence.

Scope remains deliberately narrow: only capture/context/seed/local/release are
advertised. Native evidence and production verification return UNSUPPORTED until
qualified providers exist. Real sanitizer replay/enforcing isolation, controller,
host and end-user release qualification remain #163/#162/#79/#165. Source/diff
windows over the configured limit retain explicit omissions; arbitrary revision
targets and FORMAL/universal safety are unavailable. This does not discharge
native domain producers under #124 or change SVF/Souffle/EIR authority contracts.

### RDX08 / #162 controller execution qualification (2026-10-31)

Runtime Task 5, Task 6's shared API/report/schema portion and verification
Task 3's P1 annotation portion are delivered. #79 retains the isolated plugin
host/manifest/runner/MCP adapter and its authenticated host qualification.
The public controller API is `start_review`, `admit_candidates`, `advance_review`
and `render_report`; every host uses the same implementation.

Candidate admission precedes EIR, resolves exact advertised source spans,
deduplicates semantic callsite anchors across origins/context kinds, and retains
rejected/deferred coverage. Source/test/doc handles are pinned and bounded.
Eight model turns, sixteen candidates, response/context/input limits and
aggregate backend allowances apply to the entire attempt. Without a tokenizer,
actual controller prompts are charged at one token per UTF-8 byte, or the host's
larger measured usage. Missing usage prevents another turn; model responses
cannot raise limits or claim deterministic provenance or proof authority.

Operation/ref/limit pairs are exact per-turn grants. Pre-dispatch reservations,
process limits and successful actual-usage reconciliation include initialization
and repeated calls. The hash-chained WAL and atomic fsynced checkpoint preserve
replay receipts. Concurrent identical request IDs return one stored response;
conflicting IDs or reused capabilities refuse before backend execution. A crash
around completion never silently reexecutes uncertain work. Normal audit writes
reserve 1 KiB for a compact durable abort record, including response-rejection
exhaustion. Unvisited advertised contexts remain terminal coverage gaps.

`review-report.v1` preserves local evidence and supporting/contradicting refs;
Markdown escapes source/model text. Semantic report identity excludes measured
costs, timing, occurrence IDs and capabilities. P1 `semantic-contract.v1`
annotations preserve unregistered release/callback concerns and source/test/doc
intent with PROPOSED/CONDITIONAL status, dependency digests and no verification
ref. Source/doc/model changes invalidate reuse. There is no EIR fact promotion,
formal contract store or concurrency proof.

Observed RED before implementation: missing candidate, policy/budget and
controller modules. Behavioral RED→GREEN regressions cover stale/invented refs,
model authority/source instruction strings, exact-span ambiguity, duplicate
origins/sites, ninth-turn and missing usage, conservative input accounting,
aggregate context windows, actual output/work reconciliation, concurrent retry,
and crash after backend completion. The real bounded C++ resident service was
used for start/advance, immutable source context, a compiler-observed 32-byte
extent, local evidence, retry, annotation and terminal report. The regression
fixture is not a qualified production change corpus.

Independent whole-branch review found four Important issues: a malformed
assessment escaping durable model accounting; audit exhaustion preventing abort
recovery; missing unvisited-context gaps; and model-forged deterministic origin.
Each was reproduced and fixed RED→GREEN. A repeated-site multi-anchor proposal
was also fixed so one semantic callsite is not misclassified as ambiguous.
The audit fix additionally covers exhaustion while rejecting a model response.
No review findings remain deferred.

Scope decisions: repository worktree placement and the `codex/` branch prefix
follow project/desktop instructions. The approved design/plan was executed
without restarting design approval. #79's host adapter is not duplicated here;
#163 owns qualified replay, #164 corpus freezing before tuning, and #165 release
qualification. No tuning was performed. Backend v2 has no live-tree-advancement
operation: reports explicitly retain that unknown instead of inventing a result.
Continuing a nonterminal attempt requires its original resident backend; recovery
does not serialize native pointers or recapture a changed tree. These limits
require downstream host/replay/qualification work and leave live advancement
unassessed. Native SVF/Souffle and EIR authority contracts remain unchanged.

Final verification used Homebrew Clang 20.1.8 with LLVM/Clang 24 and pinned
SVF/Souffle in the isolated `codex/issue-162-review-controller` worktree:

```sh
cmake --preset default -DCMAKE_C_COMPILER=/opt/homebrew/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/clang++ -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --clean-first -j 8
ctest --test-dir build -j 8 --output-on-failure --output-junit /tmp/rdx08-final-ctest.xml
```

The final clean build completed **589/589 steps**, zero errors and one existing
`libveritas_analysis_semantic.a` duplicate-library warning. Full CTest passed
**999/999** in **64.40 seconds**, including **76/76** Python runtime tests.
JUnit and internal GoogleTest output show zero skips; the native
`WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts` test ran and passed.
Changed source license headers and staged/unstaged whitespace checks passed.
OS process-table access was allowed for the process/RSS tests; sandbox refusal
was not accepted as deadline/output-limit evidence. Primary `main` remained
clean throughout. This is controller qualification, not #165 production release
qualification, and does not close #79/#124/#163/#164/#165.
