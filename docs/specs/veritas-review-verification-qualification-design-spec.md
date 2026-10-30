# Review Verification, Semantic Contracts, and Qualification

**Status:** Detailed P0/P1 release gates and conditional P4 expansion.
**Plan:** [verification and qualification](../plans/veritas-review-verification-qualification-implementation-plan.md).

## 1. Verifier boundary

`VerificationRequest` contains exactly `binding`, `obligation_id`,
`predicate_kind`, `typed_operands`, `input_artifact_ids`, `assumptions`,
`execution_domain`, `backend_id`, `backend_version`, `budget`. Its content
address excludes timing and occurrence data. A registered producer, not the
model, chooses executable/tool configuration from trusted policy.

`VerificationResult` contains the exact request digest and binding,
producer/configuration identity, status, decided proposition/domain,
certificate/counterexample artifacts, input dependencies and operational
outcome. Status is PROVED, REFUTED, UNKNOWN, INCOMPLETE or ERROR. TIMEOUT,
UNSUPPORTED and cancellation are operational outcomes, never a deciding proof.

For P1 the only executable predicate is `copy_bound.v1`, whose proposition is
`requested_bytes <= capacity_bytes` for the declared concrete execution. Its
closed operands are `{kind:"copy_bound.v1", callsite_ref:EntityRef,
destination_ref:EntityRef, source_ref:EntityRef, length_ref:EntityRef,
capacity_bytes:uint64, requested_bytes:uint64, readable_bytes:uint64,
payload_digest:string, harness_id:string}`. Validate the numeric extents against
compiler/source artifacts and target width; the harness must have at least the
declared readable bytes. `execution_domain={kind:"CONCRETE_INPUT", snapshot_id,
input_digest, harness_id}` for replay. Future FORMAL domain/predicate schemas
require separate registration; arbitrary predicate text is not an executable API.

Transition table after all binding/authority/witness checks:

| Status/domain | Required meaning | Allowed disposition |
| --- | --- | --- |
| REFUTED / CONCRETE_INPUT | Matching observed out-of-bounds site violates the bound | VERIFIED FINDING for this execution |
| PROVED / CONCRETE_INPUT | Bound holds for this one admitted input | No defect observed on this input; never universal VERIFIED SAFE |
| PROVED / registered FORMAL | Exact bound holds over a covered, non-vacuous admitted domain | Scoped verified-safe disposition only for that registered capability |
| REFUTED / registered FORMAL | Valid feasible counterexample to the exact bound | VERIFIED FINDING with the counterexample/domain |
| UNKNOWN / INCOMPLETE / ERROR | No deciding proof | Preserve concern/observation/inconclusive; no authority promotion |

A passing replay that did not independently check the bound returns UNKNOWN,
not PROVED. Base rejection is a separate control result, not a universal proof.

`ValidateVerificationResult(request, result, policy) -> Status` checks exact
binding/predicate/operands, version/authority registration, dependency content,
non-circular assumptions, and required witness/certificate. A valid JSON object
from an unregistered executable does not establish authority. Replaying a result
against another snapshot or changing the predicate is refused.

`ApplyVerification(state, request, result) -> ReviewDisposition` is the only
controller transition into VERIFIED FINDING. Universal VERIFIED SAFE is
reserved for a configured formal backend with a defined execution domain,
admitted assumptions and complete/non-vacuous coverage. No current replay
result establishes universal safety. Native `EvidenceCase` state changes require
their own authority-validating adapter; local report state is not written back
into native EIR merely to make the enum look consistent.

## 2. Decisive P1 replay

Register `asan-replay.v1` for `copy_bound.v1`, on POSIX with trusted compiler
and sanitizer runtime. Input is the captured source/build context plus a
registered harness ID and typed bytes/length, not arbitrary model-authored C++
or command strings. Compile/run in a private disposable snapshot directory;
use argv allowlists, process groups, bounded output and the controller limits.
No live-tree mutation or remote execution is part of this contract.

Private cwd and argv limits do not confine untrusted C++ or global initializers.
Compilation and execution require an operator-configured **enforcing isolation
profile**: read-only captured inputs/toolchain, writes only to disposable output,
network denied, no access to host credentials/live checkout, bounded child
processes, and cleanup/reaping. The trusted runner is chosen from policy, never
from source/model text. Qualify the profile with filesystem/network canaries
before advertising replay authority. If no such runtime is available, return
UNSUPPORTED before compiling/running; there is no unconfined fallback. A concrete
OS/container runner may be selected during deployment, but this task neither
installs one nor claims private directories alone provide those guarantees.
Captured compiler flags are also admitted under the runtime spec's policy;
no arbitrary compiler plugin/response-file execution is permitted.

Qualification fixture: 32-byte local destination, 33 readable input bytes,
requested length 33. Modified code retains the source-extent check but removes
the destination bound; base rejects that request. Require sanitizer evidence
that names the exact admitted site in the modified snapshot and a successful
base rejection control. Preserve compiler/runtime/configuration and executable
digests. A crash without matching sanitizer/site evidence is ERROR/INCONCLUSIVE,
not a verified overflow. Missing sanitizer/runtime is UNSUPPORTED; the release
remains unqualified. Real cases use the compiler-established N/N+1 extent.

Negative controls: length 0, N, N+1 with short source, correct destination guard,
sibling guard, different callsite, unrelated sanitizer crash, reordered source,
wrong snapshot and malformed harness input. Indexed reads/stores are separately
registered predicates and need independent executions; do not continue after UB.

