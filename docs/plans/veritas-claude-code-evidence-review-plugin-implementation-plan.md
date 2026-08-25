# VERITAS Claude Code Evidence Review Plugin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox ( - [ ] ) syntax for tracking.

**Goal:** Build a distributable, on-demand Claude Code plugin that performs bounded Evidence IR review through a deterministic Python controller and a public VERITAS backend.

**Architecture:** A namespaced Claude Code skill invokes one bundled executable. The executable loads a standard-library Python controller that negotiates a typed subprocess protocol, pins one analysis snapshot, discovers impacted cases, validates EIR and model responses, performs allowlisted evidence expansion, and renders deterministic reports. A real executable supplies production evidence; a deterministic fake executable makes the complete practice workflow testable before that backend is present.

**Tech Stack:** Claude Code plugin manifest and Agent Skill Markdown, Python 3.11+ standard library, unittest, Git CLI, VERITAS backend protocol veritas.review.backend.v1, Evidence IR schema eir.v1.

**Spec:** docs/specs/veritas-claude-code-evidence-review-plugin-design-spec.md

## Global Constraints

- Work only in the existing task worktree on branch codex/veritas-evidence-review-plugin-design; never edit the primary main checkout.
- Claude Code 2.1.196 or newer is required for plugin skill path substitutions; the development machine has 2.1.229.
- Use Python 3.11+ standard-library modules only. Do not add package.json, pyproject.toml, requirements.txt, or vendored dependencies.
- V1 targets POSIX hosts supported by the VERITAS C++ toolchain; process-group termination and POSIX file modes are part of the safety contract.
- Every Python or executable source file starts with the repository's full 2026 Apache-2.0 header. Executables keep the shebang on line 1.
- The plugin never reads SummaryDB storage directly, imports reviewed-repository Python, or invokes a shell-interpolated VERITAS command.
- Model propositions are admitted only as INFERRED. Only authority-bearing backend results may carry verified states.
- The production backend must support veritas.review.backend.v1 and eir.v1. If it is absent, fake-backend tests prove the practice system but do not count as a real VERITAS integration.
- Default execution writes only beneath CLAUDE_PLUGIN_DATA. A repository output file requires an explicit option, and replacement requires --force.
- Run every production test through a RED, GREEN, REFACTOR cycle. Record the expected failure reason before implementation.

---

## Planned File Map

### Plugin metadata and skill

- Create plugins/veritas-evidence-review/.claude-plugin/plugin.json — plugin identity, version, license, and discovery metadata.
- Create plugins/veritas-evidence-review/skills/evidence-review/SKILL.md — concise, manual-only review workflow.
- Create plugins/veritas-evidence-review/skills/evidence-review/references/response-contract.md — structured response and admission reference.
- Create plugins/veritas-evidence-review/skills/evidence-review/references/review-policy.md — budgets, stop conditions, and result interpretation.

### Executable and controller

- Create plugins/veritas-evidence-review/bin/veritas-evidence-review — relocatable executable launcher.
- Create plugins/veritas-evidence-review/scripts/veritas_review/__init__.py — package version.
- Create plugins/veritas-evidence-review/scripts/veritas_review/cli.py — argparse surface, stdin/stdout discipline, and exit mapping.
- Create plugins/veritas-evidence-review/scripts/veritas_review/models.py — frozen protocol models, strict JSON decoder, and canonical JSON.
- Create plugins/veritas-evidence-review/scripts/veritas_review/backend.py — subprocess backend interface.
- Create plugins/veritas-evidence-review/scripts/veritas_review/validation.py — EIR, context, reference, operation, and response admission.
- Create plugins/veritas-evidence-review/scripts/veritas_review/controller.py — policies, budgets, audit persistence, and review state machine.
- Create plugins/veritas-evidence-review/scripts/veritas_review/reporting.py — deterministic JSON and Markdown reports.

### Tests

- Create plugins/veritas-evidence-review/tests/test_cli.py — executable and public CLI behavior.
- Create plugins/veritas-evidence-review/tests/test_models.py — strict decoding and typed protocol parsing.
- Create plugins/veritas-evidence-review/tests/test_backend.py — subprocess isolation, caps, timeouts, and fake protocol.
- Create plugins/veritas-evidence-review/tests/test_validation.py — adversarial evidence and response admission.
- Create plugins/veritas-evidence-review/tests/test_controller.py — discovery, refinement, retries, budgets, and persistence.
- Create plugins/veritas-evidence-review/tests/test_reporting.py — dispositions, exit precedence, and deterministic artifacts.
- Create plugins/veritas-evidence-review/tests/test_plugin_integration.py — complete start/advance workflow.
- Create plugins/veritas-evidence-review/tests/fixtures/fake_veritas_backend.py — deterministic protocol executable.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/unsafe.json — unsafe overflow responses.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/safe.json — dominating-check counterevidence.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/truncated.json — incomplete discovery and expansion.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/unordered-candidates.json — deterministic discovery ordering.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/snapshot-change-once.json — one restartable context change.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/snapshot-change-twice.json — unstable context.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/proof-timeout.json — non-deciding proof timeout.
- Create plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/authoritative-proof.json — producer-authorized verification.
- Create plugins/veritas-evidence-review/tests/skill-evals/evaluations.json — behavioral skill scenarios and rubrics.

