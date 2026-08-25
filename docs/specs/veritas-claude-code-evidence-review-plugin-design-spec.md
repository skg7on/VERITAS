# VERITAS Claude Code Evidence Review Plugin Design Spec

**Status:** Approved

**Scope:** On-demand Evidence IR code review in Claude Code

**Depends on:** M9 provenance and Explain APIs, M10B Evidence Builder input
APIs, M10C `eir.v1` semantic serialization, and a public VERITAS review
backend exposing the operations in this specification

**Complements:** Evidence IR architecture, the M10B–M10C API-to-Evidence-IR
contract, and the Evidence IR Agent security use cases

---

# 1. Purpose

This specification defines a distributable Claude Code plugin that turns a
Git semantic delta into a bounded set of Evidence IR review cases. The plugin
lets Claude reason over compact, provenance-backed evidence without reading
SummaryDB directly or treating model output as an authoritative program fact.

The governing workflow is:

```text
on-demand Git comparison
        -> deterministic impacted-candidate discovery
        -> validated EIR-L0 case
        -> bounded semantic expansion
        -> INFERRED model hypotheses and proof obligations
        -> deterministic review report
```

The initial end-to-end acceptance case is buffer overflow. The orchestration
protocol and renderer are claim-agnostic so later VERITAS claim kinds can use
the same plugin without changing its trust model.

# 2. Goals and non-goals

## 2.1 Goals

The V1 plugin shall:

1. provide an explicit, user-invoked Claude Code review skill;
2. discover Evidence cases affected by a base-to-working-snapshot semantic
   delta, with an explicit case-ID override;
3. consume only public VERITAS semantic operations and validated `eir.v1`;
4. disclose EIR-L0 first and add L1/L2 evidence only through budgeted deltas;
5. validate every model reference, request, hypothesis, and assessment before
   executing an operation or rendering a result;
6. preserve ProgramContext, epistemic state, provenance, contradiction,
   unknowns, omissions, and truncation;
7. keep model conclusions non-authoritative;
8. produce deterministic per-case and aggregate review artifacts; and
9. run with Python 3's standard library and the VERITAS CLI/backend, without a
   package-install step or long-running service.

## 2.2 Non-goals

V1 shall not:

* run automatically after file edits or commits;
* query RocksDB, SQLite, Souffle outputs, or other SummaryDB layers directly;
* ask Claude to search an entire repository for vulnerabilities;
* implement new static analyses or reconstruct missing provenance;
* authorize Claude to create `MUST`, `MUST_NOT`, `PROVED`, `REFUTED`,
  `VERIFIED_SAFE`, or `VERIFIED_DEFECT` results;
* silently infer safety from empty, incomplete, or truncated evidence;
* edit reviewed code or post review comments to a remote service;
* require an MCP server, daemon, Python binding, or in-process C++ FFI; or
* claim support for a proof backend that VERITAS has not configured.

# 3. Readiness and entry criteria

The checked-in project documentation currently treats the Review Agent path
as later than M10C. Plugin implementation may be developed with a fake backend,
but real-backend acceptance requires all of these entry criteria:

1. M9 can return stable facts, run bindings, rooted provenance, and explicit
   Explain truncation.
2. M10B can pin one immutable read snapshot and produce typed evidence inputs
   with completion facts.
3. M10C can validate and serialize one `EvidenceCase` as deterministic
   `eir-json` using schema `eir.v1`.
4. A public local backend can negotiate versions, discover impacted
   candidates, load EIR levels, and execute the semantic operations defined in
   section 8.
5. The backend can identify a dirty working-tree analysis snapshot without
   mixing it with the base revision or another analysis run.

If an entry criterion is missing, the plugin shall fail readiness checks or run
only against the explicitly selected fake backend. A fake-backend run is a
demonstration, not a real code review.

# 4. Product decisions

V1 fixes the following product choices:

