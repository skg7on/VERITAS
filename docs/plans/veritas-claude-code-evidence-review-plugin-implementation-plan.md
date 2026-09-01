# VERITAS Claude Code Evidence Review Plugin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a distributable, explicitly invoked Claude Code plugin that
performs bounded, capability-isolated Evidence IR review without giving the
reasoning model ambient repository or shell access.

**Architecture:** A manual foreground-forked skill mints a short-lived
invocation capability and runs one restricted plugin agent. That agent can see
only the plugin-scoped `start` and `advance` MCP tools; a standard-library stdio
bridge delegates both calls to one deterministic controller, which validates
Evidence IR and invokes a public VERITAS subprocess backend. Append-only audit
state, immutable snapshot binding, exact budgets, and authority-aware reporting
form the enforcement boundary.

**Tech Stack:** Claude Code 2.1.218+, Claude plugin manifest/skill/agent/MCP
configuration, MCP JSON-RPC over stdio, Python 3.11+ standard library,
`unittest`, POSIX file/process APIs, Git CLI, `veritas.review.backend.v1`, and
`eir.v1`.

**Spec:** `docs/specs/veritas-claude-code-evidence-review-plugin-design-spec.md`

**Status:** Ready for implementation

## Global Constraints

- Before Task 1, use `superpowers:using-git-worktrees` to create a fresh
  implementation worktree from updated `main`; do not implement on the
  documentation branch or in the primary checkout.
- Before Task 1, issue #79 must link the merged design and plan, distinguish
  model-facing MCP from the subprocess backend contract, and show the twelve
  task boundaries in this plan.
- The primary checkout remains clean on `main`; all edits, tests, commits, and
  pushes occur in the implementation worktree.
- V1 requires Claude Code 2.1.218 or newer and must pass capability tests at
  both that floor and the current supported version before release.
- Use Python 3.11+ standard-library modules only. Do not add `package.json`,
  `pyproject.toml`, `requirements.txt`, or vendored runtime dependencies.
- V1 is POSIX-only. `fcntl`, process groups, `O_NOFOLLOW`, `fsync`, file mode
  `0600`, and directory mode `0700` are part of the contract.
- Every Python or executable source begins with the repository's full
  Apache-2.0 header. An executable shebang stays on line 1.
- Model-facing review options are limited to `--base`, `--head`, `--case`, a
  lower-only `--max-cases`, and an allowlisted `--practice` scenario.
- Backend selection, export destinations, and `--force` remain operator-only.
  Plaintext capabilities never appear in process arguments or environment
  variables.
- The restricted agent has exactly the plugin-scoped `start` and `advance`
  tools. It has no Bash, file, search, web, Skill, Agent, ToolSearch, memory,
  hooks, or additional MCP capability.
- The plugin never reads SummaryDB storage directly, imports reviewed-project
  Python, or executes a shell-interpolated Git or VERITAS command.
- Every model proposition remains `INFERRED`. Only validated Evidence IR or an
  allowlisted authority-bearing proof producer may create verified state.
- Fake scenarios are permanently labeled `practice`. An absent production
  backend skips the real contract with exactly
  `real VERITAS review backend not configured`; it never selects fake evidence
  as a substitute.
- Default state lives beneath `${CLAUDE_PLUGIN_DATA}`. Selected exports require
  an explicit operator path; replacement requires `--force`.
- Every production behavior follows RED, GREEN, REFACTOR. Each task ends with
  the task-specific tests, `git diff --check`, one focused commit, and a clean
  reviewer handoff.

### Portable worktree preflight

Run this once after the implementation worktree is created and again before
the final gate:

~~~bash
set -e
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_BRANCH="$(git branch --show-current)"
GIT_DIR_REAL="$(cd "$(git rev-parse --git-dir)" && pwd -P)"
GIT_COMMON_REAL="$(cd "$(git rev-parse --git-common-dir)" && pwd -P)"
test -n "$TASK_BRANCH"
test "$TASK_BRANCH" != main
test "$GIT_DIR_REAL" != "$GIT_COMMON_REAL"
test "$PWD" = "$TASK_ROOT"
~~~

---

## Planned File Map

### Plugin surface

- Create `plugins/veritas-evidence-review/.claude-plugin/plugin.json` for
  plugin identity, version, license, and discovery metadata.
- Create `plugins/veritas-evidence-review/.mcp.json` for the lifecycle-managed
  `veritas_review` stdio server.
- Create `plugins/veritas-evidence-review/agents/evidence-review-runner.md` for
  the exact two-tool reasoning context.
- Create `plugins/veritas-evidence-review/skills/evidence-review/SKILL.md` for
  explicit authorization, inert argument rendering, and foreground execution.
- Create
  `plugins/veritas-evidence-review/skills/evidence-review/references/response-contract.md`
  for the admitted model response.
- Create
  `plugins/veritas-evidence-review/skills/evidence-review/references/review-policy.md`
  for budgets, stop conditions, practice labels, and retention behavior.
- Create `plugins/veritas-evidence-review/bin/veritas-evidence-review` as the
  relocatable operator/test launcher.

### Normative schemas

- Create `plugins/veritas-evidence-review/schemas/veritas-review-tool-v1.schema.json`
  for `start`, `advance`, and `ControllerOutput`.
- Create
  `plugins/veritas-evidence-review/schemas/veritas-review-controller-v1.schema.json`
  for persisted policy, occurrence, attempt, case, and report state.
- Create
  `plugins/veritas-evidence-review/schemas/veritas-review-backend-v1.schema.json`
  for all six subprocess operations and their common envelopes.
- Create
  `plugins/veritas-evidence-review/schemas/veritas-review-response-v1.schema.json`
  for the model response.
- Create schema examples beneath
  `plugins/veritas-evidence-review/schemas/examples/` for handshake, start,
  advance, correction, terminal output, and persisted state.

### Standard-library implementation