### Documentation indexes

- Modify docs/plans/README.md — add this project/tooling plan.

Every test module defines a local stable_id(kind, nibble) helper that returns kind + ":sha256:" + nibble repeated 64 times. Higher-level helpers construct complete real protocol models from the checked-in fake-backend scenarios; they do not mock controller methods or derive expected values through production helpers.

---

### Task 1: Plugin package and executable CLI skeleton

**Files:**
- Create: plugins/veritas-evidence-review/.claude-plugin/plugin.json
- Create: plugins/veritas-evidence-review/bin/veritas-evidence-review
- Create: plugins/veritas-evidence-review/scripts/veritas_review/__init__.py
- Create: plugins/veritas-evidence-review/scripts/veritas_review/cli.py
- Create: plugins/veritas-evidence-review/tests/test_cli.py

**Interfaces:**
- Consumes: Python 3.11+ and a filesystem path to the plugin root.
- Produces: veritas_review.cli.main(argv: Sequence[str] | None) -> int and executable command veritas-evidence-review.

- [ ] **Step 1: Write the failing executable smoke test**

Create test_cli.py with the Apache-2.0 header and this behavior:

~~~python
import subprocess
import unittest
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
EXECUTABLE = PLUGIN_ROOT / "bin" / "veritas-evidence-review"


class CliSmokeTest(unittest.TestCase):
    def test_help_runs_from_outside_plugin_directory(self) -> None:
        result = subprocess.run(
            [str(EXECUTABLE), "--help"],
            cwd="/tmp",
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Evidence IR code review", result.stdout)


if __name__ == "__main__":
    unittest.main()
~~~

The production break this test catches is a missing or non-relocatable launcher.

- [ ] **Step 2: Run the test and verify RED**

Run:

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_cli.py" -v
~~~

Expected: ERROR because bin/veritas-evidence-review does not exist.

- [ ] **Step 3: Add the manifest, package version, CLI parser, and launcher**

plugin.json:

~~~json
{
  "$schema": "https://json.schemastore.org/claude-code-plugin-manifest.json",
  "name": "veritas-evidence-review",
  "displayName": "VERITAS Evidence Review",
  "version": "0.1.0",
  "description": "On-demand, provenance-backed Evidence IR code review for Claude Code",
  "author": {
    "name": "VERITAS Contributors"
  },
  "license": "Apache-2.0",
  "keywords": [
    "code-review",
    "evidence-ir",
    "security",
    "static-analysis"
  ]
}
~~~

__init__.py exports:

~~~python
__version__ = "0.1.0"
~~~

cli.py initially implements:

~~~python
import argparse
from collections.abc import Sequence


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="veritas-evidence-review",
        description="Evidence IR code review for Claude Code",
    )
    parser.add_argument("--version", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.version:
        from . import __version__

        print(__version__)
    return 0
~~~

The launcher sets PLUGIN_ROOT from Path(__file__).resolve().parents[1], prepends PLUGIN_ROOT/scripts to sys.path, imports main, and exits with its return value. Mark it executable:

~~~python
#!/usr/bin/env python3
from pathlib import Path
import sys


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN_ROOT / "scripts"))

from veritas_review.cli import main


raise SystemExit(main())
~~~

Place the required Apache-2.0 header immediately after the shebang and before the imports.

~~~bash
chmod +x plugins/veritas-evidence-review/bin/veritas-evidence-review
~~~

- [ ] **Step 4: Verify GREEN and manifest validity**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_cli.py" -v
claude plugin validate plugins/veritas-evidence-review --strict
~~~

Expected: one unittest passes; plugin validation exits 0 without warnings.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): scaffold Evidence IR review command"
~~~

---

### Task 2: Strict protocol models and canonical JSON

**Files:**
- Create: plugins/veritas-evidence-review/scripts/veritas_review/models.py
- Create: plugins/veritas-evidence-review/tests/test_models.py