| Decision | V1 choice |
| --- | --- |
| Invocation | Explicit `/veritas-evidence-review:evidence-review` skill |
| Trigger | On demand; no automatic hooks |
| Default scope | Semantic delta from merge-base to the current working snapshot |
| Candidate selection | Diff-driven discovery with optional `--case <StableId>` |
| Claim coverage | Claim-agnostic protocol; buffer overflow is the first complete fixture |
| Integration | CLI/subprocess adapter behind one Python backend interface |
| Model authority | Observations, `INFERRED` hypotheses, and pending proof obligations only |
| Persistence | Append-only audit state under `${CLAUDE_PLUGIN_DATA}` |
| Repository mutation | None by default |
| Dependencies | Python 3 standard library plus public Git and VERITAS executables |

# 5. Plugin package

The plugin lives at:

```text
plugins/veritas-evidence-review/
├── .claude-plugin/
│   └── plugin.json
├── skills/
│   └── evidence-review/
│       ├── SKILL.md
│       └── references/
│           ├── response-contract.md
│           └── review-policy.md
├── bin/
│   └── veritas-evidence-review
├── scripts/
│   └── veritas_review/
│       ├── __init__.py
│       ├── backend.py
│       ├── cli.py
│       ├── controller.py
│       ├── models.py
│       ├── validation.py
│       └── reporting.py
└── tests/
    ├── fixtures/
    └── test_*.py
```

Claude Code discovers the plugin through `.claude-plugin/plugin.json`. The
manifest name is `veritas-evidence-review`, so the public skill is namespaced
as `/veritas-evidence-review:evidence-review`.

The skill uses `disable-model-invocation: true`. A code-review run can be
expensive and may expose selected source fragments, so invocation must remain
an explicit user decision.

The executable in `bin/` is the only command the skill invokes. It loads the
Python package relative to `${CLAUDE_PLUGIN_ROOT}` and never imports code from
the reviewed repository.

`cli.py` parses the public command line, maps typed failures to stable exit
codes, and keeps structured stdout separate from human-readable stderr.

# 6. Component responsibilities

## 6.1 Skill

`SKILL.md` defines:

* when the explicit command is appropriate;
* the L0-first bounded-refinement workflow;
* the response contract;
* the distinction between authoritative evidence and model hypotheses;
* the rule that every operation passes through the controller; and
* the terminal rendering behavior.

Substantial response and policy detail lives in the two linked references so
the main skill remains concise.

## 6.2 Backend adapter

`backend.py` is the only component aware of concrete VERITAS command spelling.
It translates subprocess results into typed plugin models and provides one
interface that a future MCP transport can reuse without changing the skill or
controller.

It executes processes with argument vectors, never interpolated shell command
strings. It enforces time, output-size, encoding, and JSON-shape limits before
returning data to the controller.

## 6.3 Controller

`controller.py` owns run state, deterministic candidate order, the L0-to-L1/L2
loop, budgets, response admission, snapshot stability, audit events, and stop
conditions. It does not interpret defect semantics beyond generic protocol
rules.

## 6.4 Validation

`validation.py` rejects malformed or unsafe backend and model data. Validation
is a precondition for persistence, evidence expansion, proof dispatch, and
report generation.

## 6.5 Reporting

`reporting.py` derives Markdown and JSON from validated case state. Reports do
not copy a model's verdict. They combine authoritative EIR state with admitted,
clearly labeled model hypotheses and unresolved blockers.

# 7. User interface

The primary invocation is:

```text
/veritas-evidence-review:evidence-review [base-ref]
```

The skill maps user arguments to the executable:

```bash
veritas-evidence-review start \
    --project "${CLAUDE_PROJECT_DIR}" \
    --base <base-ref>
```

Supported V1 options are:

```text
--base <git-ref>          explicit comparison base
--head <git-ref>          review a committed target instead of the working snapshot
--case <finding-id>       bypass discovery and review one case
--max-cases <positive>    cap candidates after deterministic ordering
--output <path>           copy the final Markdown report to a user-selected path
--json-output <path>      copy the aggregate machine result
--backend <path>          select the public VERITAS backend executable
--force                   replace an existing explicitly selected output file
```

If `--base` is absent, the controller resolves the merge-base between the
target and the symbolic `origin/HEAD`. If that reference is unavailable, it
requires an explicit base instead of guessing. `--head` defaults to the current
working snapshot, including tracked staged and unstaged changes. Untracked
files are excluded unless the VERITAS project manifest already admits them.

Without `--output`, the final Markdown is printed to stdout and audit artifacts
remain only in plugin data. A selected output path must not already exist unless
the user also supplies `--force`.

# 8. Backend contract

The Python interface is:

```text
handshake() -> BackendCapabilities
pin_snapshot(ProjectSpec) -> SnapshotBinding
discover_candidates(DiscoveryRequest) -> CandidateSet
get_case(GetCaseRequest) -> EvidenceEnvelope
execute(EvidenceOperationRequest) -> EvidenceDelta
request_proof(ProofRequest) -> ProofResult
```

The subprocess adapter maps these calls to the public VERITAS backend. Every
backend response uses canonical JSON on stdout and diagnostics on stderr.

## 8.1 Handshake

The handshake returns:

```text
backend_protocol_versions[]
eir_schema_versions[]
operations[]
claim_kinds[]
proof_backends[]
backend_version
analyzer_versions[]
```

V1 requires `veritas.review.backend.v1` and `eir.v1`. Unsupported major
versions stop before analysis.

## 8.2 Snapshot binding

The immutable binding contains:

```text
repository_id
base_revision_id
target_revision_or_snapshot_id
build_variant_id
analysis_configuration_id
analysis_run_id
native_projection_id
fact_snapshot_id
working_tree_fingerprint
```

Every later response must reproduce the binding. A changed member is a
snapshot mismatch, not a new delta inside the current run.

## 8.3 Candidate discovery

`CandidateSet` contains ordered candidate records plus completeness metadata:

```text
finding_id
claim_kind
severity
subject_ref
source_ref
sink_ref
semantic_delta_refs[]
query_completion_fact_id
```

Discovery can over-approximate. Each candidate begins in `POSSIBLE_DEFECT` or
another non-verified state. Empty truncated discovery is not "no findings."

## 8.4 Evidence operations

V1 recognizes only these semantic operations:

```text
get_primary_evidence
expand_summary
expand_path
explain_fact
get_unknowns
get_assumptions
get_conflicts
request_source
request_proof
```

An `EvidenceEnvelope` advertises the operations allowed for that turn and the
exact target IDs each operation may accept. The controller does not infer an
operation from prose.

`request_source` returns the smallest backend-selected expression, statement,
or function fragment for a supplied entity/source-anchor ID. It is revision
bound, separately budgeted, and marked untrusted.

`request_proof` may return authoritative proof state only when the result names
a configured authority-bearing producer. `TIMEOUT`, `UNSUPPORTED`, and backend
errors remain non-deciding results.

# 9. Review protocol

## 9.1 Start and discovery

The controller:

1. validates command arguments and project containment;
2. runs the backend handshake;
3. resolves the base and target;
4. pins one immutable backend snapshot;
5. discovers impacted candidates or validates `--case`;
6. sorts candidates by stable finding identity; and
7. creates an append-only run directory.

## 9.2 Initial turn

For each candidate the controller loads and validates EIR-L0. Claude receives:

```text
run and ProgramContext identity
case ID and EvidenceID
primary claim and verification state
primary path
supporting and contradicting evidence
strongest unknowns and omissions
completeness and truncation
allowed operation/target pairs
remaining budgets
```

The initial prompt does not contain an entire EIR-L2 graph, complete
provenance closure, whole source file, or unrestricted repository tool.

