# VERITAS Claude Code Evidence Review Plugin Design Spec

**Status:** Approved

**Scope:** Explicitly invoked, capability-isolated Evidence IR code review in
Claude Code

**Depends on:** M9 provenance and Explain APIs, M10B Evidence Builder input
APIs, M10C `eir.v1` semantic serialization, M10A recursive security relations,
and a public VERITAS review backend implementing
`veritas.review.backend.v1`

**Complements:** Evidence IR architecture, the M10B-M10C API-to-Evidence-IR
contract, and the Evidence IR Agent security use cases

**Reviewed against:** [issue #79](https://github.com/skg7on/VERITAS/issues/79)
and the
[Evidence IR Agent security use-case spec][agent-use-cases]

[agent-use-cases]: veritas-evidence-ir-agent-security-use-cases-design-spec.md

---

# 1. Purpose

This specification defines a distributable Claude Code plugin that turns a Git
comparison or stable finding ID into a bounded set of Evidence IR review cases.
The plugin lets Claude reason over compact, provenance-backed evidence without
reading SummaryDB or the reviewed repository directly and without treating
model output as an authoritative program fact.

The governing workflow is:

```text
explicit user invocation
        -> capability-isolated review agent
        -> deterministic Git comparison and candidate discovery
        -> validated EIR-L0 case
        -> bounded semantic expansion through two allowlisted tools
        -> INFERRED model hypotheses and pending proof obligations
        -> deterministic case and aggregate reports
```

The initial end-to-end acceptance case is buffer overflow. The orchestration
protocol and renderer are claim-agnostic so later VERITAS claim kinds can use
the same plugin without weakening its trust model.

# 2. Goals and non-goals

## 2.1 Goals

The V1 plugin shall:

1. provide an explicit, user-invoked Claude Code review skill;
2. run the reasoning loop in a forked plugin agent whose available tools are
   exactly the plugin's model-facing `start` and `advance` operations;
3. require a short-lived capability minted only by explicit skill expansion
   before either model-facing operation can execute;
4. discover Evidence cases affected by a base-to-working-snapshot semantic
   delta, with an explicit stable finding-ID override;
5. consume only public VERITAS semantic operations and validated `eir.v1`;
6. disclose EIR-L0 first and add L1/L2 evidence only through budgeted deltas;
7. validate every model reference, request, hypothesis, and assessment before
   executing an evidence or proof operation;
8. preserve ProgramContext, epistemic state, provenance, contradiction,
   unknowns, omissions, guards, and truncation;
9. keep model conclusions non-authoritative;
10. produce deterministic per-case and aggregate review artifacts; and
11. run on POSIX hosts with Python 3.11+ standard-library code plus public Git
    and VERITAS executables, without a package-install step.

## 2.2 Non-goals

V1 shall not:

* run automatically after file edits, commits, or ordinary prompts;
* expose Bash, file, search, web, Skill, Agent, or unrelated MCP tools to the
  Evidence review agent;
* query RocksDB, SQLite, Souffle outputs, or other SummaryDB layers directly;
* ask Claude to search an entire repository for vulnerabilities;
* implement new static analyses or reconstruct missing provenance;
* authorize Claude to create `MUST`, `MUST_NOT`, `PROVED`, `REFUTED`,
  `VERIFIED_SAFE`, or `VERIFIED_DEFECT` results;
* silently infer safety from empty, incomplete, or truncated evidence;
* let the model select an arbitrary backend executable or overwrite an
  arbitrary output file;
* edit reviewed code or post review comments to a remote service;
* use MCP between the controller and the public VERITAS backend;
* run an independently managed daemon or open a listening socket; or
* claim support for a proof backend that VERITAS has not configured.

The plugin includes a lifecycle-managed local stdio MCP process solely as the
model-facing capability transport. It is not the VERITAS backend transport and
contains no Evidence semantics independent of the shared controller.

# 3. Readiness and entry criteria

The repository currently treats the Review Agent path as later than M10C.
Plugin implementation may be developed and fully exercised as a practice
system with a deterministic fake backend, but production qualification
requires all of these entry criteria:

1. M9 can return stable facts, run bindings, rooted provenance, and explicit
   Explain truncation.
2. M10A provides the registered recursive security relations needed by the
   initial buffer-overflow claim.
3. M10B can pin one immutable read snapshot and produce typed evidence inputs
   with completion facts.
4. M10C can validate and serialize one `EvidenceCase` as deterministic
   `eir-json` using schema `eir.v1`.
5. A public local backend implements the normative
   `veritas.review.backend.v1` wire contract in section 10.
6. The backend can distinguish a dirty working-tree analysis snapshot from the
   base revision and from every other analysis run.
7. The installed Claude Code version supports foreground forked skills,
   plugin-agent tool allowlists, plugin stdio MCP servers, and skill path
   substitution. V1 requires at least 2.1.218, and release qualification must
   execute against that floor as well as the current supported version.

If a VERITAS entry criterion is missing, production review fails readiness or
runs only against an explicitly selected bundled practice scenario. If a
Claude Code capability or managed policy is missing, invocation fails closed.
Neither condition may fall back to ambient Bash, repository tools, or fake
production evidence.

## 3.1 Claude Code host contract

The plugin relies only on documented Claude Code behavior:

* skill `!` command expansion runs before the rendered skill prompt reaches the
  forked agent;
* `${CLAUDE_PROJECT_DIR}` and `${CLAUDE_SESSION_ID}` are substituted for local
  plugin skills;
* `disable-model-invocation: true` removes the skill from model invocation;
* `context: fork` with `background: false` waits for an isolated agent result;
* an agent `tools` field is an availability allowlist, while a skill
  `allowed-tools` field is only a temporary permission grant and is not a
  restriction boundary; and
* bundled plugin MCP tools use the scoped name
  `mcp__plugin_<plugin-name>_<server-name>__<tool-name>`.

The governing references are the official Claude Code
[skills](https://code.claude.com/docs/en/slash-commands),
[subagents](https://code.claude.com/docs/en/sub-agents),
[plugin](https://code.claude.com/docs/en/plugins-reference), and
[MCP](https://code.claude.com/docs/en/mcp) documentation. Release tests assert
the host behavior rather than treating documentation alone as proof. A host
that exposes different tool names, retains unlisted tools, disables skill shell
execution, or blocks the bundled server fails closed.

# 4. Product decisions

V1 fixes the following choices:

| Decision | V1 choice |
| --- | --- |
| Invocation | Explicit `/veritas-evidence-review:evidence-review` skill |
| Trigger | User only; no automatic hooks or model invocation |
| Reasoning context | Foreground fork using the internal Evidence review agent |
| Agent capabilities | Exactly the plugin-scoped `start` and `advance` MCP tools |
| Invocation authorization | Five-minute, single-use, project/session-bound capability |
| Default scope | Merge-base to current tracked working snapshot |
| Candidate selection | Diff discovery with optional `--case <StableId>` |
| Claim coverage | Claim-agnostic protocol; buffer overflow is the first complete fixture |
| Model transport | Minimal local stdio MCP bridge |
| Backend transport | Bounded CLI/subprocess adapter |
| Model authority | Observations, `INFERRED` hypotheses, and pending obligations only |
| Persistence | Append-only audit occurrence under `${CLAUDE_PLUGIN_DATA}` |
| Repository mutation | None during model review |
| Dependencies | Python 3.11+ standard library plus public Git and VERITAS executables |

# 5. Plugin package

The plugin lives at:

```text
plugins/veritas-evidence-review/
├── .claude-plugin/
│   └── plugin.json
├── .mcp.json
├── agents/
│   └── evidence-review-runner.md
├── skills/
│   └── evidence-review/
│       ├── SKILL.md
│       └── references/
│           ├── response-contract.md
│           └── review-policy.md
├── bin/
│   └── veritas-evidence-review
├── schemas/
│   ├── veritas-review-tool-v1.schema.json
│   ├── veritas-review-controller-v1.schema.json
│   ├── veritas-review-backend-v1.schema.json
│   ├── veritas-review-response-v1.schema.json
│   └── examples/
├── scripts/
│   └── veritas_review/
│       ├── __init__.py
│       ├── backend.py
│       ├── capabilities.py
│       ├── cli.py
│       ├── controller.py
│       ├── mcp_server.py
│       ├── models.py
│       ├── reporting.py
│       └── validation.py
└── tests/
    ├── fixtures/
    ├── skill-evals/
    └── test_*.py
```

Claude Code discovers the plugin through `.claude-plugin/plugin.json`. The
manifest name is `veritas-evidence-review`, so the public skill is namespaced
as `/veritas-evidence-review:evidence-review`.

`.mcp.json` starts one local stdio server named `veritas_review`. With plugin
name `veritas-evidence-review`, the only accepted runner tool names are:

```text
mcp__plugin_veritas-evidence-review_veritas_review__start
mcp__plugin_veritas-evidence-review_veritas_review__advance
```

Release tests assert both exact names. If a supported Claude Code release
exposes a different name, the runner fails to launch and the plugin must update
its manifest and agent definition together before claiming support.

The executable in `bin/` is the operator and test CLI. It loads the Python
package relative to its own resolved plugin root and never imports code from
the reviewed repository.

Every new Python file and executable source begins with the repository's
Apache-2.0 SPDX header. License verification is part of the plugin test gate.

# 6. Capability-isolated architecture

The model-facing boundary is:

```text
explicit /veritas-evidence-review:evidence-review invocation
        |
        v
skill expansion executes bundled authorize command once
        |
        v
short-lived invocation capability
        |
        v
foreground fork: evidence-review-runner
tools = only mcp__plugin_veritas-evidence-review_veritas_review__start
             mcp__plugin_veritas-evidence-review_veritas_review__advance
        |
        v
thin stdio MCP transport
        |
        v
shared Python controller and validation boundary
        |
        v
bounded subprocess adapter -> public VERITAS backend
```

The skill has `disable-model-invocation: true`, `context: fork`, and
`background: false`. Its configured agent is the plugin's internal
`evidence-review-runner`.

The runner's `tools` field is an allowlist containing only the two exact MCP
tool names. It omits Bash, PowerShell, Read, Grep, Glob, Edit, Write, WebFetch,
WebSearch, Skill, Agent, ToolSearch, and every unrelated MCP tool. The model
therefore cannot bypass the controller by reading repository source, invoking
Git or VERITAS directly, loading another skill, or delegating to another agent.

The runner sets a fixed `maxTurns` consistent with the controller's hard turn
budget and omits `skills`, `memory`, `isolation`, `hooks`, `mcpServers`, and
`permissionMode`. Its declared model selector and prompt-contract version are
part of the audit policy; neither changes Evidence authority.

The skill's injected authorization command executes before the fork receives
its prompt. It mints a cryptographically random capability and records only its
hash. If skill shell execution is disabled, permission is denied, the command
fails, or its output is malformed, the whole invocation aborts before the
runner starts.

The plugin MCP tools remain visible to the parent Claude Code session while the
plugin is enabled, as required by Claude Code's plugin MCP lifecycle. They are
not sufficient authority: `start` rejects any call without a live invocation
capability. Normal MCP permission handling is an additional user control, not
the trust boundary.

# 7. Component responsibilities

## 7.1 Manual skill

`SKILL.md` defines the explicit invocation, passes user arguments as untrusted
review-selection data, obtains the injected capability, runs the foreground
fork, and instructs the runner to return only the controller's terminal report
or typed failure.

Its `allowed-tools` grants only the exact bundled `authorize` command and the
two plugin-scoped MCP calls for the invoking turn. The Bash grant exists only
so Claude Code can perform the pre-render `!` expansion; Bash remains absent
from the runner's agent `tools` allowlist.

The skill never contains evidence-enforcement logic that is absent from the
controller.

## 7.2 Restricted runner

`agents/evidence-review-runner.md` defines the fixed L0-first loop. It may:

1. call `start` once with the supplied invocation capability;
2. evaluate only the returned validated Evidence payload;
3. call `advance` with one schema-valid response;
4. repeat only while status is `awaiting_response`; and
5. return the terminal Markdown and structured status.

The runner has no independent path to Git, source, the backend, audit files, or
other tools.

## 7.3 Capability manager

`capabilities.py` mints, validates, consumes, expires, and revokes invocation
and run capabilities. It uses `secrets` for 256-bit random values and stores
only domain-separated hashes.

An invocation capability binds:

```text
project canonical real path
plugin version
Claude session ID
issued time
expiry time
consumed state
```

It is single-use and expires after five minutes. `start` atomically consumes
it and returns a run capability bound to the review occurrence, current
attempt, immutable snapshot, and policy digest. Run capabilities expire after
configured inactivity, rotate when an attempt restarts, and are revoked at
terminal state.

Capability records use exclusive no-follow creation and an exclusive POSIX
file lock for compare-and-consume. Plaintext tokens appear only in the injected
fork prompt and MCP call arguments. They never appear in command arguments,
environment variables, persistent files, diagnostics, audit events, or reports.
Hash comparison is constant-time.

The injected authorization command does not interpolate `$ARGUMENTS` into a
shell command. User arguments reach the fork only as inert prompt data and are
parsed again by `start`; shell authorization receives only host-supplied plugin,
project, and session bindings. The runner is instructed to pass the rendered
arguments unchanged, while the controller independently restricts them to the
safe V1 grammar and reports the resolved comparison and case scope.

The capability intentionally does not authorize arbitrary model-selected
executables or output paths. Those fields do not exist in the model-facing
schema.

## 7.4 MCP transport

`mcp_server.py` implements only the MCP initialization, tool listing, tool
calls, cancellation, ping, and error behavior required by the supported Claude
Code versions. It exposes only `start` and `advance`, applies request byte
limits before decoding, and delegates every accepted call to the shared
controller.

Calls are serialized per review occurrence. A duplicate or concurrent call
with the same run capability loses the atomic consume-and-rotate race, returns
a typed conflict, and executes zero backend operations. Cancellation terminates
the active backend process group and records an incomplete attempt before a
replacement call can proceed.

It opens no network socket and contains no parallel validation or reporting
implementation.

## 7.5 Backend adapter

`backend.py` is the only component aware of the production VERITAS executable
and subcommand spelling. It translates canonical JSON into immutable plugin
models and provides the interface:

```text
handshake
pin_snapshot
discover_candidates
get_case
execute
request_proof
```

The same interface is implemented by the deterministic fake practice backend.

## 7.6 Controller

`controller.py` owns candidate order, snapshot attempts, L0-to-L1/L2 state,
budgets, response admission, operation dispatch, proof binding, audit events,
stop conditions, and terminal aggregation. It is claim-agnostic and does not
reinterpret defect semantics already expressed in EIR.

## 7.7 Validation and reporting

`validation.py` rejects malformed, mixed-context, unauthorized, or unsafe
backend and model data before persistence or dispatch.

`reporting.py` derives canonical JSON and escaped Markdown from validated case
state. It never copies a model verdict into an authoritative field.

# 8. Model-facing tool contract

The logical MCP operations are versioned as `veritas.review.tool.v1`:

```text
start(invocation_capability, review_options) -> ControllerOutput
advance(run_capability, review_response) -> ControllerOutput
```

## 8.1 Safe review options

The public skill and model-facing `ReviewOptions` accept only:

```text
--base <git-ref>
--head <git-ref>
--case <finding-id>
--max-cases <positive value not exceeding the policy default>
--practice <bundled allowlisted scenario>
```

`--base` and `--head` may be combined with `--case`; they define the pinned
comparison and target snapshot in which the finding must exist. `--case`
selects exactly that valid stable finding and bypasses candidate discovery. It
never broadens review and cannot resolve a finding from another snapshot.

Backend selection, arbitrary output paths, limit increases, and replacement
flags are deliberately absent from `ReviewOptions`.

## 8.2 Operator CLI

The standalone executable provides `authorize`, `serve-mcp`, `start`,
`advance`, and `export` entry points. Operator-only configuration may select a
backend, export a completed run, and replace an explicit destination:

```text
--backend <trusted executable path>
--output <path>
--json-output <path>
--force
```

These options are not exposed to the Evidence review runner. Production backend
selection normally comes from trusted plugin or environment configuration
resolved before model review.

## 8.3 Controller output

Every model-facing result has the exact envelope:

```text
protocol
status
review_id
attempt_id
run_capability
case_id
turn
evidence_level
payload
remaining_budgets
reasons
report
exit_code
```

Only fields valid for the current status may appear. Additional fields are
rejected. Status is one of:

```text
awaiting_response
response_rejected
complete
incomplete
failed
```

`run_capability` appears only in model-facing transport output, is never part
of canonical controller state or a report, and is redacted from diagnostics.

`start` and `advance` always return canonical structured data. Terminal
Markdown is the `report.markdown` member that the restricted runner returns to
the user. This removes any ambiguity between Markdown and JSON on stdout.

# 9. Versioned controller state

Persisted state uses `veritas.review.controller.v1`. Normative JSON Schemas and
golden examples define:

* immutable policy and budget ledger;
* review occurrence and attempt records;
* snapshot binding;
* candidate and case state;
* admitted responses and hypotheses;
* evidence deltas and proof results;
* append-only audit events; and
* case and aggregate reports.

Python immutable models and strict parsers are authoritative at runtime. The
schemas are interoperability and conformance artifacts, not runtime
dependencies.

The state machine is:

```text
authorize
  -> negotiate backend
  -> pin snapshot
  -> discover candidates or validate explicit case
  -> validate first EIR-L0
  -> awaiting_response
       -> admit response
       -> dispatch authorized operations
       -> validate delta or proof
       -> awaiting_response | case terminal
  -> next candidate
  -> aggregate terminal report
```

# 10. Public backend wire contract

The public subprocess protocol is `veritas.review.backend.v1`. Its normative
schema and examples are checked in beneath `schemas/` so the plugin and backend
can be implemented independently.

The backend is invoked with exactly one subcommand:

```text
handshake
pin-snapshot
discover
get-case
execute
request-proof
```

Every subcommand reads exactly one bounded canonical JSON request from stdin,
including `handshake`. Every response writes exactly one canonical JSON object
to stdout and diagnostics only to stderr.

The common request envelope is:

```text
protocol
request_id
operation
parameters
```

The common success envelope is:

```text
protocol
request_id
ok = true
result
```

The common failure envelope is:

```text
protocol
request_id
ok = false
error {
    code
    message
    retryable
    details
}
```

Unknown keys, missing keys, duplicate keys, mismatched request IDs, unsupported
protocols, an `operation` that does not match the invoked subcommand, and an
envelope whose `ok` branch is internally inconsistent are protocol failures.

## 10.1 Handshake

The handshake result declares:

```text
backend_protocol_versions[]
eir_schema_versions[]
operations[]
claim_kinds[]
proof_backends[]
authority_bearing_producers[]
backend_version
analyzer_versions[]
```

V1 requires `veritas.review.backend.v1` and `eir.v1`. Unsupported major
versions stop before snapshot pinning.

## 10.2 Snapshot binding

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
analyzer_versions[]
```

Every evidence-bearing response repeats this binding. Stable IDs never excuse
a context mismatch.

## 10.3 Candidate discovery

Candidate records contain:

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

The set also contains completeness, stable truncation reasons, examined count,
and query provenance. The controller sorts by stable finding identity before
applying its case limit. Empty truncated discovery is incomplete, never no
findings.

## 10.4 Evidence operations

V1 recognizes only:

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

An Evidence envelope advertises the exact operation/target pairs allowed for
that turn. `execute` accepts only advertised non-proof operations.
`request-proof` accepts a typed obligation and backend preference from the
current case.

Every delta repeats its snapshot binding, identifies additions and unchanged
stable references, preserves denials and truncation, and contains no hidden
replacement of prior evidence.

`request_source` is permitted only for an advertised entity or source-anchor
reference when semantic evidence is insufficient. Its response is bound to the
same ProgramContext and source artifact, carries omitted-region metadata, and
is charged against independent line and byte budgets.

A proof result binds:

```text
proof obligation ID
canonical predicate
requested and actual backend
authority-bearing producer identity
snapshot binding
input evidence and dependency IDs
status
counterexample or certificate reference
resource outcome
```

Only a configured authority-bearing producer may return deciding proof state.
`TIMEOUT`, `UNSUPPORTED`, cancellation, and backend errors are non-deciding.

# 11. Review workflow

## 11.1 Authorization and start

The controller:

1. atomically consumes the invocation capability;
2. derives the canonical project from its binding;
3. validates safe review options;
4. loads trusted backend configuration;
5. negotiates protocol and EIR versions;
6. resolves the Git comparison;
7. pins one immutable backend snapshot;
8. discovers candidates or validates the explicit case;
9. sorts candidates and applies the case limit; and
10. creates a durable review occurrence and first attempt.

No candidate reaches the model until its complete L0 envelope, references,
provenance requirements, completeness metadata, and snapshot binding validate.

## 11.2 Git comparison

If `--base` is absent, the controller resolves the merge-base between the
target and symbolic `origin/HEAD`. If that reference is unavailable, it
requires an explicit base.

Every user-provided ref is first resolved with an option-safe command
equivalent to:

```text
git rev-parse --verify --end-of-options <ref>^{commit}
```

The resolved object ID, not the original token, is passed to later Git
operations. Argument arrays prevent shell injection; explicit option
termination prevents Git option injection.

`--head` defaults to the current tracked working snapshot, including staged
and unstaged tracked changes. Untracked files are excluded unless the VERITAS
project manifest explicitly admits them.

## 11.3 Initial turn

For each candidate the controller loads and validates EIR-L0. The runner sees:

```text
review, attempt, run, and ProgramContext identity
case ID and EvidenceID
primary claim and authoritative verification state
primary causal path
supporting and contradicting evidence
strongest unknowns and omissions
completeness and truncation
allowed operation/target pairs
remaining budgets
```

The initial payload excludes complete EIR-L2, full provenance closures, whole
source files, unrestricted paths, and repository tools.

## 11.4 Model response and admission

The runner returns one object matching `veritas-review-response-v1`:

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

Admission runs in this fixed order:

```text
byte limit and UTF-8
  -> duplicate-free exact JSON schema
  -> StableIds and complete context binding
  -> EIR semantic invariants
  -> citations and typed predicates
  -> operation/target authorization
  -> budget availability
  -> backend dispatch
```

Any failure before dispatch guarantees zero backend operations.

Accepted model propositions become immutable hypotheses with
`epistemic = INFERRED`. Caller-supplied epistemic or verification fields are
rejected.

## 11.5 Delta loop and stop conditions

Accepted operations execute in response order after all requests in the
response have passed admission. The controller does not partially dispatch a
response whose later request is invalid.

The next payload contains additions, denials, truncation, unchanged stable
references, and remaining budgets. It does not restate unchanged evidence.

A case stops when:

* validated authoritative evidence decides the claim;
* no allowed operation can resolve the strongest remaining uncertainty;
* the runner returns a supported likely/inconclusive assessment with no
  material request;
* the turn or evidence budget is exhausted; or
* a non-recoverable validation or backend error quarantines the case.

The controller never continues merely to consume remaining budget.

# 12. Epistemic and authority rules

The controller enforces:

1. Observations cite supplied fact, path, summary, unknown, omission, proof, or
   source-anchor references.
2. Model propositions are always `INFERRED`.
3. Attempts to create `MUST`, `MUST_NOT`, `PROVED`, `REFUTED`,
   `VERIFIED_SAFE`, or `VERIFIED_DEFECT` are rejected, not normalized.
4. Model confidence remains distinct from VERITAS epistemic state.
5. Unknowns, omissions, and truncated queries cannot become negative facts.
6. Guarded and path-sensitive evidence remains guarded and path-sensitive.
7. Contradicting evidence remains visible in every relevant turn and report.
8. Multiple model hypotheses may coexist; agreement does not promote them.
9. Proof obligations begin `PENDING` and name requested authoritative
   backends.
10. A deciding proof result must match the exact obligation, canonical
    predicate, evidence dependencies, producer allowlist, and snapshot.
11. A final verified state must already exist in validated EIR or arrive in a
    validated authority-bearing proof result.

This resolves an ambiguity in acceptance scenario 2 of the companion security
use-case spec. Its phrase "admitted only as an INFERRED hypothesis" means that
model text can never gain `MUST` authority; V1 uses the stricter behavior stated
in that document's response contract and in issue #79: a caller-supplied
authority state is rejected, after which one corrected response may express
the proposition as an `INFERRED` hypothesis.

# 13. Security boundary

The controller and capability architecture, not skill prose, form the
enforcement boundary.

## 13.1 Process execution

The plugin:

* resolves the configured Git and backend executables before model review;
* accepts production backend selection only from explicit trusted operator or
  plugin configuration, never from reviewed-repository content;
* executes argument arrays rather than shell-interpolated commands;
* never accepts a model-selected executable path;
* uses an explicit project working directory;
* launches backend processes in a new process group;
* supplies a scrubbed, documented environment allowlist;
* applies monotonic deadlines and kills the entire group on timeout;
* reads stdout and stderr concurrently in bounded binary chunks;
* rejects non-UTF-8 protocol data, duplicate keys, trailing JSON, and oversized
  output; and
* sanitizes bounded stderr before displaying it.

Backend test-only environment keys are permitted only when the selected
executable is the checked-in fake backend under an explicit practice policy.

## 13.2 Prompt injection and display safety

EIR fields, model text, paths, diagnostics, and source excerpts are untrusted
data. They cannot add instructions, tools, capabilities, operations, or
authority.

Validation rejects prohibited control characters, terminal escape sequences,
and bidirectional-formatting controls. JSON preserves admitted text as data.
Markdown rendering escapes raw HTML, links, fence delimiters, and other active
syntax. Untrusted path or source text never becomes a clickable destination or
raw terminal control sequence.

Semantic report identity is computed from normalized validated semantic fields
before presentation escaping. Escaping remains deterministic.

## 13.3 Context isolation

Facts from different repositories, revisions, build variants, analyzer
configurations, analysis runs, native projections, fact snapshots, or analyzer
version sets cannot coexist in one attempt.

A snapshot mismatch discards the complete attempt. No facts, model responses,
hypotheses, budgets, or proof results cross into the replacement attempt.

Claude Code may load the reviewed project's `CLAUDE.md` into the custom forked
agent. The controller therefore assumes the runner's entire instruction context
can be hostile. Project instructions cannot add tools, change the MCP schemas,
mint a capability, authorize an operation, relax a budget, or promote evidence.
They may at worst make the runner return invalid data, waste bounded budget, or
stop early, which produces a rejected, incomplete, or inconclusive review.

# 14. Budgets and policy

V1 defines positive limits for:

```text
candidate cases
turns per case
model response bytes and member counts
EIR bytes per turn and case
summary expansions
path expansions and alternate paths
provenance depth and nodes
source lines and bytes
proof requests, time, memory, and output
backend process time, stdout, and stderr
MCP request and response bytes
audit and report bytes
```

Defaults live in one frozen policy object whose canonical digest is included in
the attempt binding and audit record.

Model-facing options may lower `max_cases`. They cannot raise any limit. Limit
increases are operator configuration outside the Evidence review agent.

Every turn shows remaining semantic-operation budgets. Reaching a limit
produces a typed denial or truncation event and preserves the relevant frontier
or expandable reference.

Input field and collection limits are selected so the largest admitted state
has a bounded report. If rendering nevertheless exceeds the cap, that is an
internal invariant failure; the plugin never silently removes evidence to fit.

# 15. Audit persistence and identities

Default state lives under:

```text
${CLAUDE_PLUGIN_DATA}/veritas-evidence-review/
├── capabilities/
└── reviews/<review-id>/
    ├── review.json
    ├── events.jsonl
    ├── attempts/<attempt-id>/state.json
    ├── cases/<case-id>/turns/<turn>.json
    ├── cases/<case-id>/report.json
    ├── report.json
    ├── report.md
    └── complete
```

Plugin uninstall may delete `${CLAUDE_PLUGIN_DATA}` unless the user elects to
keep it. Documentation and terminal output must state this retention boundary.

The plugin distinguishes:

```text
review occurrence ID   local audit occurrence
attempt ID             one pinned-snapshot attempt
semantic report ID     canonical validated result identity
```

Occurrence IDs, capabilities, timestamps, checkout paths, and incidental
process metadata never enter semantic report identity.

The audit records the declared agent model selector, a host-provided resolved
model or occurrence identifier when available, the prompt-contract version,
Claude Code and plugin versions, backend and analyzer versions, requests,
denials, budgets, and report identity. If Claude Code cannot expose a resolved
model identifier to the plugin, the field is explicitly `unavailable`; the
plugin never invents one. Model metadata is reasoning provenance, not
program-fact provenance.

## 15.1 Filesystem rules

The plugin is POSIX-only in V1 and uses directory-relative file operations:

* audit directories are mode `0700` and files are `0600`;
* path components are opened relative to validated directory descriptors;
* traversal and symlinks in every controlled component are rejected with
  no-follow semantics;
* temporary files use unpredictable names and exclusive creation;
* checkpoints and reports use file `fsync`, same-directory atomic rename, then
  parent-directory `fsync`;
* selected exports use the same rules and never replace an existing target
  without an operator-supplied `--force`; and
* a completion marker is created and durably synced only after every referenced
  artifact is durable.

`events.jsonl` is single-writer, opened append-only with no-follow semantics,
and uses monotonic sequence numbers plus a hash chain. Each complete record is
written and synced before dependent checkpoint state. Recovery validates the
chain to the last complete record. An incomplete or corrupt tail quarantines
the attempt; it is never silently truncated or presented as complete.

# 16. Failure and recovery

The controller applies:

| Failure | Behavior |
| --- | --- |
| Missing capability or capability mismatch | Reject before controller/backend work |
| Disabled skill shell execution or unavailable allowed tool | Fail closed; no Bash fallback |
| Missing executable or incompatible protocol | Stop before snapshot pinning |
| Analysis/discovery failure | Mark aggregate incomplete |
| Empty truncated discovery | Report incomplete, never no findings |
| Invalid EIR or unresolved provenance/reference | Quarantine case before model review |
| Snapshot mismatch | Discard attempt and restart the complete attempt once |
| Second snapshot change | Stop as unstable |
| First invalid model response | Execute nothing and return one correction |
| Second invalid response | End case inconclusive |
| Denied or unsupported request | Record denial and continue only if useful work remains |
| Budget exhausted | Preserve frontier and end likely/inconclusive unless already decided |
| Proof timeout/unsupported/error | Preserve non-deciding proof result |
| Audit or report write failure | No completion marker and no partial selected artifact |

The audit retains a discarded first attempt and starts the replacement with a
new attempt ID and rotated run capability under the same review occurrence.

Candidate-level failures do not erase successful cases. Any quarantined or
unfinished candidate makes the aggregate result partial.

Stable exit precedence is:

```text
70  internal controller failure
69  backend unavailable or incompatible
65  invalid evidence or backend protocol data
64  usage, configuration, or invocation-capability error
20  partial or inconclusive review
10  complete review with one or more actionable findings
0   complete review with no actionable finding
```

Failure categories take precedence over completed-review categories. Partial
or inconclusive state takes precedence over actionable findings. Structured
output retains every available case result regardless of process precedence.
An actionable finding is a `LIKELY_DEFECT` or `VERIFIED_DEFECT` disposition.

# 17. Review result contract

Each case result contains:

```text
finding identity and ProgramContext
claim and authoritative verification state
derived disposition
short semantic explanation
primary source and sink anchors as data
supporting facts with epistemic state
contradicting facts
primary path, guards, and feasibility
assumptions, unknowns, omissions, denials, and truncation
proof obligations and authoritative results
model observations and hypotheses labeled INFERRED
recommended remediation or next evidence action
```

The controller derives dispositions from validated state. It never copies a
model verdict into an authoritative field.

Canonical case state separates execution status from disposition:

```text
case_status = complete | quarantined
disposition = VERIFIED_DEFECT | VERIFIED_SAFE |
              LIKELY_DEFECT | LIKELY_FALSE_POSITIVE | INCONCLUSIVE | null
```

`VERIFIED_DEFECT` and `VERIFIED_SAFE` require validated authority-bearing
state. Likely dispositions require an admitted model assessment plus its cited
deterministic evidence and unresolved blockers. A quarantined case has a null
disposition and makes the aggregate partial; a complete `INCONCLUSIVE` case
also yields the partial/inconclusive process category.

The aggregate result contains comparison identity, final snapshot binding,
backend and analyzer versions, candidate and disposition counts, incomplete
reasons, attempt history, and case report identities.

For a verified defect, the report leads with the authoritative proof or
counterexample. For a likely defect, it leads with deterministic evidence and
the unresolved blocker. For an inconclusive case, it names the exact missing
fact, unsupported operation, capability failure, or truncation reason.

Equivalent validated evidence and admitted responses produce byte-identical
canonical JSON and escaped Markdown. Occurrence metadata remains audit-only.

# 18. Testing strategy

## 18.1 Unit and adversarial tests

Test-first development covers:

* strict JSON, normative schemas, StableIds, typed predicates, exact fields,
  and canonical ordering;
* capability expiry, single use, replay, cross-project use, cross-session use,
  rotation, and revocation;
* MCP initialization, exact tool inventory, malformed JSON-RPC, unknown
  methods, cancellation, oversized messages, and concurrent calls;
* epistemic admission, citations, operation/target pairs, proof binding, and
  zero-call rejection;
* exact budgets and positive-limit validation;
* Git option injection, environment scrubbing, process-group timeout, and
  stdout/stderr caps;
* traversal, symlink races, interrupted writes, directory durability, event
  integrity, and completion-marker ordering; and
* Markdown, terminal, HTML, link, fence, control-character, escape-sequence,
  and bidirectional-text attacks.

Every rejected response or unauthorized request asserts zero backend calls.

## 18.2 Deterministic practice integration

The bundled fake backend covers:

* unsafe buffer overflow with expandable path evidence;
* safe counterevidence through a complete `MUST` dominating check;
* truncated discovery and evidence with explicit frontier and reason;
* unordered candidates normalized by stable finding identity;
* one snapshot change followed by successful restart;
* two snapshot changes ending as unstable;
* proof timeout and unsupported theory remaining non-deciding;
* authority-bearing proof as the only verified promotion path;
* multiple candidates with a quarantined case;
* invalid verified claims, invented IDs, cross-case references, hidden fields,
  malformed predicates, control characters, unauthorized source requests, and
  oversized data; and
* relocation into a path containing spaces plus byte-identical reruns across
  clean stores and alternate checkout roots.

Practice reports are permanently labeled practice. They are never described
as production VERITAS integration.

## 18.3 Claude Code capability tests

Acceptance runs verify:

* `claude plugin validate --strict`;
* namespaced manual skill discovery and no model invocation;
* foreground fork behavior;
* the runner sees exactly `start` and `advance`;
* Bash, file, search, web, Skill, Agent, and unrelated MCP tools are absent;
* direct MCP calls without an invocation capability fail before controller and
  backend work;
* disabled skill shell execution, blocked MCP policy, unavailable server, and
  unsupported Claude Code versions fail closed; and
* behavioral pressure cannot promote model evidence or bypass the controller.

The test matrix includes the documented minimum Claude Code version and the
current supported version. If authenticated Claude Code execution is
unavailable, behavioral checks are reported as unrun; unit and manifest tests
do not substitute for them.

## 18.4 Real-backend contract

When production entry criteria are present, the gated contract test exercises
handshake, snapshot pinning, discovery, EIR-L0, EIR deltas, fact explanation,
source budgets, proof dispatch, version rejection, provenance, completeness,
and final reporting against the real backend.

Without configuration it skips with exactly:

```text
real VERITAS review backend not configured
```

It never selects the fake backend as a substitute.

# 19. Acceptance criteria

The plugin practice system is complete when:

1. a local Claude Code session can explicitly invoke the namespaced skill;
2. the skill runs in a foreground fork with exactly two model-facing tools;
3. calls without a current explicit-invocation capability execute no review;
4. the runner cannot access repository, Bash, web, delegation, or unrelated
   MCP capabilities;
5. a base-to-working-snapshot comparison discovers deterministically ordered
   impacted cases;
6. `--case` reviews exactly one supplied valid finding;
7. EIR-L0 is always the first model payload and later turns are deltas;
8. the unsafe fixture produces an actionable non-verified report citing stable
   evidence;
9. the safe fixture preserves complete counterevidence and never relies on
   truncated absence;
10. truncated discovery or evidence produces a partial/inconclusive result;
11. every model-authored proposition remains `INFERRED`;
12. out-of-case references and unauthorized operations execute nothing;
13. mixed context or a repeated snapshot change stops safely;
14. proof timeout or unsupported theory cannot create a verified state;
15. only an authority-bearing, obligation-bound proof can promote state;
16. reports and audit artifacts are deterministic apart from declared
    occurrence metadata;
17. interruption cannot leave an apparently complete artifact;
18. display rendering cannot activate untrusted evidence or model text;
19. all Python, practice, Claude Code, plugin validation, license, build, and
    repository checks pass; and
20. the primary repository checkout remains clean on `main`.

Production qualification additionally requires the real-backend contract to
pass with zero skips and no fake substitution.

# 20. Delivery stages and implementation shape

Development has two explicit stages:

1. **Practice system:** normative protocols, capability isolation, MCP bridge,
   controller, fake backend, reports, and all non-real-backend tests.
2. **Production qualification:** the public backend and upstream Evidence IR
   dependencies satisfy the real contract with zero skips.

This revision intentionally amends two scope statements in issue #79. The
issue's exclusion of "MCP transport in V1" remains true for the public VERITAS
backend transport, but not for the minimal model-facing plugin bridge required
for capability isolation. Its ten-task checklist becomes the twelve-task shape
below so the capability manager, stdio MCP bridge, and restricted runner have
independent test-first boundaries. Once this spec and its revised plan are
approved, issue #79 must be updated to point at the published documents and
reflect those two changes before implementation begins.

The implementation plan shall use twelve TDD tasks:

1. plugin, launcher, and CLI skeleton;
2. normative schemas and immutable models;
3. subprocess backend and fake scenarios;
4. EIR and response admission;
5. capabilities, policy, budgets, and durable audit;
6. minimal standard-library stdio MCP transport;
7. restricted plugin agent and manual skill;
8. start, Git discovery, snapshot stability, and L0;
9. advance, deltas, proof dispatch, and correction;
10. deterministic reports and exit precedence;
11. complete practice workflow, adversarial corpus, and Claude evaluations;
12. real-backend gate and full repository verification.

The design branch remains documentation-only. Implementation begins only after
the amended spec and plan are reviewed and published, in a fresh dedicated
worktree from updated `main`. Plans and commands derive branch, repository, and
worktree paths dynamically; they do not hardcode a username, checkout path, or
stale design branch.

# 21. Future extensions

The stable controller and backend interfaces permit later additions without
changing V1 trust rules:

* a remote model-facing transport with equivalent capability semantics;
* GitHub or other review-comment publication after separate explicit
  authorization;
* additional claim-specific proof policies;
* evidence-diff presentation between completed Evidence cases;
* organization-managed budget and trust policies; and
* remote authority-bearing proof services.

Automatic edit hooks, unsolicited review, arbitrary source access, and model
selection of backends or output destinations remain out of scope until they
receive separate design and approval.

# 22. Design invariant

For every plugin review:

```text
explicit user authority
        creates
short-lived project-bound capability
        used only by
a tool-restricted Evidence review agent
        consuming
small immutable provenance-backed Evidence IR
        through
an allowlisted and budgeted controller
        where Claude may add
INFERRED hypotheses and pending proof obligations
        while only authoritative backends may create
verified program facts or final verified states
```

The plugin improves review usefulness without granting the model ambient
repository capabilities or weakening the Evidence IR epistemic boundary.