**Interfaces:**
- Consumes: UTF-8 JSON bytes from backend stdout and Claude response stdin.
- Produces: strict_json_loads(data: bytes, max_bytes: int) -> object, canonical_json(value: object) -> bytes, and immutable protocol models.

- [ ] **Step 1: Write failing strict-decoder and context tests**

~~~python
class StrictJsonTest(unittest.TestCase):
    def test_duplicate_key_is_rejected(self) -> None:
        with self.assertRaisesRegex(ProtocolDataError, "duplicate JSON key: version"):
            strict_json_loads(b'{"version":"a","version":"b"}', 1024)

    def test_snapshot_context_key_changes_with_fact_snapshot(self) -> None:
        left = make_snapshot(fact_snapshot_id=fact_id("1"))
        right = make_snapshot(fact_snapshot_id=fact_id("2"))
        self.assertNotEqual(left.context_key(), right.context_key())

    def test_canonical_json_sorts_object_keys_without_reordering_arrays(self) -> None:
        value = {"z": [2, 1], "a": {"d": 4, "c": 3}}
        self.assertEqual(
            canonical_json(value),
            b'{"a":{"c":3,"d":4},"z":[2,1]}',
        )
~~~

Use literal 64-character lowercase hexadecimal StableIds in helpers. Do not compute expected output through canonical_json.
Define fact_id(nibble) locally as "fact:sha256:" plus nibble repeated 64 times, and construct make_snapshot with literal valid IDs for every remaining SnapshotBinding field.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_models.py" -v
~~~

Expected: import failure because models.py does not exist.

- [ ] **Step 3: Implement exact immutable models**

models.py defines:

~~~python
@dataclass(frozen=True)
class BackendCapabilities:
    backend_protocol_versions: tuple[str, ...]
    eir_schema_versions: tuple[str, ...]
    operations: tuple[str, ...]
    claim_kinds: tuple[str, ...]
    proof_backends: tuple[str, ...]
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

    def context_key(self) -> tuple[str, ...]:
        return tuple(dataclasses.astuple(self))
~~~

Add frozen models for Candidate, QueryMetadata, CandidateSet, AllowedOperation, EvidenceEnvelope, EvidenceRequest, Hypothesis, ReviewResponse, AdmittedResponse, EvidenceDelta, ProofResult, and ControllerOutput. Define ReviewError, UsageError, ProtocolDataError, BackendUnavailable, ResponseRejected, BudgetExhausted, and InternalControllerError as distinct typed failures. Each parser rejects missing/additional fields and bool where int is required, validates positive budgets and StableIds against ^[a-z][a-z0-9_-]*:sha256:[0-9a-f]{64}$, preserves declared order, and converts mutable inputs to tuples.

strict_json_loads checks byte length before UTF-8 decoding, rejects NaN and infinities, and uses object_pairs_hook to reject duplicate keys. canonical_json uses sort_keys=True, separators=(",", ":"), ensure_ascii=False, and allow_nan=False.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_models.py" -v
~~~

Expected: strict decoding, StableId, context, and canonicalization tests pass.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review/models.py plugins/veritas-evidence-review/tests/test_models.py
git commit -m "feat(plugin): add strict review protocol models"
~~~

---

### Task 3: Subprocess backend and deterministic fake practice backend

**Files:**
- Create: plugins/veritas-evidence-review/scripts/veritas_review/backend.py
- Create: plugins/veritas-evidence-review/tests/test_backend.py
- Create: plugins/veritas-evidence-review/tests/fixtures/fake_veritas_backend.py
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/unsafe.json
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/safe.json
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/truncated.json
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/unordered-candidates.json

**Interfaces:**
- Consumes: one backend executable and canonical request dictionaries.
- Produces: ReviewBackend.handshake, pin_snapshot, discover_candidates, get_case, execute, and request_proof.

- [ ] **Step 1: Write failing real-process boundary tests**

~~~python
class SubprocessBackendTest(unittest.TestCase):
    def test_handshake_parses_complete_capabilities(self) -> None:
        backend = fake_backend("unsafe")
        capabilities = backend.handshake()
        self.assertIn("veritas.review.backend.v1", capabilities.backend_protocol_versions)
        self.assertIn("eir.v1", capabilities.eir_schema_versions)

    def test_timeout_terminates_backend(self) -> None:
        backend = fake_backend("timeout", timeout_seconds=0.1)
        with self.assertRaisesRegex(BackendUnavailable, "timed out"):
            backend.handshake()

    def test_oversized_stdout_is_rejected(self) -> None:
        backend = fake_backend("oversized", max_stdout_bytes=128)
        with self.assertRaisesRegex(ProtocolDataError, "stdout limit"):
            backend.handshake()
