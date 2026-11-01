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

- [x] Write `ModifiedCopyOverflowsAndBaseRejects33`, length 0/32/33 and short
  source controls, `OtherCrashIsNotThisObligation`, `MissingRuntimeIsUnsupported`,
  and `WrongSiteCannotVerify`. Require real compiler/sanitizer execution.
  Add `GlobalInitializerCannotWriteHostCanary`, `NetworkIsDenied`,
  `CapturedPluginFlagIsRejected`, and `NoIsolationProfileIsUnsupported`; these
  exercise the actual configured enforcement runtime before replay is advertised.
- [x] Run AsanReplayTest for RED; fake transcript fixtures may test parsing but
  cannot pass the real qualification case.
- [x] Implement allowlisted compiler/harness execution in private snapshot input,
  an enforcing write/network/credential isolation profile, bounded process/output
  management and precise sanitizer/site matching. Private cwd is not confinement.
  Match REFUTED to a real bound violation; a non-deciding passing replay is UNKNOWN.
- [x] Verify missing dependencies, child process leaks, cancellation and replay
  artifacts/digests. The base's rejection is a negative control, not global safety.
- [x] Commit with the actual deciding execution record and remaining assumptions.

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

- [x] Test rejection of short/nonexistent revisions, mismatched file digests,
  missing program membership, missing real case in either first-release family,
  and a supposedly negative control that still violates its source contract.
- [x] Run `python3 -m unittest discover -s tools/review_eval/tests -p test_corpus.py`
  for RED before implementing the corpus validator.
- [ ] Select/license and pin real local-bounds and release/callback changes plus
  clean controls; verify they build/replay under their declared harness. Do not
  invent revision identities; record a corpus gap if selection fails.
  Selection, licensing, revision pinning, artifact/license digests and upstream
  git-object validation are delivered. The operator-qualified replay execution
  remains #165, so this bullet stays open and the families keep
  `production_qualification: OPEN`.
- [x] Freeze thresholds/splits before tuning and distinguish fixed-candidate from
  whole-discovery runs. Validate all manifests and publish their exact digests.
- [x] Commit the corpus/rubric; this is a prerequisite for production qualification,
  not proof that the model has detected its cases.

## Task 5: Measure A–E quality, costs and release gates

**Files:** Create `tools/review_eval/{run,evaluate,report}.py`, tests and
`benchmarks/review/results/` measurement artifacts created by the actual implementation.
Use existing RunMetrics/M13/M14 instruments for native measurements.
**Consumes:** frozen corpus, controller/backend and provider profiles A–E.
**Produces:** machine-readable per-attempt results, distributions and go/no-go record.

- [x] Write tests where a fixed-candidate verifier succeeds but discovery misses
  the known bug, where no real replay decides, and where warm-only accounting
  omits expensive bootstrap. Each must fail qualification despite a plausible report.
- [x] Run the evaluator's unittest suite for RED before its implementation.
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
receipt fixture does not establish production verification.

### RDX09 / Task 2 qualification (2026-11-01)

Verification-plan Task 2 is delivered. `asan-replay.v1` is a registered,
operator-configured producer for `copy_bound.v1`, wired through backend v2
`verify`. It compiles the admitted callsite's captured translation unit and a
registered trusted harness with the configured compiler's AddressSanitizer
runtime, and executes only the registered concrete payload. No request or model
field selects a compiler, command, predicate or authority; `harness_id` selects
only among operator-registered harnesses, and the site/size gate binds the
execution to the admitted callsite.

Enforcement: the operator selects `macos-sandbox` (sandbox-exec profile) or
`linux-bwrap` (bubblewrap). Each replay qualifies the profile with real canaries
for a host write outside the disposable tree, a read of a host credential, and
a loopback connection to a host listener. An allowed canary, an unavailable
compiler/sanitizer runtime/harness/payload, or an empty isolation profile
returns UNSUPPORTED before any captured source is compiled or run; there is no
private-cwd-only fallback. Captured plugin/response-file/explicit-target
compiler flags are refused before execution. Child processes run in their own
process group with wall/output/RSS/process bounds, are killed with the group on
exhaustion, and are reaped.

REFUTED requires a sanitizer buffer-overflow WRITE of exactly `requested_bytes`
at the admitted callsite's unambiguous file basename and line, with
`requested_bytes > capacity_bytes`. The evidence is a canonical facts witness
(no addresses/PID/transcript), so identical admitted inputs produce the same
verification ID across disposable roots; the raw sanitizer/compile logs and
executable bytes are retained as non-dependency artifacts. The executable
identity normalizes the disposable root path, the platform linker's
UUID/signature/link-edit metadata and ELF build-id notes, and materialized
source mtimes (ELF DWARF line tables can record them).