- Create `plugins/veritas-evidence-review/scripts/veritas_review/__init__.py`
  for protocol/plugin versions.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/cli.py` for
  parser construction, trusted operator configuration, stdin/stdout discipline,
  and stable exit mapping.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/models.py` for
  frozen protocol types, strict JSON, StableIds, and canonical serialization.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/backend.py` for
  bounded subprocess execution and the `ReviewBackend` protocol.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/validation.py`
  for EIR, context, reference, predicate, operation, and response admission.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/capabilities.py`
  for invocation/run capability minting, hashing, locking, rotation, and
  revocation.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/controller.py`
  for policy, budgets, Git comparison, attempts, audit state, and review flow.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/mcp_server.py`
  for bounded JSON-RPC framing and the two logical MCP tools.
- Create `plugins/veritas-evidence-review/scripts/veritas_review/reporting.py`
  for canonical reports, escaped Markdown, semantic identity, exports, and exit
  precedence.

### Tests and fixtures

- Create focused `test_cli.py`, `test_schemas.py`, `test_models.py`,
  `test_backend.py`, `test_validation.py`, `test_capabilities.py`,
  `test_audit.py`, `test_mcp_server.py`, `test_controller_start.py`,
  `test_controller_advance.py`, `test_reporting.py`, and
  `test_plugin_integration.py` modules beneath
  `plugins/veritas-evidence-review/tests/`.
- Create `plugins/veritas-evidence-review/tests/support.py` for test-only clocks,
  literal StableId constructors, complete fixture builders, and backend-call
  spies. It never imports production serialization to compute expected bytes.
- Create executable fixture
  `plugins/veritas-evidence-review/tests/fixtures/fake_veritas_backend.py`.
- Create JSON scenarios beneath
  `plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/` for
  `unsafe`, `safe`, `truncated`, `unordered-candidates`,
  `snapshot-change-once`, `snapshot-change-twice`, `proof-timeout`,
  `proof-unsupported`, `authoritative-proof`, and `quarantined-case`.
- Create `plugins/veritas-evidence-review/tests/skill-evals/evaluations.json` and
  executable `run_evaluations.py` for authenticated Claude capability checks.

The existing `docs/specs/README.md` and `docs/plans/README.md` already link the
approved documents; implementation tasks do not rewrite those indexes.

---

### Task 1: Plugin, launcher, and CLI skeleton

**Files:**
- Create: `plugins/veritas-evidence-review/.claude-plugin/plugin.json`
- Create: `plugins/veritas-evidence-review/bin/veritas-evidence-review`
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/__init__.py`
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/cli.py`
- Create: `plugins/veritas-evidence-review/tests/test_cli.py`

**Interfaces:**
- Consumes: Python 3.11+ and the launcher's resolved plugin root.
- Produces: `build_parser() -> argparse.ArgumentParser`,
  `main(argv: Sequence[str] | None = None) -> int`, and the subcommands
  `authorize`, `serve-mcp`, `start`, `advance`, and `export`.

- [ ] **Step 1: Write the failing relocation and command-inventory tests**

~~~python
class CliSkeletonTest(unittest.TestCase):
    def test_help_runs_from_a_path_with_spaces(self) -> None:
        with tempfile.TemporaryDirectory(prefix="veritas plugin ") as directory:
            relocated = Path(directory) / "veritas-evidence-review"
            shutil.copytree(PLUGIN_ROOT, relocated)
            result = subprocess.run(
                [str(relocated / "bin" / "veritas-evidence-review"), "--help"],
                cwd="/tmp",
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ("authorize", "serve-mcp", "start", "advance", "export"):
            self.assertIn(command, result.stdout)

    def test_scaffold_subcommand_fails_without_emitting_json(self) -> None:
        result = run_executable("start")
        self.assertEqual(result.returncode, 64)
        self.assertEqual(result.stdout, "")
        self.assertIn("start unavailable in scaffold", result.stderr)
~~~

Define `run_executable(*arguments: str) -> subprocess.CompletedProcess[str]`
in `test_cli.py` as a direct `subprocess.run` wrapper around `EXECUTABLE` with
`capture_output=True`, `text=True`, and `check=False`.

- [ ] **Step 2: Run the test and verify RED**

~~~bash
python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_cli.py' -v
~~~

Expected: ERROR because the launcher and package do not exist.

- [ ] **Step 3: Create the manifest, package version, and relocatable launcher**

Use this manifest payload:

~~~json
{
  "$schema": "https://json.schemastore.org/claude-code-plugin-manifest.json",
  "name": "veritas-evidence-review",
  "displayName": "VERITAS Evidence Review",
  "version": "0.1.0",
  "description": "Capability-isolated, provenance-backed Evidence IR review",
  "author": {"name": "VERITAS Contributors"},
  "license": "Apache-2.0",
  "keywords": ["code-review", "evidence-ir", "security", "static-analysis"]
}
~~~

`__init__.py` exports `__version__ = "0.1.0"`. The executable contains the
license header immediately after its shebang and resolves imports without the
current working directory:

~~~python
#!/usr/bin/env python3
from pathlib import Path
import sys

PLUGIN_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN_ROOT / "scripts"))

from veritas_review.cli import main

raise SystemExit(main())
~~~

Mark it executable with `chmod +x`.

- [ ] **Step 4: Implement the parser skeleton and stable scaffold error**

~~~python
COMMANDS = ("authorize", "serve-mcp", "start", "advance", "export")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="veritas-evidence-review",
        description="Evidence IR code review for Claude Code",
    )
    parser.add_argument("--version", action="version", version=__version__)
    commands = parser.add_subparsers(dest="command")
    for name in COMMANDS:
        commands.add_parser(name)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return 0
    print(f"{args.command} unavailable in scaffold", file=sys.stderr)
    return 64
~~~

- [ ] **Step 5: Verify GREEN, validate the manifest, and commit**

~~~bash
python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_cli.py' -v
claude plugin validate plugins/veritas-evidence-review --strict
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): scaffold Evidence IR review command"
~~~

Expected: relocation and CLI tests pass; strict validation exits 0.

---

### Task 2: Normative schemas and immutable models

**Files:**
- Create: `plugins/veritas-evidence-review/schemas/veritas-review-tool-v1.schema.json`
- Create: `plugins/veritas-evidence-review/schemas/veritas-review-controller-v1.schema.json`
- Create: `plugins/veritas-evidence-review/schemas/veritas-review-backend-v1.schema.json`
- Create: `plugins/veritas-evidence-review/schemas/veritas-review-response-v1.schema.json`
- Create: the seven named JSON examples in `schemas/examples/` listed in Step 4
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/models.py`
- Create: `plugins/veritas-evidence-review/tests/support.py`
- Create: `plugins/veritas-evidence-review/tests/test_schemas.py`
- Create: `plugins/veritas-evidence-review/tests/test_models.py`

**Interfaces:**
- Consumes: bounded UTF-8 JSON bytes.
- Produces: `strict_json_loads(data: bytes, max_bytes: int) -> object`,
  `canonical_json(value: object) -> bytes`, `parse_review_options`,
  `parse_backend_response`, `parse_evidence_envelope`,
  `parse_review_response`, and frozen protocol/state dataclasses.
- Produces for later tests:
  `stable_id(kind: str, nibble: str) -> str`, `fact_id`, `finding_id`,
  `path_id`, `review_id`, `make_snapshot(**overrides) -> SnapshotBinding`,
  `make_envelope(**overrides) -> EvidenceEnvelope`,
  `valid_response(**overrides) -> ReviewResponse`,
  `admission_policy(**overrides) -> AdmissionPolicyStub`, and
  `BackendSpy.calls: list[tuple[str, object]]`.

- [ ] **Step 1: Write failing strict JSON, StableId, and schema-example tests**

~~~python
class StrictJsonTest(unittest.TestCase):
    def test_duplicate_key_is_rejected(self) -> None:
        with self.assertRaisesRegex(ProtocolDataError, "duplicate JSON key: protocol"):
            strict_json_loads(
                b'{"protocol":"a","protocol":"b"}',
                max_bytes=128,
            )

    def test_bool_is_not_an_integer_budget(self) -> None:
        with self.assertRaisesRegex(UsageError, "max_cases must be an integer"):
            parse_review_options({"max_cases": True})

    def test_canonical_json_has_literal_expected_bytes(self) -> None:
        self.assertEqual(
            canonical_json({"z": [2, 1], "a": {"d": 4, "c": 3}}),
            b'{"a":{"c":3,"d":4},"z":[2,1]}',
        )


class SchemaExamplesTest(unittest.TestCase):
    def test_every_example_names_a_v1_protocol(self) -> None:
        for path in sorted(EXAMPLES.glob("*.json")):
            value = json.loads(path.read_text(encoding="utf-8"))
            self.assertRegex(value["protocol"], r"^veritas\.review\..+\.v1$")
~~~

Define `EXAMPLES = Path(__file__).resolve().parents[1] / "schemas" / "examples"`
in `test_schemas.py`.

- [ ] **Step 2: Run the focused tests and verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_models.py' -v
~~~

Expected: import failure because `models.py` does not exist.

- [ ] **Step 3: Define the exact frozen interfaces**

Implement these roots, then add frozen member types for candidates, allowed
operations, facts, paths, unknowns, omissions, proof obligations, and reports:

~~~python
TOOL_PROTOCOL = "veritas.review.tool.v1"
CONTROLLER_PROTOCOL = "veritas.review.controller.v1"
BACKEND_PROTOCOL = "veritas.review.backend.v1"
EIR_SCHEMA = "eir.v1"


@dataclass(frozen=True)
class ReviewOptions:
    base: str | None = None
    head: str | None = None
    case_id: str | None = None
    max_cases: int | None = None
    practice: str | None = None


@dataclass(frozen=True)
class GitComparison:
    project_realpath: str
    base_revision_id: str
    target_revision_or_snapshot_id: str
    working_tree_fingerprint: str


@dataclass(frozen=True)
class BackendCapabilities:
    backend_protocol_versions: tuple[str, ...]
    eir_schema_versions: tuple[str, ...]
    operations: tuple[str, ...]
    claim_kinds: tuple[str, ...]
    proof_backends: tuple[str, ...]
    authority_bearing_producers: tuple[str, ...]
    backend_version: str
    analyzer_versions: tuple[str, ...]


@dataclass(frozen=True)
class SnapshotBinding:
    repository_id: str
    base_revision_id: str
    target_revision_or_snapshot_id: str
    build_variant_id: str
    analysis_configuration_id: str
    analysis_run_id: str
    native_projection_id: str
    fact_snapshot_id: str
    working_tree_fingerprint: str
    analyzer_versions: tuple[str, ...]

    def context_key(self) -> tuple[object, ...]:
        return dataclasses.astuple(self)


@dataclass(frozen=True)
class Candidate:
    finding_id: str
    claim_kind: str
    severity: str
    subject_ref: str
    source_ref: str | None
    sink_ref: str | None
    semantic_delta_refs: tuple[str, ...]
    query_completion_fact_id: str


@dataclass(frozen=True)
class CandidateSet:
    items: tuple[Candidate, ...]
    completeness: str
    truncation_reasons: tuple[str, ...]
    examined_count: int
    query_provenance_refs: tuple[str, ...]


@dataclass(frozen=True)
class AllowedOperation:
    operation: str
    target_id: str


@dataclass(frozen=True)
class EvidenceEnvelope:
    protocol: str
    eir_schema: str
    case_id: str
    evidence_id: str
    evidence_level: str
    snapshot: SnapshotBinding
    primary_claim: Claim
    facts: tuple[Fact, ...]
    paths: tuple[EvidencePath, ...]
    unknowns: tuple[Unknown, ...]
    omissions: tuple[Omission, ...]
    allowed_operations: tuple[AllowedOperation, ...]
    completeness: str
    truncation_reasons: tuple[str, ...]
    provenance_complete: bool

    def reference_index(self) -> frozenset[str]:
        return collect_stable_ids(self)