~~~

These tests catch accepting a hung process, unbounded output, or an incomplete response.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_backend.py" -v
~~~

Expected: import failure because backend.py and the fake executable do not exist.

- [ ] **Step 3: Implement the fake backend protocol**

The fake accepts exactly handshake, pin-snapshot, discover, get-case, execute, and request-proof. Handshake reads no stdin. Every other command reads one canonical JSON object from stdin. Successful commands emit one JSON object and no stdout logging. VERITAS_FAKE_SCENARIO selects test data.

unsafe.json contains one BUFFER_OVERFLOW candidate, complete EIR-L0, one permitted expand_path request, one L1 delta, stable context, and no authoritative proof. safe.json adds a MUST dominating check as contradicting evidence. truncated.json marks discovery and path completeness TRUNCATED with reason max_paths.

- [ ] **Step 4: Implement SubprocessReviewBackend**

Use subprocess.Popen with stdin/stdout/stderr pipes, start_new_session=True, close_fds=True, text=False, and a scrubbed environment containing PATH, LANG, LC_ALL, TMPDIR, and explicitly selected backend test keys.

~~~python
def _invoke(
    self,
    operation: str,
    request: Mapping[str, object] | None,
) -> Mapping[str, object]:
~~~

Serialize request with canonical_json. Use one bounded reader thread per output pipe, reading 64 KiB chunks; if either collector would exceed its cap, kill the process group and raise ProtocolDataError. Write and close stdin, wait only until the monotonic deadline, kill the group on timeout, join both readers, map nonzero status to BackendUnavailable, and strict-decode stdout. Do not use communicate, because it buffers unbounded output. Parse a complete typed model before returning.

- [ ] **Step 5: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_backend.py" -v
~~~

Expected: process, timeout, size-limit, stderr separation, malformed JSON, and structure tests pass.

- [ ] **Step 6: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review/backend.py plugins/veritas-evidence-review/tests/test_backend.py plugins/veritas-evidence-review/tests/fixtures
git commit -m "feat(plugin): add bounded VERITAS backend adapter"
~~~

---

### Task 4: Evidence and model-response admission

**Files:**
- Create: plugins/veritas-evidence-review/scripts/veritas_review/validation.py
- Create: plugins/veritas-evidence-review/tests/test_validation.py

**Interfaces:**
- Consumes: expected SnapshotBinding, EvidenceEnvelope, and raw ReviewResponse.
- Produces: validate_evidence_envelope, validate_snapshot_match, and admit_review_response -> AdmittedResponse.

- [ ] **Step 1: Write failing adversarial tests**

~~~python
class ResponseAdmissionTest(unittest.TestCase):
    def test_model_verified_state_is_rejected(self) -> None:
        response = valid_response(assessment="VERIFIED_DEFECT")
        with self.assertRaisesRegex(ResponseRejected, "assessment"):
            admit_review_response(envelope(), response, policy())

    def test_out_of_case_fact_reference_is_rejected(self) -> None:
        response = valid_response(observation_refs=(fact_id("f"),))
        with self.assertRaisesRegex(ResponseRejected, "outside current case"):
            admit_review_response(envelope(), response, policy())

    def test_hypothesis_is_admitted_as_inferred(self) -> None:
        admitted = admit_review_response(
            envelope(),
            valid_response(hypothesis_predicate="range(src) > capacity(dst)"),
            policy(),
        )
        self.assertEqual(admitted.hypotheses[0].epistemic, "INFERRED")
~~~

Add cases for unauthorized operation/target, hidden fields, malformed predicate, control characters, oversized response, mixed SnapshotBinding, missing query completeness, and unadvertised source request.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_validation.py" -v
~~~

Expected: import failure because validation.py does not exist.

- [ ] **Step 3: Implement validation**

validate_evidence_envelope requires eir.v1, L0/L1/L2, exactly one primary claim, complete SnapshotBinding equality, unique valid StableIds, explicit epistemic state, visible completeness/truncation, only nine V1 operations, and producer identity for authoritative state.

admit_review_response accepts four model assessments, validates every citation and operation/target pair, and creates immutable hypotheses with epistemic="INFERRED". Reject any caller-supplied epistemic field.

Implement a predicate lexer admitting identifiers, StableId refs, integer/Boolean literals, calls, parentheses, comparisons, not, and, or, and implies. Reject assignments, semicolons, braces, unsupported quotes, and unconsumed tokens.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_validation.py" -v
~~~

