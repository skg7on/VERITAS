# Verification and Review Qualification Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline or
> superpowers:subagent-driven-development when selected; P4 adapters remain
> evidence-gated rather than mandatory P1 prerequisites.

**Goal:** Establish one real deciding analytical outcome, useful non-authoritative
semantic observations, and reproducible evidence for further investment.
**Architecture:** Registered deterministic producer authority outside the model,
typed contract annotations and a frozen A–E evaluation corpus.
**Tech Stack:** C++20/Clang sanitizer replay, Python 3.11+ standard library,
GoogleTest/unittest, JSON manifests; configured Z3/other adapters only when qualified.
**Spec:** [verification/qualification](../specs/veritas-review-verification-qualification-design-spec.md).

**Work issues:** [RDX07 / #161](https://github.com/skg7on/VERITAS/issues/161), [RDX09 / #163](https://github.com/skg7on/VERITAS/issues/163), [RDX10 / #164](https://github.com/skg7on/VERITAS/issues/164), [RDX11 / #165](https://github.com/skg7on/VERITAS/issues/165), [RDX16 / #170](https://github.com/skg7on/VERITAS/issues/170).

## Global constraints and review focus

Replay proves the observed execution only; it never proves universal safety.
No model confidence/agreement or deterministic-but-unauthorized response creates
authority. Freeze evaluation before tuning; preserve unsupported/incomplete states.
Use existing runtime process/capability limits and snapshot binding.

Review focus: copied proof under another binding (Task 1); unrelated sanitizer
crash (Task 2); circular inferred contract (Task 3); synthetic-only corpus and
discovery misses (Task 4); setup costs hidden by warm-only metrics (Task 5).

## Task 1: Implement obligation identity and authority transitions

**Files:** Create `include/veritas/review/Verification.h`,
`src/review/Verification.cpp`, plugin `scripts/veritas_review/verification.py`,
`tests/unit/review/VerificationTest.cpp` and plugin `tests/test_verification.py`.
**Consumes:** lightweight plan Task 1 ReviewBinding/types and trusted producer policy.
**Produces:** `ValidateVerificationResult(request, result, policy) -> Status` and
`ApplyVerification(state, request, result) -> ReviewDisposition` per the spec.

- [ ] Test wrong binding/predicate/operand, unregistered producer, altered input
  digest, circular premise and timeout. Assert each is rejected or non-deciding;
  an exact registered certificate alone may set a deciding disposition.
- [ ] Run VerificationTest and test_verification.py for RED before implementation.
- [ ] Implement canonical request/result identity, immutable dependencies and the
  sole authority transition; keep local report and native EIR state separate.
- [ ] Verify unknown/incomplete/error distinctions, same-result replay and a
  purported universal-safe result with an empty/undefined domain.
- [ ] Commit without claiming a production verifier exists before Task 2 passes.

## Task 2: Implement real snapshot-bound sanitizer replay

**Files:** Create `src/review/AsanReplay.cpp`,
`include/veritas/review/AsanReplay.h`, registered harness fixtures and
`tests/integration/review/AsanReplayTest.cpp`; wire the backend `verify` operation.
**Consumes:** VerificationRequest, captured source/build context and trusted harness ID.
**Produces:** `RunAsanReplay(const VerificationRequest&) ->
StatusOr<VerificationResult>` for `asan-replay.v1`/`copy_bound.v1` only.

- [ ] Write `ModifiedCopyOverflowsAndBaseRejects33`, length 0/32/33 and short
  source controls, `OtherCrashIsNotThisObligation`, `MissingRuntimeIsUnsupported`,
  and `WrongSiteCannotVerify`. Require real compiler/sanitizer execution.
  Add `GlobalInitializerCannotWriteHostCanary`, `NetworkIsDenied`,
  `CapturedPluginFlagIsRejected`, and `NoIsolationProfileIsUnsupported`; these
  exercise the actual configured enforcement runtime before replay is advertised.
- [ ] Run AsanReplayTest for RED; fake transcript fixtures may test parsing but
  cannot pass the real qualification case.
- [ ] Implement allowlisted compiler/harness execution in private snapshot input,
  an enforcing write/network/credential isolation profile, bounded process/output
  management and precise sanitizer/site matching. Private cwd is not confinement.
  Match REFUTED to a real bound violation; a non-deciding passing replay is UNKNOWN.
- [ ] Verify missing dependencies, child process leaks, cancellation and replay
  artifacts/digests. The base's rejection is a negative control, not global safety.
- [ ] Commit with the actual deciding execution record and remaining assumptions.

## Task 3: Implement semantic annotations and reusable contract artifacts

**Files:** Create plugin `scripts/veritas_review/contracts.py`,
`schemas/veritas-semantic-contract-v1.schema.json`, `tests/test_contracts.py`,
and later native `src/review/ContractRepository.cpp` when P4 reuse is funded.
**Consumes:** source/test/doc context and model proposals; independent verifier refs.
**Produces:** `admit_contract(proposal, state) -> ContractAnnotation` for P1 and
versioned semantic-contract artifacts with the exact statuses/origins in the spec.

- [ ] Test release/callback observation with missing cancellation, correct control,
  missing documented intent, invalid refs and an invariant used as its own proof.
  Assert PROPOSED/CONDITIONAL for inferred intent and no native FactID emission.
- [ ] Run `python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p test_contracts.py`
  for RED before adding contract admission.
- [ ] Implement bounded annotation admission and dependencies; keep P4 persistent
  promotion separate from the P1 report annotation and formal summary store.
- [ ] Verify source/doc/model changes invalidate reuse, agreement does not promote,
  and a real independent trace remains scoped to its observed schedule.
- [ ] Commit P1 annotations; leave persistence/formal adapters pending their P4 gate.

## Task 4: Freeze corpus, negative controls and adjudication rubric

**Files:** Create `tests/qualification/review/corpus.json`, `rubric.md`, regression
fixtures and `tools/review_eval/validate_corpus.py` with unittest coverage.
**Consumes:** real base/head changes, explicit program/build manifests and digests.
**Produces:** immutable corpus release, held-out split and known-defect/clean oracles.

- [ ] Test rejection of short/nonexistent revisions, mismatched file digests,
  missing program membership, missing real case in either first-release family,
  and a supposedly negative control that still violates its source contract.
- [ ] Run `python3 -m unittest discover -s tools/review_eval/tests -p test_corpus.py`
  for RED before implementing the corpus validator.
- [ ] Select/license and pin real local-bounds and release/callback changes plus
  clean controls; verify they build/replay under their declared harness. Do not
  invent revision identities; record a corpus gap if selection fails.
- [ ] Freeze thresholds/splits before tuning and distinguish fixed-candidate from
  whole-discovery runs. Validate all manifests and publish their exact digests.
- [ ] Commit the corpus/rubric; this is a prerequisite for production qualification,
  not proof that the model has detected its cases.

## Task 5: Measure A–E quality, costs and release gates

**Files:** Create `tools/review_eval/{run,evaluate,report}.py`, tests and
`benchmarks/review/results/` measurement artifacts created by the actual implementation.
Use existing RunMetrics/M13/M14 instruments for native measurements.
**Consumes:** frozen corpus, controller/backend and provider profiles A–E.
**Produces:** machine-readable per-attempt results, distributions and go/no-go record.

- [ ] Write tests where a fixed-candidate verifier succeeds but discovery misses
  the known bug, where no real replay decides, and where warm-only accounting
  omits expensive bootstrap. Each must fail qualification despite a plausible report.
- [ ] Run the evaluator's unittest suite for RED before its implementation.
- [ ] Implement paired fixed-budget runs, separate cold/warm/setup/model costs,
  aggregate child/parent memory and per-layer bytes; unavailable capabilities
  remain explicit. Use >=100 query/attempt samples for P95 and >=3 full runs.
- [ ] Compare real A/B/C results first, then fund D/P4 only with adjudicated value.
  Preserve #142's separate scaling thresholds and report model variability/misses.
- [ ] Run complete repository/plugin verification and commit the measured decision
  record. Pending experiments stay pending; do not fill targets with predictions.

### RDX07 / Task 1 qualification (2026-10-30)

Task 1's concrete-input obligation/request/result identity and exact authority
boundary are delivered in C++ and Python. Runtime Task 4 exposes the closed
verify operation with explicit UNSUPPORTED while no production producer is
qualified. The [runtime execution record](veritas-lightweight-review-runtime-implementation-plan.md#rdx07--161-execution-qualification-2026-10-30)
records RED→GREEN evidence, independent review fixes, 589-step clean build,
999/999 full CTest with no skips and 37/37 Python tests. The test-only registered
receipt fixture does not establish production verification. Task 2's enforcing
runtime, real sanitizer execution and deciding release acceptance remain pending.