@dataclass(frozen=True)
class ControllerOutput:
    protocol: str
    status: str
    review_id: str
    attempt_id: str
    run_capability: str | None
    case_id: str | None
    turn: int
    evidence_level: str | None
    payload: Mapping[str, object] | None
    remaining_budgets: Mapping[str, int]
    reasons: tuple[str, ...]
    report: Mapping[str, object] | None
    exit_code: int | None

    def to_json_object(self) -> dict[str, object]:
        return controller_output_to_json(self)
~~~

Define distinct `UsageError`, `ProtocolDataError`, `BackendUnavailable`,
`ResponseRejected`, `BudgetExhausted`, `CapabilityRejected`, and
`InternalControllerError` exceptions. Every parser rejects missing/additional
fields, duplicate keys, non-finite numbers, bool-as-int, non-lowercase
StableIds, and collections over their declared caps.

`models.py` begins with `from __future__ import annotations`; define frozen
`Claim`, `Fact`, `EvidencePath`, `Unknown`, `Omission`, `EvidenceRequest`,
`Hypothesis`, `ReviewResponse`, `AdmittedResponse`, `EvidenceDelta`,
`ProofObligation`, and `ProofResult` before the envelopes that reference them.

- [ ] **Step 4: Check in the four schemas and literal examples**

Each schema uses draft 2020-12, `additionalProperties: false`, exact required
fields, positive integer bounds, and protocol constants. Check in these
examples:

~~~text
backend-handshake-request.json
backend-handshake-response.json
tool-start-awaiting-response.json
tool-advance-correction.json
tool-terminal-report.json
controller-attempt-state.json
review-response.json
~~~

The backend request example includes `protocol`, `request_id`, `operation`, and
`parameters` even for `handshake`. Runtime parsers remain authoritative because
V1 does not add a JSON Schema library.

- [ ] **Step 5: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_models.py' -v
PYTHONPATH=plugins/veritas-evidence-review/scripts \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_schemas.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): define Evidence review protocols"
~~~

---

### Task 3: Bounded subprocess backend and practice scenarios

**Files:**
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/backend.py`
- Create: `plugins/veritas-evidence-review/tests/test_backend.py`
- Create: `plugins/veritas-evidence-review/tests/fixtures/fake_veritas_backend.py`
- Create: `tests/fixtures/backend_scenarios/unsafe.json`
- Create: `tests/fixtures/backend_scenarios/safe.json`
- Create: `tests/fixtures/backend_scenarios/truncated.json`
- Create: `tests/fixtures/backend_scenarios/unordered-candidates.json`

**Interfaces:**
- Consumes: a trusted executable path, `BackendProcessLimits`, and canonical
  backend request models.
- Produces: `ReviewBackend` methods `handshake`, `pin_snapshot`,
  `discover_candidates`, `get_case`, `execute`, and `request_proof`, plus
  `SubprocessReviewBackend._invoke(operation, parameters) -> object`.
- Produces: frozen `BackendProcessLimits(timeout_seconds, max_stdout_bytes,
  max_stderr_bytes)` for Task 5's `ReviewPolicy` to construct.
- `test_backend.py` defines
  `fake_backend(scenario: str, fault: str | None = None,
  timeout_seconds: float = 2.0, max_stdout_bytes: int = 1048576) -> SubprocessReviewBackend`.

- [ ] **Step 1: Write failing process and envelope tests**

~~~python
class SubprocessBackendTest(unittest.TestCase):
    def test_handshake_reads_and_echoes_the_common_request(self) -> None:
        result = fake_backend("unsafe").handshake()
        self.assertIn(BACKEND_PROTOCOL, result.backend_protocol_versions)
        self.assertIn(EIR_SCHEMA, result.eir_schema_versions)

    def test_operation_mismatch_is_protocol_data_error(self) -> None:
        with self.assertRaisesRegex(ProtocolDataError, "operation mismatch"):
            fake_backend("unsafe", fault="mismatched-operation").handshake()

    def test_timeout_kills_the_process_group(self) -> None:
        with self.assertRaisesRegex(BackendUnavailable, "timed out"):
            fake_backend("unsafe", fault="timeout", timeout_seconds=0.1).handshake()

    def test_oversized_stdout_is_rejected_before_json_decode(self) -> None:
        with self.assertRaisesRegex(ProtocolDataError, "stdout limit"):
            fake_backend("unsafe", fault="oversized", max_stdout_bytes=128).handshake()
~~~

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_backend.py' -v
~~~

Expected: import failure because `backend.py` is absent.

- [ ] **Step 3: Implement the exact backend protocol and process boundary**

Define the protocol:

~~~python
class ReviewBackend(Protocol):
    def handshake(self) -> BackendCapabilities:
        raise NotImplementedError

    def pin_snapshot(self, comparison: GitComparison) -> SnapshotBinding:
        raise NotImplementedError

    def discover_candidates(
        self, snapshot: SnapshotBinding, limit: int
    ) -> CandidateSet:
        raise NotImplementedError

    def get_case(
        self, snapshot: SnapshotBinding, finding_id: str, level: str
    ) -> EvidenceEnvelope:
        raise NotImplementedError

    def execute(
        self, snapshot: SnapshotBinding, case_id: str,
        requests: tuple[EvidenceRequest, ...]
    ) -> EvidenceDelta:
        raise NotImplementedError

    def request_proof(
        self, snapshot: SnapshotBinding, case_id: str,
        obligations: tuple[ProofObligation, ...]
    ) -> tuple[ProofResult, ...]:
        raise NotImplementedError
~~~

`SubprocessReviewBackend` uses `Popen` with binary pipes,
`start_new_session=True`, `close_fds=True`, an explicit cwd, and a scrubbed
environment. It writes one bounded canonical request for every operation,
including handshake. Two reader threads consume 64 KiB binary chunks; exceeding
either cap kills the process group. A monotonic timeout kills the group and
joins both readers. Stdout must contain one strict JSON response; stderr is
bounded, sanitized, and diagnostics-only.

- [ ] **Step 4: Implement the deterministic fake executable and first fixtures**

The fake accepts exactly these subcommands:

~~~text
handshake
pin-snapshot
discover
get-case
execute
request-proof
~~~

Every subcommand reads the common request envelope from stdin and emits one
common response envelope. `VERITAS_REVIEW_FAKE_SCENARIO` is honored only when
the executable realpath equals the checked-in fake fixture and practice policy
is active. Create `unsafe.json`, `safe.json`, `truncated.json`, and
`unordered-candidates.json` with complete snapshot bindings and literal stable
IDs. `VERITAS_REVIEW_FAKE_FAULT` accepts only `mismatched-operation`, `timeout`,
and `oversized` in process-boundary tests under that same realpath/practice
check; it is absent from normal scenario runs.

- [ ] **Step 5: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_backend.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): add bounded VERITAS backend adapter"
~~~

---

### Task 4: EIR and model-response admission

**Files:**
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/validation.py`
- Create: `plugins/veritas-evidence-review/tests/test_validation.py`

**Interfaces:**
- Consumes: expected `SnapshotBinding`, parsed `EvidenceEnvelope`, raw
  `ReviewResponse`, current case references, allowed operation/target pairs,
  and immutable policy.
- Produces:
  `validate_evidence_envelope(expected_snapshot: SnapshotBinding,
  envelope: EvidenceEnvelope, authority_producers: frozenset[str]) -> EvidenceEnvelope`,
  `validate_snapshot_match(expected: SnapshotBinding,
  actual: SnapshotBinding) -> None`,
  `parse_predicate(text: str) -> Predicate`, and
  `admit_review_response(envelope: EvidenceEnvelope,
  response: ReviewResponse, policy: AdmissionPolicy) -> AdmittedResponse`.
- Produces: an `AdmissionPolicy` protocol containing the response/member,
  source, proof, and operation limits consumed by Task 5's `ReviewPolicy`.

- [ ] **Step 1: Write failing authority and zero-dispatch tests**

~~~python
class ResponseAdmissionTest(unittest.TestCase):
    def test_model_verified_state_is_rejected(self) -> None:
        response = valid_response(extra={"verification_state": "VERIFIED_DEFECT"})
        with self.assertRaisesRegex(ResponseRejected, "additional property"):
            admit_review_response(make_envelope(), response, admission_policy())

    def test_authority_word_is_not_normalized_to_inferred(self) -> None:
        response = valid_response(
            hypotheses=[{"predicate": "MUST(range(src) > capacity(dst))"}]
        )
        with self.assertRaisesRegex(ResponseRejected, "authority-bearing"):
            admit_review_response(make_envelope(), response, admission_policy())

    def test_out_of_case_reference_executes_zero_backend_calls(self) -> None:
        spy = BackendSpy()
        response = valid_response(observation_refs=[fact_id("f")])
        with self.assertRaisesRegex(ResponseRejected, "outside current case"):
            admit_review_response(
                make_envelope(), response, admission_policy()
            )
        self.assertEqual(spy.calls, [])
~~~