RED→GREEN evidence: the initial fifteen `AsanReplayTest` cases all failed
against an unimplemented producer, then passed against the real sanitizer
execution. The content-address stability case failed until the executable
identity was normalized, and the backend-protocol case failed until the verify
operation was wired. The modified fixture with the destination bound removed and
a 33-byte request is REFUTED at the admitted `memcpy` line; the base fixture
rejects the same input and is UNKNOWN. Length 0/32 are UNKNOWN. Wrong site, unrelated crash, short
payload, wrong snapshot, captured plugin flag, timeout, missing
compiler/runtime, and no isolation profile are non-deciding. The untrusted
global initializer cannot write a host canary, read a host credential, or reach
a host network listener. The decisive result passes `ValidateVerificationResult`
only with its own witness and an exact authorized result ID, and
`ApplyVerification` is the sole transition to VERIFIED FINDING.

Independent review of the first implementation found and fixed: ELF/arch-less
sanitizer frame parsing (the Darwin-only `:arch` form had made every Linux
decision impossible); symbolization and executable hashing now use an immutable
pre-run image copy rather than a work-tree file the target can replace; a
sanitizer-like transcript requires the sanitizer abort status and summary (a
forged report with a clean exit is non-deciding); captured standard, language
and include-kind semantics now match the index; every subprocess is bounded by
the caller's remaining aggregate deadline; macOS denies host signals while
allowing self-abort; detached descendants are sampled and killed on a
best-effort basis (a child that reparents between samples can escape) and
inherited descriptors are closed; cleanup never follows symlinks created by the
target;
and every denied root (including the live checkout) is canaried, with bubblewrap
masking deny roots. The write canary judges the host-side effect rather than the
child's report, because a private tmpfs can accept the path without touching the
host; each canary probe must also exit successfully. The harness is compiled
without captured macros/include dirs, and local-evidence coverage must be
complete before replay.

Remaining assumptions: the sanitizer runtime is linked into the untrusted
process, so a target that deliberately forges the sanitizer channel *and*
aborts remains indistinguishable from a real report without a trusted external
observer; the abort/summary gate rejects the clean-exit forgery, and an
out-of-process observer remains future work. Replay executes on the configured
compiler's native target, while the structural index uses its portable
x86_64-linux analysis target; the harness re-checks the admitted extents on the
replay target, but target-conditional source is a documented limitation.
`harness_id` is request-supplied by contract, so the wire selects among
operator-registered harnesses only. The wire pins dependency digests but has no
artifact-fetch operation, so independent Python-side authority admission of a
backend result remains future host work.

Scope: replay never emits PROVED and never establishes universal safety; base
rejection is a control. No FORMAL schema, native `EvidenceCase` promotion or
real-change release corpus is delivered here. Explicit captured `--target`
values are refused because the analysis target is not assumed to be the replay
host. Linux bubblewrap execution is exercised in CI; a host that cannot create
the namespace returns UNSUPPORTED. Corpus freezing and release qualification
remain #164/#165.

Targeted verification: 22/22 AsanReplayTest, 69/69 review integration tests,
15/15 review unit tests, and 76/76 Python runtime tests.

Final local verification used Homebrew Clang 20.1.8 with LLVM/Clang 24 and the
pinned SVF/Soufflé in the isolated worktree:

```sh
cmake --preset homebrew-clang -DLLVM_PROJECT_BUILD_DIR=<llvm-build>
cmake --build --preset homebrew-clang --clean-first -j 8
ctest --test-dir build-homebrew-clang --output-on-failure \
  --output-junit /tmp/rdx09-final-ctest.xml
```

The clean build completed **591/591** steps with zero errors and the existing
`libveritas_analysis_semantic.a` duplicate-library linker warning. Full CTest
passed **1023/1023** in **391.19 s** with zero failures and zero skipped/not-run
cases. `git diff --check`, clang-format and license-header checks passed. The
Linux coverage build exposed one portability defect in the canary code — the
socket declarations were only reaching it transitively on macOS — and the
headers are now included directly; the final clean build/CTest above includes
that fix. The Linux bubblewrap/ASan qualification is exercised by CI, not this
local run.

### RDX10 / Task 4 qualification (2026-11-01)

Verification-plan Task 4 is delivered. `tests/qualification/review/corpus.json`
freezes `rdx10-real-change-2026-11-01`; `rubric.md` is the frozen adjudication
contract, and `tools/review_eval/validate_corpus.py` enforces the release. The
frozen release digest is
`sha256:a939cac2a225ba39f037e51aa8d9fa98c4acb3850c6545bf53ed9e7a73cb5bb1`;
the split digest is
`sha256:da9784cfbbc9484379606f5b1f77a888167d2b31ef9c3375fe19bd15e1089ae9`;
the rubric digest is
`sha256:1521c2347970fc61d8fe5e7359fb3f37f80d9007590ca61726711627594b4910`.