## 3. Semantic contract artifacts

`semantic-contract.v1` is separate from Function Summary IR and M9 facts:
subject/context refs; typed predicate; preconditions/effects/invariants;
source/test/doc evidence; origin DOCUMENTED/HUMAN/LLM_INFERRED; assumptions;
version; dependency digests; status PROPOSED/CONDITIONAL/VALIDATED/REFUTED/STALE;
and optional independent verification ref. A document's stated intent is not
proof that the code implements it. Caching or multiple agreeing models do not
raise authority. A validated contract remains scoped to its proved proposition,
execution model and assumptions.

First semantic observation: after `ReleaseConnection`, an already queued callback
may still operate on the released state. Capture enqueue/cancel/dispatch/release
anchors, ownership policy, expected execution ordering and source of the
contract. Report supported concern or inferred observation when ordering is
unresolved. P4 can register a finite-state/event-schedule checker over explicit
transitions and bounded schedules; no unbounded concurrency proof is implied.
The verifier cannot assume the invariant it is supposed to establish.

## 4. Frozen corpus and ablations

Before model tuning, commit a manifest containing full base/head commits,
program/build selection, artifact digests, expected predicates/known defects,
negative controls, adjudication rubric and licenses. Separate training/tuning
from held-out changes. P0 must locate and pin real C/C++ changes for both first
families; regression fixtures alone do not meet P1 production qualification.
If unavailable, record the gap and keep the gate open. Do not fabricate commit
identities or expected model observations.

Variants: A bounded-source LLM; B A plus structural index/summaries; C B plus
local compiler evidence; D C plus selected SVF/Datalog/SMT; E existing full
native/Evidence pipeline. Use fixed-candidate runs to isolate verification and
end-to-end discovery to measure missed candidates. Hold model, prompts/context
policy, total tokens, allowed time and benchmark constant within comparisons.
Unavailable D/E capabilities are marked unavailable, never replaced by fake data.

Report adjudicated precision/recall, missed critical defects, evidence accuracy,
unknown/abstention rate, reviewer effort, useful hypotheses and deciding outcomes;
cold index/setup, warm tool and end-to-end latency, aggregate parent/child peak
RSS, physical footprint, logical/physical store bytes, model turns/tokens/cost.
Repeat stochastic runs with recorded configuration and variability. Deterministic
reporting applies to identical admitted inputs/responses, not identical future
model outputs. No confidence threshold substitutes for independent checking.

## 5. Targets and decision rules

Targets are proposed acceptance criteria, not measured results: structural index
live bytes <=20% of same-fixture full-analysis live bytes; warm structural-query
P95 <2 s; bounded pre-indexed first-review P95 <30 s including model latency.
Use >=100 queries/attempts per declared workload for P95 and >=3 fresh runs
per deterministic full-build/configuration measurement. Pin compiler/build/
target, report load/swap; separate Debug and Release, cold and warm, and index,
model, provider-bootstrap and query time. No cross-configuration ratio claim.

Demand-scaling gate: adding 10x disconnected code changes fixed closed-query
examined rows/new claim facts by <=10%, excluding separately measured indexing.
Use chains, stars, layered DAGs, dense SCCs, alias-heavy and callback cases at
at least three sizes. Keep #142/#145 thresholds unchanged; demand-query and
whole-store growth are separate measurements. Exhaustive all-pairs workloads
may still be quadratic and must be reported as such.

Zero false promotion is a release requirement: unsupported, open-frontier,
truncated, mismatched or model-authored authority never becomes verified state
in qualification. At least one real analytical obligation must decide in P1;
all-inconclusive output is insufficient. Supported semantic observations remain
useful without pretending to have a concurrency proof.

P4 precision adapters are funded only when held-out comparisons show improved
adjudicated quality/coverage at acceptable incremental cost. A decision record
must report both benefits and misses; unavailable experiments keep that gate
open. Model/provider selection is an operator deployment choice requiring the
user's source-disclosure policy, not a dependency added by this design task.

## RDX07 authority-boundary execution amendment

The closed concrete-input wire models and C++/Python validation are delivered by
RDX07. The budget fields are positive `wall_ms`, `output_bytes`, `examined`.
VerificationResult carries `verification_id`, `binding`, `request_digest`,
`predicate_kind`, `typed_operands`, `execution_domain`, `producer`, `status`,
`proposition=REQUESTED_LE_CAPACITY`, `certificate_artifact_ids`,
`counterexample_artifact_ids`, `input_dependencies`, `assumptions`, `coverage`,
`operational_outcome`. Each dependency is `{artifact_id,content_digest}`.
Operational outcomes are COMPLETED/TIMEOUT/UNSUPPORTED/CANCELLED/ERROR.
The executable schema defines every field as required and rejects extra keys.

Obligation ID hashes the canonical request without its `obligation_id`; request
digest hashes the complete canonical request. Result ID hashes the canonical
result without `verification_id`. Timing/occurrences stay outside these payloads.
Trusted immutable policy pins dependency bytes and independently admitted result
IDs. A registered producer name/version/config alone is insufficient: a deciding
result must have an exact admitted receipt, complete supported coverage and its
own witness. Reusing an input as its own proof, changing input content or any
request premise, or admitting circular/unapproved assumptions is refused.
No production verifier or FORMAL schema is registered; production verify remains
UNSUPPORTED until RDX09's enforcing-runtime and real-replay qualification passes.