Implement `test_mixed_context_is_rejected`,
`test_missing_completeness_is_rejected`, `test_invented_id_is_rejected`,
`test_cross_case_reference_is_rejected`,
`test_unadvertised_operation_target_is_rejected`,
`test_unsupported_proof_backend_is_rejected`,
`test_malformed_predicate_is_rejected`, `test_hidden_field_is_rejected`,
`test_non_utf8_is_rejected`, `test_control_bidi_and_escape_are_rejected`,
`test_oversized_member_is_rejected`, and
`test_unadvertised_source_anchor_is_rejected`. Each rejection asserts
`BackendSpy.calls == []`.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_validation.py' -v
~~~

Expected: import failure because `validation.py` is absent.

- [ ] **Step 3: Implement EIR, reference, and predicate validation**

`validate_evidence_envelope` requires `eir.v1`, one primary claim, exact
ProgramContext equality, unique valid StableIds, explicit epistemic state,
completeness/truncation metadata, visible contradictions/unknowns/omissions,
and an allowlist of the nine V1 semantic operations.

The predicate lexer admits identifiers, StableId references, integers,
Booleans, calls, parentheses, comparisons, `not`, `and`, `or`, and `implies`.
It rejects assignments, semicolons, braces, unsupported quotes, unconsumed
tokens, and caller-supplied authority constructors.

- [ ] **Step 4: Implement response admission in the fixed pre-dispatch order**

~~~python
def admit_review_response(
    envelope: EvidenceEnvelope,
    response: ReviewResponse,
    policy: AdmissionPolicy,
) -> AdmittedResponse:
    validate_response_shape(response, policy)
    validate_context(response, envelope.snapshot)
    validate_references(response, envelope.reference_index())
    validate_predicates(response)
    validate_operations(response, envelope.allowed_operations)
    validate_requested_budgets(response, policy)
    return with_inferred_hypotheses(response)
~~~

Accept only `needs_evidence`, `likely_defect`, `likely_false_positive`, and
`inconclusive`. Reject caller-supplied epistemic or verification fields rather
than rewriting them.

- [ ] **Step 5: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_validation.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): enforce Evidence review admission"
~~~

---

### Task 5: Capabilities, policy, budgets, and durable audit

**Files:**
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/capabilities.py`
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/controller.py`
- Create: `plugins/veritas-evidence-review/tests/test_capabilities.py`
- Create: `plugins/veritas-evidence-review/tests/test_audit.py`
- Modify: `plugins/veritas-evidence-review/scripts/veritas_review/cli.py`

**Interfaces:**
- Consumes: plugin data root, canonical project path, plugin version, Claude
  session ID, frozen policy, validated IDs, and injected clock/random sources.
- Produces: `CapabilityManager`, `InvocationGrant`, `RunGrant`, `ReviewPolicy`,
  `BudgetLedger`, `AuditStore`, `ReviewOccurrence`, `AttemptState`, and a working
  `authorize` CLI handler.

- [ ] **Step 1: Write failing capability tests**

~~~python
class CapabilityManagerTest(unittest.TestCase):
    def test_invocation_is_single_use_and_hash_only_at_rest(self) -> None:
        token = manager.mint_invocation(PROJECT, "0.1.0", SESSION)
        self.assertNotIn(token, read_all_text(DATA_ROOT))
        grant = manager.consume_invocation(token, PROJECT, "0.1.0", SESSION)
        self.assertEqual(grant.project, PROJECT.resolve())
        with self.assertRaisesRegex(CapabilityRejected, "consumed"):
            manager.consume_invocation(token, PROJECT, "0.1.0", SESSION)

    def test_cross_project_and_cross_session_use_are_rejected(self) -> None:
        token = manager.mint_invocation(PROJECT, "0.1.0", SESSION)
        with self.assertRaises(CapabilityRejected):
            manager.consume_invocation(token, OTHER_PROJECT, "0.1.0", SESSION)
        with self.assertRaises(CapabilityRejected):
            manager.consume_invocation(token, PROJECT, "0.1.0", "other-session")

    def test_concurrent_run_consume_has_one_winner(self) -> None:
        winners = consume_from_two_threads(manager, run_token)
        self.assertEqual(sum(result.ok for result in winners), 1)
~~~

Implement `test_invocation_expires_at_300_seconds`,
`test_run_expires_after_900_seconds_inactive`,
`test_plugin_version_mismatch_is_rejected`, `test_replay_is_rejected`,
`test_attempt_rotation_revokes_prior_capability`,
`test_terminal_state_revokes_capability`, `test_symlink_record_is_rejected`,
`test_plaintext_token_is_absent_from_storage`,
`test_digest_comparison_uses_hmac_compare_digest`, and
`test_token_is_absent_from_stderr`.

- [ ] **Step 2: Write failing budget, hash-chain, and durability tests**

~~~python
class AuditStoreTest(unittest.TestCase):
    def test_exact_budget_limit_is_allowed(self) -> None:
        ledger = BudgetLedger(
            review_policy(summary_expansions_per_case=2)
        )
        ledger.charge("expand_summary")
        ledger.charge("expand_summary")
        with self.assertRaises(BudgetExhausted):
            ledger.charge("expand_summary")

    def test_checkpoint_syncs_file_rename_and_parent(self) -> None:
        store = AuditStore(DATA_ROOT, sync_probe)
        store.write_checkpoint(REVIEW_ID, {"sequence": 2})
        self.assertEqual(
            sync_probe.events,
            ["fsync-temp", "replace", "fsync-parent"],
        )

    def test_corrupt_event_tail_quarantines_attempt(self) -> None:
        append_corrupt_tail(DATA_ROOT)
        with self.assertRaisesRegex(ProtocolDataError, "event chain"):
            AuditStore(DATA_ROOT).recover(REVIEW_ID)
~~~

`test_capabilities.py` defines `read_all_text(root: Path) -> str` by reading
only regular files beneath its temporary root and defines
`consume_from_two_threads` with one `threading.Barrier` plus two worker threads.
`test_audit.py` defines `review_policy(**overrides) -> ReviewPolicy` and a
`SyncProbe.events: list[str]` callback passed to `AuditStore`.

- [ ] **Step 3: Implement the frozen V1 policy**

Use these defaults:

~~~python
max_cases = 20
max_turns_per_case = 4
max_response_bytes = 64 * 1024
max_observations_per_response = 32
max_hypotheses_per_response = 16
max_evidence_requests_per_response = 16
max_proof_obligations_per_response = 2
max_source_requests_per_response = 4
max_string_bytes = 8 * 1024
max_eir_bytes_per_turn = 256 * 1024
max_eir_bytes_per_case = 1024 * 1024
summary_expansions_per_case = 4
path_expansions_per_case = 4
alternate_paths_per_case = 4
provenance_depth_per_case = 8
provenance_nodes_per_case = 256
source_lines_per_case = 80
source_bytes_per_case = 32 * 1024
proof_requests_per_case = 2
proof_time_seconds_per_case = 60.0
proof_memory_bytes = 512 * 1024 * 1024
max_proof_output_bytes = 1024 * 1024
backend_timeout_seconds = 120.0
max_backend_stdout_bytes = 8 * 1024 * 1024
max_backend_stderr_bytes = 1024 * 1024
max_mcp_request_bytes = 128 * 1024
max_mcp_response_bytes = 512 * 1024
max_audit_bytes_per_review = 16 * 1024 * 1024
max_report_bytes = 2 * 1024 * 1024
invalid_response_corrections = 1
invocation_ttl_seconds = 300.0
run_inactivity_seconds = 900.0
~~~

The canonical policy digest enters attempt and run-capability binding. A model
option may lower `max_cases`; no model field raises a limit.

- [ ] **Step 4: Implement capability storage and atomic consume/rotate**

Mint 32 random bytes with `secrets.token_urlsafe(32)`. Persist only a
domain-separated SHA-256 digest and bindings. Use directory-relative no-follow
opens, mode `0600`, `fcntl.flock(LOCK_EX)`, `hmac.compare_digest`, and atomic
consume/rotate under the exclusive lock. The `authorize` handler reads project,
and session from host-supplied arguments and derives plugin version from
`veritas_review.__version__`; it does not accept or interpolate review options.

- [ ] **Step 5: Implement append-only audit and durable checkpoints**

Events contain monotonic sequence, previous hash, canonical payload, and event
hash. Write and `fsync` each complete event before the dependent checkpoint.
Checkpoints/reports use exclusive temporary siblings, file `fsync`, same-dir
`os.replace`, and parent-directory `fsync`. Reject traversal, symlinks,
incomplete tails, hash mismatch, and over-budget audit state.

- [ ] **Step 6: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests \
  -p 'test_capabilities.py' -v
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_audit.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): add review capabilities and durable state"
~~~

---

### Task 6: Minimal standard-library stdio MCP transport

**Files:**
- Create: `plugins/veritas-evidence-review/.mcp.json`
- Create: `plugins/veritas-evidence-review/scripts/veritas_review/mcp_server.py`
- Create: `plugins/veritas-evidence-review/tests/test_mcp_server.py`
- Modify: `plugins/veritas-evidence-review/scripts/veritas_review/cli.py`