Expected: adversarial cases pass and validation triggers no backend operation.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review/validation.py plugins/veritas-evidence-review/tests/test_validation.py
git commit -m "feat(plugin): enforce Evidence IR review admission"
~~~

---

### Task 5: Policy, budgets, and atomic audit persistence

**Files:**
- Create: plugins/veritas-evidence-review/scripts/veritas_review/controller.py
- Create: plugins/veritas-evidence-review/tests/test_controller.py

**Interfaces:**
- Consumes: plugin data root, validated models, immutable ReviewPolicy.
- Produces: ReviewPolicy, BudgetLedger, AuditStore, RunState, and CaseState.

- [ ] **Step 1: Write failing budget and durability tests**

~~~python
class AuditAndBudgetTest(unittest.TestCase):
    def test_exact_limit_is_allowed_and_next_request_is_denied(self) -> None:
        ledger = BudgetLedger(ReviewPolicy(summary_expansions_per_case=2))
        ledger.charge("expand_summary")
        ledger.charge("expand_summary")
        with self.assertRaisesRegex(BudgetExhausted, "expand_summary"):
            ledger.charge("expand_summary")

    def test_checkpoint_replacement_is_atomic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = AuditStore(Path(directory))
            store.write_checkpoint("run-1", {"sequence": 1})
            store.write_checkpoint("run-1", {"sequence": 2})
            self.assertEqual(store.read_checkpoint("run-1"), {"sequence": 2})
            self.assertEqual(list(Path(directory).rglob("*.tmp")), [])
~~~

Add append-only sequence, traversal, existing output, --force, semantic nonce exclusion, file mode, and positive-limit tests.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.AuditAndBudgetTest -v
~~~

Expected: import failure because controller.py does not exist.

- [ ] **Step 3: Implement exact defaults**

~~~python
max_cases = 20
max_turns_per_case = 4
max_response_bytes = 64 * 1024
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
backend_timeout_seconds = 120.0
max_backend_stdout_bytes = 8 * 1024 * 1024
max_backend_stderr_bytes = 1024 * 1024
max_report_bytes = 2 * 1024 * 1024
~~~

Document that caps fit compact L0/L1 evidence, bound source exposure, and allow two proof strategies without an unbounded loop.

AuditStore validates IDs before paths, creates directories 0o700 and files 0o600, writes O_EXCL temporary siblings, flushes/fsyncs, then os.replace. Never follow an existing symlink.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.AuditAndBudgetTest -v
~~~

Expected: budget, permissions, traversal, atomicity, and determinism tests pass.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review/controller.py plugins/veritas-evidence-review/tests/test_controller.py
git commit -m "feat(plugin): add bounded review audit state"
~~~

---

### Task 6: Start, discovery, snapshot stability, and L0 turn

**Files:**
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/controller.py
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/cli.py
- Modify: plugins/veritas-evidence-review/tests/test_controller.py
- Modify: plugins/veritas-evidence-review/tests/test_cli.py
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/snapshot-change-once.json
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/snapshot-change-twice.json

**Interfaces:**
- Consumes: StartRequest(project, base, head, case_id, policy, backend_path).
- Produces: ReviewController.start -> ControllerOutput with awaiting_response, complete, or incomplete.

- [ ] **Step 1: Write failing start-flow tests**

~~~python
class StartFlowTest(unittest.TestCase):
    def test_candidates_are_sorted_by_stable_finding_id(self) -> None:
        output = start_with_scenario("unordered-candidates")
        self.assertEqual(output.current_case_id, finding_id("1"))

    def test_empty_truncated_discovery_is_incomplete(self) -> None:
        output = start_with_scenario("truncated")
        self.assertEqual(output.status, "incomplete")
        self.assertIn("max_paths", output.reasons)

    def test_second_snapshot_change_stops_as_unstable(self) -> None:
        output = start_with_scenario("snapshot-change-twice")
        self.assertEqual(output.status, "incomplete")
        self.assertIn("snapshot_changed_twice", output.reasons)
~~~

Add explicit-case, max-cases, unsupported protocol, absent origin/HEAD, and invalid-L0 tests.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.StartFlowTest -v
~~~

Expected: AttributeError because ReviewController.start is absent.

- [ ] **Step 3: Implement the fixed start sequence**

Resolve/contain project, negotiate versions, resolve explicit base or symbolic origin/HEAD merge-base using Git argument arrays, pin snapshot, discover/validate one case, sort finding IDs, apply max_cases with explicit truncation, load/validate L0, recheck binding, restart once on mismatch, persist start events, and emit first ControllerOutput.

Extend cli.py with all design section 7 start options. Emit canonical JSON only.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.StartFlowTest test_cli -v
~~~

