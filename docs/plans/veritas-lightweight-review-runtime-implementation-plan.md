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