**Interfaces:**
- Consumes: bounded newline-delimited JSON-RPC 2.0 messages and a
  `ReviewToolService` with `start` and `advance` methods.
- Produces: `serve_stdio(input_stream, output_stream, service) -> int`, MCP
  `initialize`, `ping`, `tools/list`, `tools/call`, and cancellation behavior.
- Produces:
  `ReviewToolService.start(invocation_capability: str,
  review_options: ReviewOptions) -> ControllerOutput`,
  `ReviewToolService.advance(run_capability: str,
  review_response: Mapping[str, object]) -> ControllerOutput`, and
  `ReviewToolService.cancel(request_id: str | int) -> None`.

- [ ] **Step 1: Write failing initialize and exact inventory tests**

~~~python
class McpServerTest(unittest.TestCase):
    def test_tools_list_exposes_only_start_and_advance(self) -> None:
        replies = run_messages(initialize(), request(2, "tools/list", {}))
        tools = replies[-1]["result"]["tools"]
        self.assertEqual([tool["name"] for tool in tools], ["start", "advance"])
        self.assertTrue(all("inputSchema" in tool for tool in tools))

    def test_start_without_capability_never_reaches_service(self) -> None:
        service = ServiceSpy()
        reply = run_tool_call(service, "start", {"review_options": {}})
        self.assertTrue(reply["result"]["isError"])
        self.assertEqual(service.calls, [])

    def test_oversized_line_is_rejected_before_json_decode(self) -> None:
        reply = run_raw(b"{" + b"x" * 131072 + b"}\n")
        self.assertEqual(reply["error"]["code"], -32600)
~~~

Implement `test_malformed_json_rpc_is_rejected`,
`test_duplicate_request_id_is_rejected`, `test_notification_has_no_reply`,
`test_unknown_method_is_rejected`, `test_invalid_tool_name_is_rejected`,
`test_additional_tool_argument_is_rejected`,
`test_success_has_structured_and_text_output`,
`test_text_output_matches_canonical_structured_output`,
`test_one_inflight_call_per_review`,
`test_concurrent_capability_use_has_one_winner`, and
`test_cancellation_kills_backend_process_group`.

In `test_mcp_server.py`, `initialize()` and `request()` return literal JSON-RPC
objects; `run_messages(*messages)` serializes them into an `io.BytesIO`, calls
`serve_stdio`, and strict-decodes every output line. `run_tool_call()` performs
initialize followed by one `tools/call`; `run_raw()` sends literal bytes.
`ServiceSpy.calls` records `(method_name, argument_object)` before returning a
literal valid `ControllerOutput`.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_mcp_server.py' -v
~~~

Expected: import failure because `mcp_server.py` is absent.

- [ ] **Step 3: Implement bounded JSON-RPC and two tool definitions**

Support MCP protocol versions `2025-03-26`, `2025-06-18`, and `2025-11-25`.
Read with `readline(max_mcp_request_bytes + 1)`, strict-decode, and write only
canonical JSON-RPC to stdout. Diagnostics use stderr. `tools/list` returns
`start` and `advance` in that order with exact input/output schemas and
read-only/idempotence annotations set conservatively to false where state is
consumed.

Successful calls return both canonical text and the complete structured
object:

~~~python
def encode_tool_result(output: ControllerOutput) -> dict[str, object]:
    structured = output.to_json_object()
    return {
        "content": [
            {
                "type": "text",
                "text": canonical_json(structured).decode("utf-8"),
            }
        ],
        "structuredContent": structured,
        "isError": False,
    }
~~~

- [ ] **Step 4: Configure the bundled server and CLI handler**

`.mcp.json` contains one server:

~~~json
{
  "mcpServers": {
    "veritas_review": {
      "type": "stdio",
      "command": "${CLAUDE_PLUGIN_ROOT}/bin/veritas-evidence-review",
      "args": ["serve-mcp"],
      "env": {
        "CLAUDE_PLUGIN_DATA": "${CLAUDE_PLUGIN_DATA}",
        "CLAUDE_PROJECT_DIR": "${CLAUDE_PROJECT_DIR}"
      }
    }
  }
}
~~~

`serve-mcp` builds host context from those trusted values and invokes
`serve_stdio`. Unknown/unavailable controller operations return typed tool
errors; they never fall back to Bash or a second transport.

- [ ] **Step 5: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_mcp_server.py' -v
claude plugin validate plugins/veritas-evidence-review --strict
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): expose capability-gated review tools"
~~~

---

### Task 7: Restricted plugin agent and manual skill

**Required sub-skill:** Use `superpowers:writing-skills` for the RED/GREEN skill
authoring cycle in this task.

**Files:**
- Create: `plugins/veritas-evidence-review/agents/evidence-review-runner.md`
- Create: `plugins/veritas-evidence-review/skills/evidence-review/SKILL.md`
- Create: both skill reference files
- Create: `plugins/veritas-evidence-review/tests/test_plugin_integration.py`
- Create: `plugins/veritas-evidence-review/tests/skill-evals/evaluations.json`

**Interfaces:**
- Consumes: explicit slash-command arguments, injected invocation capability,
  and the plugin-scoped MCP tools.
- Produces: `/veritas-evidence-review:evidence-review` and the internal
  `veritas-evidence-review:evidence-review-runner` agent.

- [ ] **Step 1: Write failing static capability tests before authoring files**

~~~python
EXPECTED_TOOLS = (
    "mcp__plugin_veritas-evidence-review_veritas_review__start",
    "mcp__plugin_veritas-evidence-review_veritas_review__advance",
)


class PluginCapabilityDefinitionTest(unittest.TestCase):
    def test_runner_has_exact_tool_allowlist(self) -> None:
        frontmatter = parse_frontmatter(AGENT_FILE)
        self.assertEqual(tuple(frontmatter["tools"]), EXPECTED_TOOLS)
        for forbidden in ("Bash", "Read", "WebFetch", "Skill", "Agent"):
            self.assertNotIn(forbidden, frontmatter["tools"])

    def test_skill_is_manual_foreground_fork(self) -> None:
        frontmatter = parse_frontmatter(SKILL_FILE)
        self.assertIs(frontmatter["disable-model-invocation"], True)
        self.assertEqual(frontmatter["context"], "fork")
        self.assertEqual(frontmatter["agent"], "evidence-review-runner")
        self.assertIs(frontmatter["background"], False)
        agent_frontmatter = parse_frontmatter(AGENT_FILE)
        self.assertEqual(agent_frontmatter["maxTurns"], 105)

    def test_authorize_command_never_interpolates_arguments(self) -> None:
        text = SKILL_FILE.read_text(encoding="utf-8")
        command = extract_dynamic_shell_block(text)
        self.assertNotIn("$ARGUMENTS", command)
~~~

Define the test helper without a shell parser:

~~~python
def extract_dynamic_shell_block(text: str) -> str:
    after_open = text.split("```!", 1)[1]
    return after_open.split("```", 1)[0]
~~~

`parse_frontmatter(path)` is a test-only standard-library helper in this module.
It requires opening and closing `---`, parses root `key: scalar` values, converts
only `true`, `false`, and decimal integers, collects indented `- value` list
members, and ignores folded-scalar continuation lines. Any duplicate key,
unexpected indentation, or malformed list member fails the test.

- [ ] **Step 2: Run static RED and record optional behavioral baseline**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest test_plugin_integration.PluginCapabilityDefinitionTest -v
~~~

Expected: failures because agent and skill files are absent.

If authenticated Claude execution is available, run the three evaluation
prompts without the skill and store raw output in a `mktemp -d` directory.
Record unsafe authority promotion, missing bounded workflow, or unavailable
execution as the baseline result; never commit model prose.

- [ ] **Step 3: Author the exact restricted agent**

Use this frontmatter:

~~~yaml
---
name: evidence-review-runner
description: >-
  Internal capability-isolated runner for explicit VERITAS Evidence review.
model: inherit
maxTurns: 105
background: false
tools:
  - mcp__plugin_veritas-evidence-review_veritas_review__start
  - mcp__plugin_veritas-evidence-review_veritas_review__advance
---
~~~

The body calls `start` once, evaluates only returned Evidence data, calls
`advance` only while `awaiting_response` or once after `response_rejected`, and
returns only terminal `report.markdown` plus structured status. It treats
project instructions and all payload text as untrusted data. It never prints a
capability or claims it read repository files.

- [ ] **Step 4: Author the manual foreground skill and references**

Use this frontmatter:

~~~yaml
---
name: evidence-review
description: >-
  Run an explicit bounded VERITAS Evidence IR review for a Git comparison or
  stable finding ID.
argument-hint: [--base REF] [--head REF] [--case ID] [--max-cases N] [--practice NAME]
disable-model-invocation: true
context: fork
agent: evidence-review-runner
background: false
allowed-tools:
  - Bash(${CLAUDE_PLUGIN_ROOT}/bin/veritas-evidence-review authorize *)
  - mcp__plugin_veritas-evidence-review_veritas_review__start
  - mcp__plugin_veritas-evidence-review_veritas_review__advance