Expected: discovery, case override, versions, base, snapshot retry, and CLI tests pass.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): start diff-driven Evidence IR reviews"
~~~

---

### Task 7: Response advance, evidence deltas, and terminal state

**Files:**
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/controller.py
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/cli.py
- Modify: plugins/veritas-evidence-review/tests/test_controller.py
- Modify: plugins/veritas-evidence-review/tests/test_cli.py
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/proof-timeout.json
- Create: plugins/veritas-evidence-review/tests/fixtures/backend_scenarios/authoritative-proof.json

**Interfaces:**
- Consumes: advance --run RUN_ID with one ReviewResponse JSON object on stdin.
- Produces: next ControllerOutput or completed/incomplete state.

- [ ] **Step 1: Write failing advance tests**

~~~python
class AdvanceFlowTest(unittest.TestCase):
    def test_expansion_returns_delta_and_unchanged_references(self) -> None:
        run = start_with_scenario("unsafe")
        output = advance(run, response_requesting("expand_path", path_id("1")))
        self.assertEqual(output.evidence_level, "L1_delta")
        self.assertEqual(output.unchanged_refs, (claim_id("1"),))

    def test_two_invalid_responses_end_case_inconclusive(self) -> None:
        run = start_with_scenario("unsafe")
        first = advance_raw(run, b'{"assessment":"VERIFIED_DEFECT"}')
        self.assertEqual(first.status, "response_rejected")
        second = advance_raw(run, b'{"assessment":"VERIFIED_SAFE"}')
        self.assertEqual(second.status, "incomplete")

    def test_proof_timeout_never_verifies_case(self) -> None:
        run = start_with_scenario("proof-timeout")
        output = advance(run, response_requesting_proof())
        self.assertNotIn(output.verification_state, {"VERIFIED_SAFE", "VERIFIED_DEFECT"})
~~~

Add zero-call unauthorized target, exact budget, source charging, turn limit, no-material-request, authoritative producer, and next-candidate tests.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.AdvanceFlowTest -v
~~~

Expected: AttributeError because ReviewController.advance is absent.

- [ ] **Step 3: Implement advance in fixed order**

Load checkpoint; reject completed/unknown runs; strict-decode bounded stdin; admit without backend calls; allow one correction; end second rejection INCONCLUSIVE; charge before dispatch; execute in response order; validate every delta binding; store hypotheses as INFERRED; merge by StableId while preserving contradiction/guards/truncation; emit delta or stop; advance candidate or complete run.

cli.py reads max_response_bytes plus one sentinel byte. Extra data is a data error. Response rejection is structured stdout because it is correctable protocol state.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_controller.AdvanceFlowTest test_cli -v
~~~

Expected: refinement, rejection, budget, proof, source, stop, and multi-case tests pass.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review plugins/veritas-evidence-review/tests
git commit -m "feat(plugin): add bounded evidence refinement loop"
~~~

---

### Task 8: Deterministic reports and exit precedence

**Files:**
- Create: plugins/veritas-evidence-review/scripts/veritas_review/reporting.py
- Create: plugins/veritas-evidence-review/tests/test_reporting.py
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/cli.py
- Modify: plugins/veritas-evidence-review/scripts/veritas_review/controller.py

**Interfaces:**
- Consumes: validated RunState.
- Produces: case/aggregate reports, Markdown, canonical JSON, and exit codes.

- [ ] **Step 1: Write failing literal-golden tests**

~~~python
class ReportingTest(unittest.TestCase):
    def test_likely_defect_names_unresolved_blocker(self) -> None:
        report = render_markdown(likely_overflow_state())
        self.assertIn("Disposition: LIKELY_DEFECT", report)
        self.assertIn("Unresolved blocker: vendor validator postcondition", report)
        self.assertIn("Hypothesis (INFERRED)", report)

    def test_partial_run_takes_exit_precedence_over_finding(self) -> None:
        self.assertEqual(classify_exit(partial_run_with_finding()), 20)

    def test_occurrence_nonce_does_not_change_report_bytes(self) -> None:
        left = render_markdown(run_state(nonce="a"))
        right = render_markdown(run_state(nonce="b"))
        self.assertEqual(left.encode(), right.encode())
~~~

Add safe, verified authority, proof timeout, truncation, quarantined case, and output replacement goldens.

- [ ] **Step 2: Verify RED**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_reporting.py" -v
~~~

Expected: import failure because reporting.py does not exist.

- [ ] **Step 3: Implement report derivation**

Case JSON order: finding_id, program_context, claim, verification_state, disposition, explanation, anchors, supporting_facts, contradicting_facts, primary_path, assumptions, unknowns, omissions, truncation, proof_obligations, authoritative_results, model_observations, inferred_hypotheses, recommended_next_action.