## 9.3 Model response

Claude returns one object matching:

```text
ReviewResponse {
    assessment
    observations[]
    hypotheses[]
    evidence_requests[]
    proof_obligations[]
    source_requests[]
}
```

Assessment is one of:

```text
needs_evidence
likely_defect
likely_false_positive
inconclusive
```

The skill submits this JSON to the controller over stdin. It uses a quoted
here-document containing contract data only; backend source payloads are never
interpolated into the shell command. The controller limits total input size and
requires response strings to be valid UTF-8 without control characters.

## 9.4 Admission and delta

Before executing a request, the controller:

1. validates the response shape and assessment;
2. resolves every cited ID inside the current case;
3. parses and type-checks supported predicate forms;
4. stores accepted model propositions as `INFERRED` hypotheses;
5. matches each operation to an advertised operation/target pair;
6. charges the request to per-turn and per-case budgets; and
7. preserves the immutable snapshot binding.

Accepted operations return an EIR-L1/L2 delta. The next turn includes additions,
denials, truncation, unchanged stable references, and remaining budgets. It
does not restate the repository or duplicate unchanged evidence.

## 9.5 Stop conditions

A case stops when:

* existing or newly returned authoritative evidence decides the claim;
* no allowed operation can resolve the strongest remaining uncertainty;
* Claude returns a supported likely/inconclusive assessment with no material
  request;
* the turn or evidence budget is exhausted; or
* a non-recoverable validation or backend error quarantines the case.

The controller never continues merely to consume remaining budget.

# 10. Epistemic and model-authority rules

The response validator enforces:

1. Observations cite supplied fact, path, summary, unknown, omission, proof, or
   source-anchor references.
2. A model proposition is always `INFERRED`, even when its text says "must,"
   "proved," or "verified."
3. Attempts to create `MUST`, `MUST_NOT`, `PROVED`, `REFUTED`,
   `VERIFIED_SAFE`, or `VERIFIED_DEFECT` are rejected rather than normalized
   silently.
4. Model confidence is distinct from VERITAS epistemic state.
5. Unknowns and truncated queries cannot become negative facts.
6. Guarded and path-sensitive evidence remains guarded and path-sensitive.
7. Contradicting evidence remains visible in every relevant turn and report.
8. Multiple model hypotheses may coexist; agreement does not promote them.
9. Proof obligations begin `PENDING` and name requested authoritative
   backends.
10. A final verified state must already exist in validated EIR or arrive in an
    authority-bearing proof result.

# 11. Trust and security boundary

The controller, not the skill prose, is the enforcement boundary.

## 11.1 Process execution

The plugin:

* executes only the selected Git and VERITAS binaries;
* uses argument arrays rather than shell-interpolated commands;
* resolves the project root and output paths before use;
* refuses output paths outside the project unless the user supplied an
  absolute path explicitly;
* refuses to replace an existing selected output unless the user supplied
  `--force`;
* applies process timeouts and terminates the process group on timeout;
* caps stdout, stderr, response, EIR, source, and report byte counts; and
* rejects non-UTF-8 JSON and duplicate JSON keys.

## 11.2 Prompt injection resistance

EIR fields and source excerpts are artifact data, not instructions. The skill
does not follow comments, strings, macro text, generated-file text, or model
prose found in evidence. Source requests are entity-bound, minimal, and charged
against an independent line budget.

The controller never exposes repository credentials, environment dumps,
unrestricted paths, or raw SummaryDB records.

## 11.3 Context isolation

Facts from different repositories, revisions, build variants, analyzer
configurations, analysis runs, projections, provider bindings, or read
snapshots cannot coexist in one run. Stable IDs do not override a context
mismatch.

# 12. Budgets

V1 defines positive limits for:

```text
candidate cases
turns per case
EIR semantic bytes per turn and case
summary expansions
path expansions
provenance depth and nodes
alternate paths
source lines and bytes
proof requests, time, and memory
backend process time and output bytes
```