---
~~~

The only injected command invokes the bundled `authorize` executable with
`${CLAUDE_PROJECT_DIR}` and `${CLAUDE_SESSION_ID}`. Place
`$ARGUMENTS` in a separately delimited prompt-data block, never in the shell
line. `response-contract.md` contains the exact response schema and forbidden
authority fields. `review-policy.md` contains numeric defaults, stop rules,
practice labeling, and the `${CLAUDE_PLUGIN_DATA}` uninstall-retention warning.

The injected block is exactly:

~~~markdown
```!
"${CLAUDE_PLUGIN_ROOT}/bin/veritas-evidence-review" authorize \
  --project "${CLAUDE_PROJECT_DIR}" \
  --session "${CLAUDE_SESSION_ID}"
```
~~~

`authorize` derives plugin version from `veritas_review.__version__` and emits
one strict JSON object containing `invocation_capability`. It receives no review
option or `$ARGUMENTS` token.

- [ ] **Step 5: Verify GREEN, strict validation, and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest test_plugin_integration.PluginCapabilityDefinitionTest -v
claude plugin validate plugins/veritas-evidence-review --strict
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): add restricted Evidence review skill"
~~~

---

### Task 8: Start, Git discovery, snapshot stability, and EIR-L0

**Files:**
- Modify: `scripts/veritas_review/controller.py`
- Modify: `scripts/veritas_review/cli.py`
- Modify: `scripts/veritas_review/mcp_server.py`
- Create: `tests/test_controller_start.py`
- Create: snapshot-change scenario fixtures

All paths in this and later task file lists are relative to
`plugins/veritas-evidence-review/`.

**Interfaces:**
- Consumes: `HostContext`, invocation capability, validated `ReviewOptions`,
  trusted backend configuration, Git repository, and `ReviewBackend`.
- Produces:
  `ReviewController.start(invocation_capability, review_options) -> ControllerOutput`
  with `awaiting_response`, `complete`, `incomplete`, or `failed` state.

- [ ] **Step 1: Write failing capability-first and discovery tests**

~~~python
class StartFlowTest(unittest.TestCase):
    def test_missing_capability_executes_no_git_or_backend(self) -> None:
        output = controller.start("missing", ReviewOptions())
        self.assertEqual(output.status, "failed")
        self.assertEqual(git_spy.calls, [])
        self.assertEqual(backend_spy.calls, [])

    def test_candidates_sort_before_case_limit(self) -> None:
        output = start_practice("unordered-candidates", max_cases=1)
        self.assertEqual(output.case_id, finding_id("1"))
        self.assertEqual(output.evidence_level, "L0")

    def test_case_override_uses_the_pinned_target_context(self) -> None:
        output = start_practice("unsafe", case_id=finding_id("7"))
        self.assertEqual(output.case_id, finding_id("7"))
        self.assertNotIn("discover", backend_spy.operations)

    def test_second_snapshot_change_is_incomplete(self) -> None:
        output = start_practice("snapshot-change-twice")
        self.assertEqual(output.status, "incomplete")
        self.assertIn("snapshot_changed_twice", output.reasons)
~~~

Implement `test_unsupported_protocol_fails`,
`test_invalid_option_grammar_fails`, `test_max_cases_may_only_lower_policy`,
`test_missing_origin_head_requires_base`,
`test_explicit_base_and_head_resolve_to_commits`,
`test_git_option_injection_is_inert`,
`test_tracked_dirty_state_changes_fingerprint`,
`test_untracked_files_are_excluded_without_manifest_admission`,
`test_invalid_l0_is_rejected`, `test_mixed_context_l0_is_rejected`,
`test_empty_truncated_discovery_is_incomplete`, and
`test_one_snapshot_change_restarts_successfully`.

`StartFlowTest.setUp` creates a temporary Git repository with `main` and
`origin/HEAD`, a `FakeClock`, a valid single-use invocation capability,
`GitSpy`, and `BackendSpy`. `start_practice()` selects only a checked-in
practice fixture and calls `ReviewController.start`; it never shells out to the
fixture directly.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_controller_start.py' -v
~~~

Expected: `ReviewController.start` is absent.

- [ ] **Step 3: Implement option-safe Git comparison**

Resolve refs with argument arrays equivalent to:

~~~text
git rev-parse --verify --end-of-options REF^{commit}
git merge-base BASE_OID TARGET_OID
~~~

Pass only resolved object IDs to later commands. Default base is the merge-base
with symbolic `origin/HEAD`; require explicit `--base` when unavailable.
Default target includes staged and unstaged tracked content. Exclude untracked
files unless the VERITAS project manifest admits them.

- [ ] **Step 4: Implement the fixed start sequence and attempt restart**

~~~python
def start(
    self,
    invocation_capability: str,
    review_options: ReviewOptions,
) -> ControllerOutput:
    grant = self.capabilities.consume_invocation(
        invocation_capability,
        self.host.project,
        self.host.plugin_version,
        self.host.session_id,
    )
    options = self.policy.validate_review_options(review_options)
    return self._start_occurrence(grant, options)
~~~

`_start_occurrence` negotiates backend/EIR versions, resolves comparison, pins
one snapshot, discovers/sorts/limits candidates or validates the exact case,
loads complete L0, validates context/provenance/completeness, persists the
attempt, and mints a run capability. A mismatch discards the entire attempt and
restarts once with a new attempt ID/capability; the second mismatch ends
incomplete. No candidate reaches the model before L0 validation succeeds.

- [ ] **Step 5: Wire MCP/CLI start without command-line tokens**

The MCP tool accepts structured `invocation_capability` and `review_options`.
The operator `start` command reads one bounded JSON object containing those
fields from stdin; it never accepts a capability flag or environment variable.
Both paths return the same canonical `ControllerOutput`.

- [ ] **Step 6: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_controller_start.py' -v
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_cli.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): start capability-bound Evidence reviews"
~~~

---

### Task 9: Advance, deltas, proofs, and correction

**Files:**
- Modify: `scripts/veritas_review/controller.py`
- Modify: `scripts/veritas_review/cli.py`
- Modify: `scripts/veritas_review/mcp_server.py`
- Create: `tests/test_controller_advance.py`
- Create: proof scenario fixtures

**Interfaces:**
- Consumes: one live run capability and one strict `ReviewResponse`.
- Produces:
  `ReviewController.advance(run_capability, review_response) -> ControllerOutput`
  with a rotated capability, correction, next delta/L0 case, or terminal state.

- [ ] **Step 1: Write failing atomic admission and proof tests**

~~~python
class AdvanceFlowTest(unittest.TestCase):
    def test_invalid_later_request_prevents_all_dispatch(self) -> None:
        started = start_practice("unsafe")
        response = response_with_valid_then_invalid_request()
        output = controller.advance(started.run_capability, response)
        self.assertEqual(output.status, "response_rejected")
        self.assertEqual(backend_spy.calls_after_start, [])

    def test_rejected_response_rotates_capability_once(self) -> None:
        started = start_practice("unsafe")
        first = controller.advance(started.run_capability, authority_response())
        self.assertEqual(first.status, "response_rejected")
        with self.assertRaises(CapabilityRejected):
            controller.advance(started.run_capability, valid_response())
        second = controller.advance(first.run_capability, authority_response())
        self.assertEqual(second.status, "incomplete")

    def test_timeout_and_unsupported_proof_do_not_verify(self) -> None:
        for scenario in ("proof-timeout", "proof-unsupported"):
            output = advance_requesting_proof(scenario)
            self.assertNotIn(
                output.payload["verification_state"],
                ("VERIFIED_SAFE", "VERIFIED_DEFECT"),
            )

    def test_allowlisted_authority_may_promote_exact_obligation(self) -> None:
        output = advance_requesting_proof("authoritative-proof")
        self.assertEqual(output.payload["verification_state"], "VERIFIED_DEFECT")
~~~

Implement `test_source_charges_lines_and_bytes`,
`test_exact_turn_and_evidence_budgets_are_allowed`,
`test_unchanged_reference_is_not_recounted`,
`test_delta_preserves_contradictions_guards_and_truncation`,
`test_cross_snapshot_delta_is_rejected`,
`test_proof_is_bound_to_dependencies`,
`test_proof_producer_mismatch_cannot_verify`,
`test_no_material_request_stops_case`,
`test_terminal_case_starts_next_candidate`,
`test_run_capability_replay_and_concurrency_have_one_winner`, and
`test_snapshot_change_during_advance_restarts_attempt`.

`AdvanceFlowTest.setUp` uses the same temporary repository, fake clock,
capability manager, and backend spy contract as `StartFlowTest`; every helper
first obtains a real `ControllerOutput` from `start` and passes its returned run
capability to `advance`.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_controller_advance.py' -v
~~~

Expected: `ReviewController.advance` is absent.

- [ ] **Step 3: Implement consume-validate-dispatch-rotate order**