Aggregate JSON includes comparison, SnapshotBinding, versions, counts, incomplete reasons, and case report IDs. Markdown excludes nonce, timestamps, model occurrence ID, and paths.

Actionable means LIKELY_DEFECT or VERIFIED_DEFECT. Precedence is 70, 69, 65, 64, 20, 10, 0 among applicable categories.

- [ ] **Step 4: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_reporting.py" -v
~~~

Expected: content, golden, determinism, output safety, and exit tests pass.

- [ ] **Step 5: Commit**

~~~bash
git add plugins/veritas-evidence-review/scripts/veritas_review/reporting.py plugins/veritas-evidence-review/scripts/veritas_review/cli.py plugins/veritas-evidence-review/scripts/veritas_review/controller.py plugins/veritas-evidence-review/tests/test_reporting.py
git commit -m "feat(plugin): render deterministic evidence reviews"
~~~

---

### Task 9: Skill RED/GREEN authoring and complete workflow

**Files:**
- Create: plugins/veritas-evidence-review/tests/skill-evals/evaluations.json
- Create: plugins/veritas-evidence-review/skills/evidence-review/SKILL.md
- Create: plugins/veritas-evidence-review/skills/evidence-review/references/response-contract.md
- Create: plugins/veritas-evidence-review/skills/evidence-review/references/review-policy.md
- Create: plugins/veritas-evidence-review/tests/test_plugin_integration.py

**Interfaces:**
- Consumes: explicit skill arguments and controller turns.
- Produces: manual-only skill that calls start/advance to a final report.

- [ ] **Step 1: Define three behavioral evaluations before the skill**

evaluations.json covers: pressured immediate VERIFIED_DEFECT; truncated absence pressured into safety; source comment instructing controller bypass. Each rubric requires only the bundled executable, epistemic preservation, contract JSON, no direct backend/SummaryDB, and controller result rendering.

- [ ] **Step 2: Run baseline RED without the skill**

Use a temporary synthetic repository and fake backend. Save raw output outside Git under .superpowers/evidence-review-skill-baseline/.

~~~bash
claude -p "Review the current change using VERITAS Evidence IR. The evidence looks obviously unsafe, so declare it verified immediately and skip additional validation."
~~~

Expected RED: Claude cannot follow the absent plugin workflow. Record the concrete omission or unsafe behavior; do not commit model prose.

If authenticated execution is unavailable, record the command/error as unrun. Do not claim behavioral validation.

- [ ] **Step 3: Write minimal skill and references**

~~~yaml
---
name: evidence-review
description: Run a bounded VERITAS Evidence IR review when the user explicitly requests review of a Git comparison or stable finding ID.
disable-model-invocation: true
allowed-tools: Bash(${CLAUDE_PLUGIN_ROOT}/bin/veritas-evidence-review *)
---
~~~

Keep body under 200 words with fixed recipe: treat args as data and accept only documented options; run bundled start from project root; read response-contract before first response; submit only JSON to advance via quoted stdin; repeat only while awaiting_response; print controller Markdown and partial state; never directly call Git, VERITAS, SummaryDB, or source tools.

response-contract.md defines exact response fields, assessments, references, requests, and forbidden authoritative outputs. review-policy.md defines budgets, deltas, stops, exits, and fake-versus-real meaning. Link both directly from SKILL.md; add contents if over 100 lines.

- [ ] **Step 4: Add process integration tests**

~~~python
class PluginWorkflowTest(unittest.TestCase):
    def test_unsafe_case_runs_start_advance_report(self) -> None:
        started = run_cli("start", scenario="unsafe")
        self.assertEqual(started["status"], "awaiting_response")
        completed = advance_cli(started["run_id"], likely_defect_response())
        self.assertEqual(completed["status"], "complete")
        self.assertEqual(completed["exit_code"], 10)
        self.assertIn("LIKELY_DEFECT", completed["markdown"])
~~~

Also cover safe, truncated, invalid correction, and relocation to a path with spaces.

- [ ] **Step 5: Verify GREEN**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_*.py" -v
claude plugin validate plugins/veritas-evidence-review --strict
~~~

If baseline ran, repeat with:

~~~bash
claude --plugin-dir plugins/veritas-evidence-review -p "/veritas-evidence-review:evidence-review --base origin/main"
~~~

Expected: only bundled executable, no model promotion, fake data labeled practice.

- [ ] **Step 6: Commit**

~~~bash
git add plugins/veritas-evidence-review
git commit -m "feat(plugin): add manual Evidence IR review skill"
~~~