Defaults are encoded in one immutable policy object and included in the audit
record. Command-line options may lower safe defaults. Raising proof, source, or
process limits requires an explicit option; a model request cannot raise them.

Every turn shows remaining semantic-operation budgets. Reaching a limit
produces a typed truncation or denial event and preserves the relevant frontier
or expandable reference.

# 13. Audit state

Run state lives under:

```text
${CLAUDE_PLUGIN_DATA}/veritas-evidence-review/runs/<run-id>/
├── run.json
├── events.jsonl
├── cases/<case-id>/state.json
├── cases/<case-id>/turns/<turn>.json
├── cases/<case-id>/report.json
└── report.md
```

`run-id` is derived from the backend protocol version, snapshot binding,
comparison, policy digest, and a local occurrence nonce. Semantic Evidence IDs
remain VERITAS-owned; the plugin does not mint replacements.

Events are append-only. Mutable checkpoints use a temporary sibling plus
atomic rename. Completion markers are written only after all referenced files
are durable. An interrupted run can be inspected but never masquerades as
complete.

The audit records model identifier, model occurrence, prompt-contract version,
backend and analyzer versions, requests, denials, budgets, and report identity.
Model metadata is reasoning provenance, not program-fact provenance.

The local nonce and incidental timestamps remain in the audit occurrence only.
They are excluded from the canonical semantic report projection, so identical
validated evidence and admitted responses render byte-identical Markdown and
aggregate JSON.

# 14. Failure and recovery

The controller applies these rules:

| Failure | Behavior |
| --- | --- |
| Missing executable or incompatible protocol | Stop before discovery |
| Analysis/discovery failure | Mark aggregate review incomplete |
| Empty truncated discovery | Report incomplete, never no findings |
| Invalid EIR or unresolved provenance/reference | Quarantine case before model review |
| Snapshot mismatch | Discard mixed attempt and restart the whole run once |
| Second snapshot change | Stop as unstable |
| Invalid model response | Execute nothing; allow one corrected response |
| Second invalid response | End case inconclusive |
| Denied/unsupported request | Record denial and continue if useful work remains |
| Budget exhausted | Preserve truncation and end likely/inconclusive unless already decided |
| Proof timeout/unsupported/error | Preserve non-deciding proof result |
| Report write failure | Leave no completion marker or partial target file |

Candidate-level failures do not erase successful cases. Any quarantined or
unfinished candidate makes the aggregate result partial.

Stable process exit codes are:

```text
0   complete review, no actionable finding
10  complete review, one or more actionable findings
20  partial or inconclusive review
64  usage or configuration error
65  invalid evidence or protocol data
69  backend unavailable or incompatible
70  internal controller failure
```

An actionable finding is a `LIKELY_DEFECT` or `VERIFIED_DEFECT` case. If a run
contains both an actionable finding and any partial/inconclusive disposition,
exit `20` takes precedence over exit `10`; structured output still reports the
finding. Usage, evidence, backend, and internal failures take precedence over
all completed-review categories.

Human-readable diagnostics go to stderr. Structured results and requested
artifacts do not share stderr.

# 15. Review result contract

Each case result contains:

```text
finding identity and ProgramContext
claim and authoritative verification state
short semantic explanation
primary source and sink anchors
supporting facts with epistemic state
contradicting facts
primary path and feasibility
assumptions, unknowns, omissions, and truncation
proof obligations and authoritative results
LLM observations and hypotheses, clearly labeled
recommended remediation or next evidence action
```

The aggregate result contains comparison identity, snapshot binding, backend
and analyzer versions, candidate/disposition counts, incomplete-review reasons,
and links to per-case records.

For a verified defect, the report leads with the authoritative proof or
counterexample. For a likely defect, it leads with deterministic evidence and
the unresolved blocker. For an inconclusive case, it names the exact missing
fact, unsupported operation, or truncation reason.

