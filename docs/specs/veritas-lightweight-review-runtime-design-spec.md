# Lightweight Review Runtime and Backend v2

**Status:** Detailed runtime contract. RDX05–07 foundations are implemented;
controller, real replay, host and release qualification remain pending.
**Stage:** P1; does not depend on P2/P3 storage changes.
**Plan:** [lightweight runtime](../plans/veritas-lightweight-review-runtime-implementation-plan.md).
**Amends:** the context/discovery portions of the approved plugin specification;
the v1 EIR-only analytical backend remains a separately negotiated capability.

## 1. Deployment and responsibility

Implement the native structural/local provider as a C++20 library under
`include/veritas/review/` and `src/review/`, exposed by `veritas-review-backend`.
Keep the host-neutral Python 3.11+ standard-library controller in
`plugins/veritas-evidence-review/scripts/veritas_review/`, the approved package
layout. No `plugins/` implementation exists at baseline `99d3130`: reuse here
means reuse of the approved design, not reuse of shipped Python modules. The
focused plan creates the package, security/controller and host pieces before
qualifying them. The Claude plugin is
one host adapter (#79), not a second controller or the native backend. No model
vendor is hardcoded, no reviewed-project Python is imported, and no package
installation is required for the controller.

The controller owns policy, aggregate budgets, candidate admission and audit.
The backend owns snapshot capture, compiler-grounded evidence and registered
replay. The model proposes candidates, context requests, hypotheses and next
questions; it never supplies executable commands or deciding verification states.

## 2. Canonical wire types

All v2 envelopes use protocol `veritas.review.backend.v2`, exactly one UTF-8
JSON object, unknown-key/duplicate-key rejection, integers excluding booleans,
no NaN/infinity, and canonical object-key ordering. The existing request/success/
failure envelope structure is retained with exact `request_id` matching.
Semantic JSON uses UTF-8 strings without Unicode normalization, lexicographically
sorted object keys, no insignificant whitespace, no slash/non-ASCII escaping,
and standard JSON escaping of controls/quotes/backslashes. Semantic numbers are
bounded integers only; durations/costs use integer milliseconds/micro-units.
Arrays retain declared order except fields explicitly specified as sets. Native
and Python encoders must produce identical bytes for Unicode, NUL/control,
empty-array and boundary-integer fixtures; audit timing remains outside IDs.

New review IDs are opaque strings `<kind>:sha256:<64 lowercase hex>` with kinds
`review-snapshot`, `program-scope`, `context`, `candidate`, `local-evidence`,
`obligation`, `verification`, `review-report`, `structural-index`. They are not M9 `StableId`s and
must not be parsed or stored as FactIDs/AnalysisRunIDs. Use tagged C++ wrappers
and Python dataclasses to prevent accidental interchange. Hash a domain tag
plus canonical JSON semantic payload; occurrence nonce, wall times, absolute
checkout paths and provider timing are audit metadata, excluded from identity.

```text
ReviewBinding {
  snapshot_id, repository_id, base_revision_id,
  working_tree_digest, program_scope_id, build_context_digest,
  native_binding: null | {analysis_run_id, projection_id, fact_snapshot_id},
  provider_versions: sorted list of {name, version, config_digest}
}
ProgramScope {
  member_tu_digests[], dependency_artifact_digests[], target_digest,
  membership_source: BUILD_LINK_ARTIFACT | EXPLICIT_DECLARATION | OUTPUT_HINT,
  coverage: DECLARED_CLOSED | OPEN | UNRESOLVED, assumptions[], frontier[]
}
```

Every post-capture result repeats the exact binding. Handshake has no binding;
capture returns the newly established binding. `native_binding=null` is valid for the
lightweight provider. No fake native projection/run is minted to satisfy a v1
shape. v1-only clients fail negotiation before snapshot capture when v2 context
or candidates are required; source access already admitted by v1 is preserved.

## 3. Snapshot and membership

`CaptureReviewSnapshot(const SnapshotRequest&) -> StatusOr<ReviewSnapshot>`
captures the base commit, all selected tracked files including dirty content,
explicitly selected untracked files, compile commands, and selected test/doc
artifacts into content-addressed immutable blobs. Normalize repository-relative
paths, reject traversal and unsupported symlinks, and record unreadable/excluded
members. Materialize worker input from these blobs, not the live checkout.
Capture and validate referenced include/configuration artifacts; missing inputs
make compiler capability partial/unsupported. Source reads use the same captured
bytes even if the working tree changes. Final report records whether the live
tree has since advanced; it does not silently refresh the snapshot.

Object output paths are grouping hints. They cannot establish transitive
library membership, generated inputs or linker selection. Prefer an explicit
program manifest or resolved build/link artifact, and include its dependencies
and digest. A declared closed scope is an explicit assumption, not a discovered
whole-program proof. An output-only anonymous group stays OPEN. Local array
extent can still be answered in an open program; universal interprocedural
absence cannot. Existing native ID-chain migration is deferred to its own gate.

## 4. Structural and local provider

Compiler commands are data, not trusted executables. Resolve a trusted compiler
from operator policy; admit semantic flags through an explicit allowlist with
known arities. Expand only bounded captured response files. Refuse plugin-load,
arbitrary executable/wrapper, untrusted module/PCH and unsupported options;
rewrite output/temp paths into the private snapshot. Include paths must resolve
to captured inputs or trusted versioned read-only toolchain artifacts. Do not
silently drop an unknown semantic flag and claim equivalent analysis. Compile
and replay isolation uses the enforcing profile in the qualification spec.

`BuildStructuralIndex(const ReviewSnapshot&, const StructuralBudget&) ->
StatusOr<StructuralIndex>` parses selected TUs with Clang without linking the
project, constructing SVF, or invoking WPA. Persist definitions, type/extent
descriptors, source anchors, syntactic direct calls, actual callsites and
argument bindings. Distinguish compiler-resolved direct calls from unresolved
indirect calls; neither syntactic absence nor a missing header is `NoAlias`.

`InvestigateLocal(const LocalRequest&) -> StatusOr<LocalEvidence>` supports the
initial predicate `copy_bound.v1`: a direct local constant-sized array,
registered `memcpy` form, size operand, and locally admitted guard/control-flow
facts. Extents are byte counts from compiler type/layout; do not confuse array
elements with bytes. Refuse variable-length/ambiguous heap extents, unsupported
macros/ABI forms, integer casts the producer does not model, or mutated aliases.
Expose the unsupported frontier, not a guessed interval.

`LocalEvidence` carries binding, predicate/version, subject and callsite refs,
typed byte extent and operand refs, supporting and contradicting observations,
assumptions, producer/configuration, coverage and provenance anchors. This is
`local-evidence.v1`, not `eir.v1`. Native `EvidenceCaseBuilder` receives only
valid M10B native handoffs; a `NativeEirAdapter` may expose an existing validated
case under its native binding. There is no implicit conversion of local data
into an M9 fact. The review report can cite both artifact kinds.

## 5. Context and candidates

`ContextArtifact` is `{context_id, binding, kind, path, content_digest, start,
end, excerpt, omitted_regions[], selection_reason, entity_bindings[]}`. Supported kinds are DIFF,
SOURCE, TEST, DOC. Offsets count UTF-8 bytes; line numbers are presentation
metadata. A context handle is advertised by the controller/backend, scoped to
one binding. Tests/docs lacking program entity IDs use these artifact refs,
never invented EIR entities. Model text is always untrusted data.

For indexed source/diff context, each advertised entity binding is
`{entity_ref:EntityRef, start:uint64, end:uint64, kind:FUNCTION/CALLSITE/VALUE/MEMORY}`
within that artifact. Other context kinds may have an empty binding list.
`AnchorRef={context_id,start,end,entity_ref:EntityRef-or-null}` is the exact
CandidateProposal anchor type. A source-anchored LLM proposal can use null; the
controller resolves it against advertised bindings for the exact source span.
One matching callsite yields an admitted local target; zero or ambiguous matches
remain unresolved and may request more context. The model never invents an
index ordinal, and source proximity alone never selects an arbitrary callee.

`CandidateProposal` requires `{binding, origin, claim_kind, anchor_refs[],
contract_refs[], question, assumptions[]}`; origin is LLM or DETERMINISTIC.
Admission validates advertised refs, selected artifacts, supported claim/report
kind and positive limits. Canonical identity covers sorted anchors, normalized
registered claim kind and predicate/contract identity; model prose/severity
does not change the deduplication key. Deterministic sorting precedes case
limits. Record rejected/deferred candidates and reasons for coverage evaluation.
Unregistered semantic concerns become report annotations; they are not discarded
just because an analytical EIR claim kind is unavailable.

Release/callback observations cite registration, release state writes,
cancellation/fencing path, execution model and tests/docs. Naming alone is an
inferred convention. A semantic observation may be useful while its invariant
remains unproved. Candidate admission and severity confer no proof authority.

## 6. Operations and state machine

### 6.1 Closed operation schemas

All fields below are required unless explicitly nullable. Every object rejects
additional keys. `binding` is the exact ReviewBinding; post-capture `ref` must be
an advertised snapshot handle. Capture uses trusted invocation selection keys
instead, resolved from the operator's project configuration before any model
turn. `EntityRef={index_id: structural-index ID, ordinal:uint32}` resolves only
inside that captured index; it is not a native entity ID. `Coverage={supported:
bool, scope_closed:bool, traversal:COMPLETE|INCOMPLETE, frontier_refs:[ref],
reasons:[string], examined:uint64}`. No empty successful result erases coverage.

| Operation | parameters | result |
| --- | --- | --- |
| handshake | `{accepted_protocols:[string]}` | `{protocols:[string], backend_version:string, capabilities:[Capability]}` |
| capture | `{project_key:string, base_ref:string, target_ref:string-or-null, selected_untracked:[path], program_manifest_key:string-or-null, context_selection_keys:[string]}` | `{binding, context_refs:[ref], capabilities:[Capability], coverage:Coverage}` |
| context | `{binding, context_refs:[ref], max_bytes:uint32, max_tokens:uint32}` | `{binding, artifacts:[ContextArtifact], usage:Usage, coverage:Coverage}` |
| seed-candidates | `{binding, changed_context_refs:[ref], limit:uint32}` | `{binding, candidates:[CandidateProposal], coverage:Coverage, usage:Usage}` |
| local-evidence | `LocalRequest` below | `LocalEvidence` below |
| native-evidence | `{binding, candidate_id, level:L0/L1/L2, budget:EvidenceQueryBudget}` | `{binding, evidence_id, eir_json, coverage:Coverage, usage:Usage}` |
| verify | `VerificationRequest` from the qualification spec | `{binding, verification:VerificationResult, usage:Usage}` |
| release | `{binding}` | `{binding, released:bool}` |

`project_key` is resolved from trusted invocation configuration, not a model
path. Capture/release and provider selection are controller-only operations.
Normalize/validate Git refs before argv construction; forbid option injection.
`Capability={operation:string, version:string, predicate_kinds:[string],
authority:OBSERVATION/CONCRETE_REPLAY/FORMAL, limits:{wall_ms:uint64,
output_bytes:uint64, examined:uint64}}`; all numeric limits are positive.
`Usage={wall_ms:uint64, cpu_ms:uint64, peak_rss_bytes:uint64,
output_bytes:uint64, examined:uint64}`. Model usage is recorded separately.
`native-evidence` refuses a null native binding and never fabricates its fields.

`LocalRequest={binding, predicate_kind:"copy_bound.v1", callsite_ref:EntityRef,
requested_slots:[OPERANDS/EXTENT/GUARD], max_examined:uint64}`. Requested slots
are a sorted unique nonempty list. `LocalEvidence={local_evidence_id, binding,
predicate_kind, callsite_ref, observations:[LocalObservation], assumptions:[string],
coverage:Coverage, producer:{name,version,config_digest}, usage:Usage}`.
`LocalObservation={slot, subject_ref:EntityRef, value, source_refs:[ref]}` is a
closed discriminated union: OPERANDS value is `{destination,source,length}` of
EntityRefs; EXTENT is `{bytes:uint64}`; GUARD is `{condition_ref:EntityRef,
dominates:bool, relation:"SIZE_LE_EXTENT", operand_refs:[EntityRef]}`. Unsupported
slots remain absent with an explicit coverage reason; no placeholder zero extent.

`copy_bound.v1` asks whether the consumed size is at most destination capacity
for the declared execution domain. It does not equate source-readable extent
with destination extent. Verification operands/polarity are defined in the
qualification spec; local structural observations alone do not decide feasibility.
Initial executable analytical kind is `copy_bound.v1`; the registered report
kind `semantic_observation.v1` may retain an unregistered proposed predicate
as text with context refs, without making it executable or authoritative.

### 6.2 Controller execution

The native executable accepts these subcommands, each reading one request on
stdin: `handshake`, `capture`, `context`, `seed-candidates`, `local-evidence`,
`native-evidence`, `verify`, `release`. No arbitrary SQL, path, program, compiler
flag or shell-command operation is exposed. Handshake advertises versions,
predicate capabilities, producer authority and configured limits. Missing
capabilities return `UNSUPPORTED`, never a practice result.

Controller functions (new modules under the existing package):

```python
start_review(options: ReviewOptions, backend: Backend, policy: ReviewPolicy) -> Turn
admit_candidates(proposals: tuple[CandidateProposal, ...], state: ReviewState) -> Admission
advance_review(response: ModelResponse, state: ReviewState, backend: Backend) -> Turn
render_report(state: ReviewState) -> ReviewReport
```

`ReviewState` follows CREATED → SNAPSHOT_PINNED → DISCOVERING → INVESTIGATING →
REPORT_READY → CLOSED; ABORTED is terminal. `advance_review` may return to
INVESTIGATING for another admitted request, never change the pinned binding.
Advertise exact operation/ref pairs per turn. A stale response, unadvertised
ref, unknown field, capability replay, or exhausted budget is rejected before
backend execution. Persist occurrence/audit state failure-atomically between
turns. Retrying an identical admitted request ID returns its stored response;
it cannot double-spend or execute a replay twice unknowingly.

Model responses have exactly `binding`, `candidate_proposals`, `assessments`,
`requests`, and `finish`. Assessments are `needs_evidence`, `likely_defect`,
`likely_false_positive`, `inconclusive`. Each cites advertised refs and states
remaining assumptions. Requests select advertised operations and refs only.
No model field may choose epistemic/verification authority or a backend executable.

## 7. Budget and process contract

Default whole attempt: 8 model turns, 16 candidates, 32,000 input tokens,
64 KiB model response, 256 KiB aggregate context, 8,000 tokens/32 KiB context
per turn, 120 s aggregate backend wall time, 60 s per precision worker,
1 GiB observed RSS per worker and 2 GiB aggregate controller/worker RSS, at most
one precision worker. Sample descendants every 250 ms, terminate on observed
exhaustion, and report sampling/termination overshoot. Use OS-enforced limits
where supported; do not advertise a portable hard RSS ceiling on POSIX. An
operator requiring a hard isolation limit must select an enforcing runtime or
receive UNSUPPORTED before dispatch.
Limits are positive and operator-configured; model options can only lower them.
Charge requested work before dispatch and reconcile actual usage afterwards.
Model latency is measured separately; interactive latency includes it.

Budget order: validate request → reserve request/output/work allowance → execute
→ charge even on error/timeout → append result/audit. Repeated cheap requests
share aggregate ceilings. Source uses bytes and a fixed configured tokenizer;
if no tokenizer exists, conservatively charge one token per UTF-8 byte. The
host must enforce a 4096-output-token ceiling and provide token usage for model
calls; missing usage prevents further
model turns rather than granting free calls. Output readers are bounded while
the process is running. Use argv arrays, scrubbed environment, private snapshot
directory, process groups and deadlines; preserve the existing plugin capability
rules. Authority validation and replay details are in the qualification spec.

## 8. Reporting and acceptance

`review-report.v1` contains binding, coverage (visited/deferred/excluded sets),
candidates and annotations, supporting/contradicting refs, unknowns/assumptions,
verification refs, costs and practice/production mode. Categories are VERIFIED
FINDING, SUPPORTED CONCERN, SEMANTIC OBSERVATION, INCONCLUSIVE. These are report
categories, not EIR enum additions. An authoritative concrete replay establishes
that execution only. Empty/deferred discovery never establishes safety.

P1 release requires two adjudicated real-change families, supported by small
regression fixtures: removed local-array bound with a decisive overflow replay
and base rejection; release/callback cancellation concern with a correct
control and explicit contract assumptions. The 32/33-byte fixture is mandatory;
a real change uses its actual N/N+1 boundary. A synthetic-only result or a fake
backend transcript is practice, not production qualification.

## RDX07 execution amendment

RDX07 implements runtime Task 4 and verification Task 1. The executable uses
bounded newline-delimited, individually strict v2 envelopes in one resident
subprocess, preserving immutable in-memory handles across requests without
recapturing live content. A named subcommand accepts its operation or an initial
handshake; every post-capture operation still requires its exact pinned binding.
There is one active snapshot; release/process exit removes its handles. This
framing choice avoids a disk serialization or native-pointer restoration format.

Trusted configuration resolves project/context/program keys before model access.
`target_ref=null` selects the working tree; non-null revision-target capture is
explicitly unsupported. DIFF context is an exact deterministic full-file
replacement. Contexts exceeding the 32-KiB configured window retain an incomplete
gap; no omitted copy site is advertised for local investigation. Structural work
exhaustion refuses public capture before publication, while parse gaps remain
partial. The public limit is propagated into the structural provider.

Only capture/context/seed/local/release are advertised. Native evidence and
verification have no qualified production implementation and return UNSUPPORTED.
The closed verification codecs are present for downstream RDX09; arbitrary
FORMAL domains remain unavailable. Authority requires a separately admitted
exact-result receipt in trusted policy, beyond producer/version registration.
This does not deliver RDX08/RDX09/RDX11 or weaken their real qualification gates.