Real families: local-bounds pins libuv
`e7b3c3fb6b56298174e5eaf2ac3bea2b721cf0f5` (`char tmp[sizeof
"255.255.255.255"]`; `memcpy(tmp, curtok, pch - curtok)` in `src/inet.c`) to
`4fac9427e4a652cdebf241803ce7443dd59a4451` (bounded scope-id copy).
Release/callback pins `e8458b2402e7a068f25f01d0b72e6aed73ca2138` to
`a7c44d674833e8dec66b8730b5635fcad0f81162` (`uv__udp_sendmsg` after a recv
callback closes the handle) and the independent
`e62ccbf7707f62b8056a963b6bd01507fbf02817` to
`b030b02a3e51acd09e260bf20e115e841c3842d0` POLLERR sink. Every case pins the
MIT license and source/test/build artifact digests. N=16/N+1=17 and a
short-source payload are frozen; the guarded head, the closing-handle head and
an unrelated 1.50.0 version bump are the clean controls. The guarded controls
and all real defects are held out; only the unrelated version bump is tuning.
The existing `asan_replay` fixtures are regression companions, not real cases.

The validator rejects short/non-commit revisions, changed paths outside the
base..head diff, artifact/local digest mismatches, a callsite outside program
membership, a family without a real case or its required clean controls, a
control whose witness does not contain its guard, a tuning case that reuses a
held-out revision pair/path set, split leakage, stale rubric/split/release
digests, a semantic observation promoted without an independent reference, a
short-source control that could decide, a non-`memcpy` `copy_bound.v1`
callsite, and a missing fixed-candidate/whole-discovery mode. D/E remain
explicitly UNAVAILABLE.

RED→GREEN: the first unittest run failed all 22 original cases with
`NotImplementedError`. The final suite passes 30/30, including a temporary-git
fixture for the revision/diff/digest/witness/split rules and the real corpus.
`validate_corpus.py --content-root . --repository-root <libuv clone>`
additionally accepted every real revision, diff, artifact digest, callsite and
anchor against the pinned libuv objects. An independent review found and the
delivery fixed: the registered harness accepts the producer's `{program,
payload, requested_bytes}` argv contract and re-checks the prefix length (with
a regression test); the guarded controls moved out of the tuning split;
`SHORT_SOURCE` is required and must be non-deciding; per-family clean controls
are mandatory; control/sink references are typed; and the CTest wrapper fails
if test discovery is empty.

Targeted CTest: `ReviewCorpusValidatorTest` and `ReviewCorpusContentTest`
passed 2/2. Full clean build/CTest: **1025/1025** in **406.56 s** with zero
skipped/not-run cases.

Replay remains `NOT_RUN`: the corpus freezes the trusted harness
(`harnesses/libuv_inet_pton.cc`) and three payloads, but operator-qualified
replay execution is #165; the harness/payload digests are published and
production qualification stays OPEN. No model was tuned, no A–E measurement or
universal-safety claim is made, and no native Evidence state is changed.

### RDX11 / Task 5 evaluator infrastructure (2026-11-01)

The operator-adapter runner, independent response-bound adjudication, A–E
reporting and CTest regression integration are implemented in
`tools/review_eval/`. The suite was observed RED against unimplemented entry
points, then GREEN. It covers a discovery miss despite fixed-candidate success,
all-inconclusive output, missing cold/bootstrap costs, changed result bytes,
independent reviewer binding, insufficient samples, nonce separation and bounded
subprocess execution. See [execution contract](../../tools/review_eval/README.md).

This is a partial delivery. The committed
`benchmarks/review/results/rdx11-decision.{json,md}` is generated from the actual
pending runner output: **OPEN, zero attempts**. It contains no measured model,
replay or performance claims. Direct API authentication, real A/B/C controller
adapters and bounded contexts, independent adjudication, per-layer/native instruments, real deciding replay/controls and
fresh deterministic build measurements remain pending. The user approved
OpenAI gpt-6.1-sol at medium reasoning, public-libuv-only disclosure and a $5
smoke cap; the full paid evaluation is not authorized. The user then explicitly
deferred model runs and requested infrastructure/CI completion. D/E are UNAVAILABLE.
Neither issue #165 nor runtime Task 7 is complete; their acceptance checkboxes
remain open. P4 investment and #142/#145 thresholds are unchanged.

Infrastructure verification: clean Homebrew Clang/LLVM 24 build **591/591**;
full CTest **1026/1026** in **124.01 s** (four workers), with zero skipped/not-run cases and
zero internal GoogleTest skips; evaluator/corpus Python **56/56**, including
26 evaluator regressions; plugin Python **76/76**. Independent review found
and the delivery fixed sample relabeling, observed-memory scoring,
positive-case recall/miss accounting and semantic-versus-analytical deciding
counts. Separate compiler process groups are cleaned up; trusted adapters that
daemonize into new sessions remain unsupported, and enforcing containment is
required for production. The report unconditionally retains OPEN while the
production release validator is unavailable. Diff and license checks pass.

A further RED→GREEN regression rejects a supported-positive/unproven-positive/
missed-defect judgment on a frozen clean control before it can inflate quality
metrics. The clean build and complete CTest suite were repeated after this fix;
the final verification counts above include it.