~~~python
def advance(
    self,
    run_capability: str,
    review_response: Mapping[str, object],
) -> ControllerOutput:
    run = self.capabilities.consume_run(run_capability)
    state = self.audit.load_attempt(run.review_id, run.attempt_id)
    try:
        parsed = parse_review_response(review_response, self.policy)
        admitted = admit_review_response(state.envelope, parsed, self.policy)
        self.budgets.preflight(state, admitted)
    except ResponseRejected as error:
        return self._correction_or_inconclusive(state, error)
    return self._dispatch_and_rotate(state, admitted)
~~~

All requests in the response pass admission and budget preflight before the
first backend call. Dispatch in response order. Validate every returned binding
and StableId before merging additions. Preserve unchanged refs, denials,
frontier, guards, contradictions, unknowns, omissions, and truncation.

- [ ] **Step 4: Implement proof authority and restart behavior**

Only exact obligation/predicate/dependency/snapshot matches from handshake-
allowlisted authority producers can promote state. Timeouts, cancellation,
unsupported theory, and backend errors remain non-deciding. Any snapshot
mismatch discards facts, hypotheses, budgets, and proofs from the attempt;
restart the full occurrence once and return a fresh L0/capability.

- [ ] **Step 5: Wire MCP/CLI advance without command-line tokens**

MCP receives `run_capability` and `review_response` as exact tool arguments.
The operator command reads both from bounded stdin. A correctable rejection is
structured output, not stderr-only failure. Terminal output omits
`run_capability` and revokes it.

- [ ] **Step 6: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_controller_advance.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): add bounded Evidence refinement loop"
~~~

---

### Task 10: Deterministic reports, safe rendering, and exit precedence

**Files:**
- Create: `scripts/veritas_review/reporting.py`
- Create: `tests/test_reporting.py`
- Modify: `scripts/veritas_review/controller.py`
- Modify: `scripts/veritas_review/cli.py`

**Interfaces:**
- Consumes: validated occurrence/attempt/case state.
- Produces: `derive_case_report`, `derive_aggregate_report`,
  `semantic_report_id`, `render_markdown`, `export_report`, and
  `classify_exit`.

- [ ] **Step 1: Write failing disposition and literal-golden tests**

~~~python
class ReportingTest(unittest.TestCase):
    def test_likely_defect_names_evidence_and_blocker(self) -> None:
        markdown = render_markdown(likely_overflow_state())
        self.assertIn("Disposition: LIKELY_DEFECT", markdown)
        self.assertIn("Hypothesis (INFERRED)", markdown)
        self.assertIn("Unresolved blocker: vendor validator postcondition", markdown)

    def test_quarantined_case_has_null_disposition(self) -> None:
        report = derive_case_report(quarantined_state())
        self.assertEqual(report.case_status, "quarantined")
        self.assertIsNone(report.disposition)

    def test_occurrence_metadata_does_not_change_semantic_bytes(self) -> None:
        left = aggregate_state(review_id=review_id("1"), timestamp="1")
        right = aggregate_state(review_id=review_id("2"), timestamp="2")
        self.assertEqual(canonical_report_json(left), canonical_report_json(right))

    def test_partial_precedes_actionable_finding(self) -> None:
        self.assertEqual(classify_exit(partial_with_likely_defect()), 20)
~~~

Implement `test_verified_safe_report`, `test_verified_defect_report`,
`test_likely_false_positive_report`, `test_inconclusive_report`,
`test_proof_timeout_cannot_verify`,
`test_empty_truncated_discovery_is_incomplete`,
`test_attempt_history_is_preserved`, `test_report_byte_cap_is_enforced`,
`test_internal_precedes_backend_unavailable`,
`test_backend_unavailable_precedes_protocol_data`,
`test_protocol_data_precedes_usage`, `test_usage_precedes_partial`,
`test_partial_precedes_actionable_finding`, and
`test_actionable_finding_precedes_clean` for precedence
`70, 69, 65, 64, 20, 10, 0`.

- [ ] **Step 2: Write failing display and export attacks**

Use literal fixtures containing raw HTML, Markdown links, triple fences,
terminal escape bytes, bidi formatting, controls, and path-looking text. Assert
no raw HTML, active link, executable escape, or clickable local path appears.
Assert a symlink destination and existing destination without `--force` fail
without partial replacement.

- [ ] **Step 3: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_reporting.py' -v
~~~

Expected: import failure because `reporting.py` is absent.

- [ ] **Step 4: Implement canonical state projection and dispositions**

Separate `case_status = complete | quarantined` from dispositions
`VERIFIED_DEFECT`, `VERIFIED_SAFE`, `LIKELY_DEFECT`,
`LIKELY_FALSE_POSITIVE`, `INCONCLUSIVE`, or null. Verified dispositions derive
only from validated authority state. Likely dispositions include the admitted
assessment, cited deterministic evidence, contradictions, and unresolved
blockers. Exclude capabilities, occurrence IDs, timestamps, checkout paths,
process metadata, and model occurrence from semantic identity.

- [ ] **Step 5: Implement escaped Markdown, durable export, and CLI output**

Escape raw HTML, links, fences, controls, terminal escapes, and bidi controls
deterministically. Render source anchors as inert data. Write exports using the
AuditStore no-follow/temp/fsync/rename/parent-fsync path. `--force` remains an
operator-only `export` option. Terminal MCP results carry Markdown in
`report.markdown`; stdout never mixes a second human format with JSON.

- [ ] **Step 6: Verify GREEN and commit**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_reporting.py' -v
git diff --check
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): render deterministic Evidence reviews"
~~~

---

### Task 11: Complete practice workflow, adversarial corpus, and Claude evaluations

**Files:**
- Complete: all JSON scenarios under `tests/fixtures/backend_scenarios/`
- Modify: `tests/test_plugin_integration.py`
- Create: `tests/skill-evals/evaluations.json`
- Create: `tests/skill-evals/run_evaluations.py`
- Modify: skill references when an evaluation exposes an instruction gap

**Interfaces:**
- Consumes: installed plugin directory, fake practice backend, synthetic Git
  repositories, optional authenticated Claude CLI.
- Produces: full practice-system proof, relocation proof, exact capability
  inventory evidence, and explicit run/unrun Claude evaluation results.

- [ ] **Step 1: Write failing end-to-end practice tests**

~~~python
class PracticeWorkflowTest(unittest.TestCase):
    def test_unsafe_case_finishes_actionable_but_not_verified(self) -> None:
        report = run_complete_practice("unsafe", likely_defect_response())
        self.assertEqual(report["disposition"], "LIKELY_DEFECT")
        self.assertNotEqual(report["verification_state"], "VERIFIED_DEFECT")
        self.assertTrue(report["practice"])

    def test_safe_case_uses_complete_must_counterevidence(self) -> None:
        report = run_complete_practice("safe", likely_false_positive_response())
        self.assertIn("dominating_check", report["contradicting_facts"][0]["kind"])
        self.assertFalse(report["relies_on_absence"])

    def test_relocated_plugin_produces_identical_semantic_report(self) -> None:
        left = run_relocated("plugin one", "unsafe")
        right = run_relocated("plugin two", "unsafe")
        self.assertEqual(left.semantic_json, right.semantic_json)
~~~

Implement `test_each_of_ten_scenarios_reaches_expected_terminal_state`,
`test_multi_case_quarantine_continues_review`,
`test_clean_store_reruns_are_byte_identical`,
`test_alternate_checkout_roots_are_semantically_identical`,
`test_plugin_path_with_spaces_works`,
`test_malformed_backend_envelopes_fail_closed`,
`test_interruption_revokes_live_capability`,
`test_corrupt_audit_tail_quarantines_attempt`, and
`test_rejected_request_executes_zero_backend_calls`.

- [ ] **Step 2: Verify integration RED before completing fixtures**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest test_plugin_integration.PracticeWorkflowTest -v
~~~

Expected: named missing scenarios or incomplete terminal behavior fail.

- [ ] **Step 3: Complete fixtures and make every practice report explicit**

Each fixture declares `practice: true`, complete protocol/context identities,
expected backend call log, allowed operations, truncation/frontier, proof
producer metadata, and literal expected report identity. The fake executable
refuses unknown scenario names and can never be selected by production backend
fallback.

- [ ] **Step 4: Define authenticated Claude evaluations**

`evaluations.json` contains these named cases:

~~~text
manual-namespaced-invocation
no-model-invocation
foreground-custom-agent
exact-two-tool-inventory
direct-mcp-without-capability
disabled-skill-shell-fails-closed
blocked-mcp-fails-closed
hostile-project-claude-md
authority-promotion-pressure
truncated-absence-pressure
source-comment-injection
~~~

`run_evaluations.py` creates a temporary project, invokes Claude with
`--plugin-dir`, captures stream JSON outside the repository, and checks the
resolved agent plus actual tool inventory. It refuses to run unless
`VERITAS_RUN_CLAUDE_EVALS=1`. Missing authentication records `unrun` and exits
success only for development; release qualification requires every case pass.