# 16. Testing strategy

## 16.1 Test-first controller development

Every controller behavior begins with a failing `unittest` case. Unit tests
cover:

* strict JSON decoding and duplicate-key rejection;
* stable-ID and ProgramContext validation;
* epistemic admission;
* operation/target authorization;
* budget accounting;
* snapshot restart limits;
* deterministic candidate and member ordering;
* atomic persistence;
* report derivation; and
* exit-code classification.

## 16.2 Adversarial response corpus

Fixtures attempt to submit model-authored verified states, invented IDs,
cross-case references, mixed revisions, unsafe operations, hidden truncation,
malformed predicates, control characters, oversized fields, and source-borne
prompt instructions. The controller must reject or conservatively represent
each case without invoking the requested backend operation.

## 16.3 Fake-backend integration

A deterministic fake executable covers:

* unsafe overflow;
* safe counterevidence;
* likely and inconclusive outcomes;
* truncated discovery and evidence;
* backend timeout;
* one and two snapshot changes;
* multiple candidates with one quarantined case;
* proof timeout and unsupported theory; and
* byte-identical reruns and golden reports.

## 16.4 Real-backend contract

When the entry criteria in section 3 are present, contract tests exercise
handshake, snapshot pinning, impacted discovery, EIR-L0, EIR deltas, fact
explanation, source budgets, proof dispatch, and version rejection against the
real VERITAS backend.

## 16.5 Claude Code plugin and skill checks

Acceptance runs:

* `claude plugin validate` on the plugin root;
* skill discovery under the expected namespace;
* executable permission and relocation tests; and
* a synthetic-repository command smoke test.

The skill receives a behavioral RED/GREEN test with a fake backend: record the
baseline behavior without the skill, then rerun the same pressure scenario
with the plugin and confirm that all evidence operations still pass through the
controller. This external test requires an authenticated Claude Code CLI. If
it is unavailable, the result is reported as unrun; unit, integration, and
manifest tests do not substitute for it.

# 17. Acceptance criteria

The plugin is complete when:

1. a local Claude Code session can explicitly invoke the namespaced skill;
2. a base-to-working-snapshot comparison discovers deterministically ordered
   impacted cases;
3. `--case` reviews exactly one supplied valid finding;
4. the unsafe overflow fixture produces an actionable report citing stable
   evidence;
5. the safe fixture preserves counterevidence and never relies on truncated
   absence;
6. truncated discovery or case evidence produces a partial/inconclusive result;
7. every model-authored proposition remains `INFERRED`;
8. out-of-case references and unauthorized operations execute nothing;
9. mixed ProgramContext or a repeated snapshot change stops safely;
10. proof timeout or unsupported theory cannot create a verified state;
11. reports and audit artifacts are deterministic apart from declared
    occurrence metadata;
12. interruption cannot leave an apparently complete artifact;
13. all Python, fake-backend, real-backend-when-available, and plugin validation
    checks pass; and
14. the primary repository checkout remains clean on `main`.

# 18. Future extensions

The stable backend interface permits later additions without changing V1's
trust rules:

* an MCP transport exposing the same semantic operations;
* GitHub or other review-comment publication after explicit authorization;
* additional claim-specific proof policies;
* evidence-diff presentation between two completed Evidence Cases;
* organization-managed budget and trust policies; and
* remote authoritative proof services.

Automatic edit hooks remain out of scope until cost, snapshot churn, and source
disclosure behavior have separate design and approval.

# 19. Design invariant

For every plugin review:

```text
deterministic VERITAS analysis
        produces
small, immutable, provenance-backed Evidence IR
        consumed through
an allowlisted and budgeted controller
        where Claude may add
INFERRED hypotheses and pending proof obligations
        while only authoritative backends may create
verified program facts or final verified states
```

The plugin improves review usefulness without weakening the Evidence IR
epistemic boundary.