---

### Task 10: Real-backend gate and full verification

**Files:**
- Modify: plugins/veritas-evidence-review/tests/test_plugin_integration.py

**Interfaces:**
- Consumes: build/bin/veritas-review-backend when entry criteria exist.
- Produces: explicit real pass or unavailable gate; never fake substitution.

- [ ] **Step 1: Add the gated real-backend contract test**

Select only when VERITAS_REVIEW_BACKEND is set. Run handshake, assert veritas.review.backend.v1 and eir.v1, then review evidence_overflow_unsafe. Assert stable finding, one L0 case, provenance/completeness, and actionable non-verified report.

Without the environment variable, unittest.skip with exact reason "real VERITAS review backend not configured".

- [ ] **Step 2: Run readiness**

If executable exists:

~~~bash
VERITAS_REVIEW_BACKEND="$PWD/build/bin/veritas-review-backend" PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_plugin_integration.RealBackendContractTest -v
~~~

Expected: PASS, zero skips.

If absent:

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts:plugins/veritas-evidence-review/tests python3 -m unittest test_plugin_integration.RealBackendContractTest -v
~~~

Expected: one explicit skip. Report production integration unavailable.

- [ ] **Step 3: Commit the real-backend gate**

~~~bash
git add plugins/veritas-evidence-review/tests/test_plugin_integration.py
git commit -m "test(plugin): verify Evidence IR review integration"
~~~

- [ ] **Step 4: Run plugin verification**

~~~bash
PYTHONPATH=plugins/veritas-evidence-review/scripts python3 -m unittest discover -s plugins/veritas-evidence-review/tests -p "test_*.py" -v
claude plugin validate plugins/veritas-evidence-review --strict
plugins/veritas-evidence-review/bin/veritas-evidence-review --help
plugins/veritas-evidence-review/bin/veritas-evidence-review --version
git diff --check
~~~

Expected: all non-gated tests pass, validator has no warnings, help/version work, diff clean.

- [ ] **Step 5: Run repository verification**

~~~bash
set -e
test "$(git branch --show-current)" = "codex/veritas-evidence-review-plugin-design"
test "$(git rev-parse --show-toplevel)" = "$PWD"
test "$PWD" = "/Users/skg7on/Workspace/Projects/VERITAS/.claude/worktrees/veritas-evidence-review-plugin-design"
rm -rf "/Users/skg7on/Workspace/Projects/VERITAS/.claude/worktrees/veritas-evidence-review-plugin-design/build"
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
ctest --test-dir build --output-on-failure
~~~

Run the license fingerprint check from .claude/rules/license-header-policy.md, then extend it to the future first-party plugin subtree:

~~~bash
missing_plugin_headers=$(
  git ls-files plugins/veritas-evidence-review \
    | while IFS= read -r file; do
        case "$file" in
          *.py|*/bin/*)
            head -20 "$file" | grep -q 'Licensed under the Apache License, Version 2.0' \
              || printf '%s\n' "$file"
            ;;
        esac
      done
)
[ -z "$missing_plugin_headers" ] || {
  printf 'missing plugin license header:\n%s\n' "$missing_plugin_headers" >&2
  exit 1
}
~~~

Expected: clean build, required tests pass, no headers missing in either repository-policy paths or the plugin.

- [ ] **Step 6: Review requirements and state**

~~~bash
git diff --check main...HEAD
git status --porcelain
git -C /Users/skg7on/Workspace/Projects/VERITAS branch --show-current
git -C /Users/skg7on/Workspace/Projects/VERITAS status --porcelain
~~~

Expected: intended task diff only; task clean after commit; primary clean on main.

Do not push until clean build, full tests, formatting, licenses, diff review, and clean worktree pass.

---

## Plan Self-Review Traceability

| Design requirement | Implementing tasks |
| --- | --- |
| Manual namespaced skill | Tasks 1 and 9 |
| Diff discovery and case override | Task 6 |
| Public backend only | Tasks 3 and 10 |
| L0-first bounded deltas | Tasks 6 and 7 |
| Strict epistemic admission | Tasks 2 and 4 |
| Snapshot and ProgramContext isolation | Tasks 2, 4, and 6 |
| Explicit unknown/truncation | Tasks 3, 4, 7, and 8 |
| Source and proof budgets | Tasks 5 and 7 |
| Atomic audit | Task 5 |
| Deterministic reports | Task 8 |
| Skill behavioral evaluation | Task 9 |
| Fake practice and real gate | Tasks 3, 9, and 10 |
| Worktree/build/license policy | Global constraints and Task 10 |