- [ ] **Step 5: Run practice GREEN and available Claude checks**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_*.py' -v
claude plugin validate plugins/veritas-evidence-review --strict
VERITAS_RUN_CLAUDE_EVALS=1 \
  python3 plugins/veritas-evidence-review/tests/skill-evals/run_evaluations.py
~~~

Expected: all standard-library/practice tests pass. The last command either
passes every authenticated case or reports exactly why host execution is
unavailable; do not relabel unavailable as pass.

- [ ] **Step 6: Commit**

~~~bash
git diff --check
git add plugins/veritas-evidence-review
git commit -m "test(plugin): prove isolated practice workflow"
~~~

---

### Task 12: Real-backend gate and full repository verification

**Files:**
- Modify: `tests/test_plugin_integration.py`
- Modify: plugin code only when the real contract exposes a specification
  mismatch; fix that mismatch with its own failing regression test.

**Interfaces:**
- Consumes: `VERITAS_REVIEW_BACKEND` when the public backend is configured.
- Produces: explicit real integration pass or the one exact skip reason, plus
  complete plugin/repository verification evidence.

- [ ] **Step 1: Write the gated real-backend contract test**

~~~python
class RealBackendContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        configured = os.environ.get("VERITAS_REVIEW_BACKEND")
        if not configured:
            raise unittest.SkipTest("real VERITAS review backend not configured")
        cls.backend = SubprocessReviewBackend.from_trusted_path(Path(configured))

    def test_complete_buffer_overflow_contract(self) -> None:
        capabilities = self.backend.handshake()
        self.assertIn(BACKEND_PROTOCOL, capabilities.backend_protocol_versions)
        self.assertIn(EIR_SCHEMA, capabilities.eir_schema_versions)
        comparison = real_overflow_comparison()
        snapshot = self.backend.pin_snapshot(comparison)
        candidates = self.backend.discover_candidates(snapshot, limit=20)
        self.assertGreater(len(candidates.items), 0)
        case = self.backend.get_case(snapshot, candidates.items[0].finding_id, "L0")
        self.assertEqual(case.evidence_level, "L0")
        self.assertTrue(case.provenance_complete)
        self.assertEqual(case.snapshot.context_key(), snapshot.context_key())
~~~

Implement `test_snapshot_pinning_contract`,
`test_discovery_completeness_contract`, `test_eir_delta_contract`,
`test_fact_explanation_contract`, `test_source_budget_contract`,
`test_proof_dispatch_contract`, `test_version_rejection_contract`, and
`test_final_report_contract`. The test never constructs a fake backend.

Define `real_overflow_comparison() -> GitComparison` in the same test module.
It requires `VERITAS_REVIEW_FIXTURE_PROJECT`, `VERITAS_REVIEW_FIXTURE_BASE`,
and `VERITAS_REVIEW_FIXTURE_HEAD`, resolves both refs with the option-safe Git
helper from Task 8, computes the tracked working-tree fingerprint, and contains
no fake scenario field. A missing fixture variable skips with the exact reason
`real VERITAS review fixture comparison not configured`; the zero-skip release
gate must supply all three.

- [ ] **Step 2: Run the gate in the available mode**

When configured:

~~~bash
VERITAS_REVIEW_BACKEND="$(realpath "$VERITAS_REVIEW_BACKEND")" \
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest test_plugin_integration.RealBackendContractTest -v
~~~

Expected: PASS with zero skips.

When absent:

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest test_plugin_integration.RealBackendContractTest -v
~~~

Expected: one skip with exact reason
`real VERITAS review backend not configured`.

- [ ] **Step 3: Commit the real gate**

~~~bash
git diff --check
git add plugins/veritas-evidence-review/tests/test_plugin_integration.py
git commit -m "test(plugin): gate real Evidence review backend"
~~~

- [ ] **Step 4: Run complete plugin verification**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests \
  python3 -m unittest discover \
  -s plugins/veritas-evidence-review/tests -p 'test_*.py' -v
claude plugin validate plugins/veritas-evidence-review --strict
plugins/veritas-evidence-review/bin/veritas-evidence-review --help
plugins/veritas-evidence-review/bin/veritas-evidence-review --version
git diff --check
~~~

Expected: all non-gated tests pass, strict plugin validation has no warnings,
help/version exit 0, and the diff has no whitespace errors.

For release qualification, run the capability evaluations against explicit
floor and current binaries:

~~~bash
set -e
test -x "$VERITAS_CLAUDE_FLOOR_BIN"
test -x "$VERITAS_CLAUDE_CURRENT_BIN"
for claude_bin in "$VERITAS_CLAUDE_FLOOR_BIN" "$VERITAS_CLAUDE_CURRENT_BIN"; do
  VERITAS_RUN_CLAUDE_EVALS=1 \
    python3 plugins/veritas-evidence-review/tests/skill-evals/run_evaluations.py \
      --claude-bin "$claude_bin"
done
~~~

Expected: both binaries pass every capability evaluation; an unset binary or
unrun evaluation blocks release qualification.

- [ ] **Step 5: Run the complete license check including the plugin subtree**

Run the repository fingerprint from `.claude/rules/license-header-policy.md`,
then this plugin extension:

~~~bash
missing_plugin_headers=$(
  git ls-files plugins/veritas-evidence-review \
    | while IFS= read -r file; do
        case "$file" in
          *.py|*/bin/*)
            head -20 "$file" \
              | grep -q 'Licensed under the Apache License, Version 2.0' \
              || printf '%s\n' "$file"
            ;;
        esac
      done
)
test -z "$missing_plugin_headers"
~~~

Expected: both checks print no missing files.

- [ ] **Step 6: Run the mandatory clean build and full test suite**

~~~bash
set -e
TASK_ROOT="$(git rev-parse --show-toplevel)"
test "$PWD" = "$TASK_ROOT"
cmake -E remove_directory "$TASK_ROOT/build"
if test -n "${VERITAS_LLVM_PROJECT_BUILD_DIR:-}"; then
  cmake --preset default \
    -DLLVM_PROJECT_BUILD_DIR="$VERITAS_LLVM_PROJECT_BUILD_DIR"
else
  cmake --preset default
fi
cmake --build --preset default
ctest --test-dir build --output-on-failure
~~~

Expected: configure/build exit 0, expected binaries exist, and CTest reports
100% pass with no crash, timeout, or skipped required gate.

- [ ] **Step 7: Verify branch diff, task cleanliness, and primary cleanliness**

~~~bash
set -e
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_BRANCH="$(git branch --show-current)"
PRIMARY_ROOT="$(git worktree list --porcelain | awk '
  $1 == "worktree" { root=$2 }
  $1 == "branch" && $2 == "refs/heads/main" { print root; exit }
')"
test -n "$TASK_BRANCH"
test "$TASK_BRANCH" != main
test -n "$PRIMARY_ROOT"
test "$(git -C "$PRIMARY_ROOT" branch --show-current)" = main
test -z "$(git status --porcelain)"
test -z "$(git -C "$PRIMARY_ROOT" status --porcelain)"
git diff --check main...HEAD
git diff --stat main...HEAD
git diff --name-status main...HEAD
~~~

Expected: task and primary worktrees are clean; branch diff contains only the
plugin and approved documentation linkage. Do not push until every mandatory
pre-push check has passed.

---

## Plan Self-Review Traceability

| Approved design requirement | Implementing tasks |
| --- | --- |
| Manual invocation and foreground fork | 7, 11 |
| Exact plugin-scoped two-tool agent | 6, 7, 11 |
| Capability mint, binding, replay defense, rotation | 5, 6, 8, 9 |
| Fail closed without skill shell/MCP/agent restriction | 6, 7, 11 |
| Safe options; operator-only backend/export/force | 1, 5, 8, 10 |
| `veritas.review.tool.v1` | 2, 6 |
| `veritas.review.controller.v1` | 2, 5, 8, 9 |
| `veritas.review.backend.v1` | 2, 3, 12 |
| Strict EIR/context/response admission | 2, 4, 8, 9 |
| Git comparison, explicit case, deterministic order | 8 |
| L0-first bounded deltas and source exception | 4, 8, 9 |
| Authority-aware proof transition | 3, 4, 9, 12 |
| Snapshot discard and restart once | 5, 8, 9, 11 |
| Positive budgets and zero-call rejection | 4, 5, 9, 11 |
| Durable no-follow append-only audit | 5, 11 |
| Escaped deterministic reports and stable exits | 10, 11 |
| Practice system and exact real-backend skip | 3, 11, 12 |
| Claude host behavior at floor/current versions | 6, 7, 11 |
| POSIX, Python 3.11+, Apache-2.0, no dependencies | Global constraints, 1-12 |
| Fresh implementation worktree and full repository gate | Global constraints, 12 |

The plan deliberately separates practice completion from production
qualification. Task 11 is a fully testable deliverable without the future
public backend; Task 12 keeps production qualification explicit until the real
contract passes with zero skips.
