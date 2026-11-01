# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Validate a frozen real-change review corpus release.

The corpus is a qualification input, not a model benchmark result.  Validation
therefore refuses anything that could make the frozen claims unverifiable:

* short, non-commit or changed-path-free revision pairs;
* artifact or local file digest mismatches;
* a case whose callsite is outside its declared program membership;
* a first-release family without a real known-defect case;
* a negative control whose cited source witness does not contain its guard;
* split leakage, stale freeze digests, or a release edited after freezing;
* semantic observations promoted beyond PROPOSED/CONDITIONAL;
* a model-visible ``FIXED_CANDIDATE``/``WHOLE_DISCOVERY`` conflation.

``--repository-root`` additionally checks the pinned upstream git objects
(revision existence, base..head diff membership, artifact digests and witness
lines).  ``--content-root`` checks operator-owned local artifacts: the rubric,
registered harness sources, registered payload bytes and synthetic regression
companions.  CI runs the structural/local checks; content-only upstream checks
run where the pinned repository objects are available.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

SCHEMA_VERSION = "veritas.review.eval.corpus.v1"
PROGRAM_VERSION = "veritas.review.program.v1"
BUDGET_VERSION = "veritas.review.eval.budgets.v1"
REQUIRED_FAMILIES = ("local-bounds", "release-callback")
REQUIRED_PROFILES = ("A", "B", "C", "D", "E")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
SEMANTIC_ROLES = ("enqueue", "cancel", "dispatch", "release")


class CorpusError(Exception):
    """A corpus release violated its frozen contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CorpusError(message)


def _require_keys(value, required, label: str) -> None:
    _require(isinstance(value, dict), f"{label} must be an object")
    missing = [key for key in required if key not in value]
    _require(not missing, f"{label} is missing required keys: {missing}")


def _require_str(value, label: str) -> str:
    _require(
        isinstance(value, str) and value != "",
        f"{label} must be a non-empty string",
    )
    return value


def _require_int(value, label: str, minimum: int = 0) -> int:
    _require(
        type(value) is int and value >= minimum,
        f"{label} must be an integer >= {minimum}",
    )
    return value


def _require_list(value, label: str, *, nonempty: bool = True) -> list:
    _require(isinstance(value, list), f"{label} must be an array")
    if nonempty:
        _require(len(value) > 0, f"{label} must not be empty")
    return value


def _require_safe_path(value, label: str) -> str:
    _require_str(value, label)
    _require(not value.startswith("/"), f"{label} must be repository-relative")
    _require("\\" not in value, f"{label} must not contain backslashes")
    _require("\x00" not in value, f"{label} must not contain NUL")
    parts = value.split("/")
    _require(
        all(part not in ("", ".", "..") for part in parts),
        f"{label} must not contain empty, '.' or '..' components",
    )
    return value


def _digest(hex_digest: str) -> str:
    return "sha256:" + hex_digest


def _normalize_digest(value, label: str) -> str:
    _require_str(value, label)
    hex_digest = value[7:] if value.startswith("sha256:") else value
    _require(
        SHA256_RE.fullmatch(hex_digest) is not None,
        f"{label} must be a lowercase SHA-256 digest",
    )
    return hex_digest


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical_json(value) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def canonical_digest(corpus: dict) -> str:
    """SHA-256 of the corpus without its own ``release_digest`` field."""

    payload = {key: item for key, item in corpus.items() if key != "release_digest"}
    return _sha256(_canonical_json(payload))


def split_digest(split: dict) -> str:
    """SHA-256 of the ordered case-to-split assignment."""

    entries = [
        {"case_id": case_id, "index": "TUNING"} for case_id in split["tuning"]
    ]
    entries += [
        {"case_id": case_id, "index": "HELD_OUT"} for case_id in split["held_out"]
    ]
    entries.sort(key=lambda entry: entry["case_id"])
    return _sha256(_canonical_json(entries))


def load_corpus(path) -> dict:
    """Read a corpus with duplicate-key and non-finite-number rejection."""

    def pairs(entries):
        result = {}
        for key, value in entries:
            _require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def reject_constant(token):
        raise CorpusError(f"non-finite JSON number is not allowed: {token}")

    def reject_float(token):
        raise CorpusError(f"floating-point JSON number is not allowed: {token}")

    try:
        text = Path(path).read_text(encoding="utf-8")
        value = json.loads(
            text,
            object_pairs_hook=pairs,
            parse_constant=reject_constant,
            parse_float=reject_float,
        )
    except CorpusError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CorpusError(f"cannot read corpus {path}: {error}") from error
    _require(isinstance(value, dict), "corpus root must be a JSON object")
    return value


# ---------------------------------------------------------------------------
# Structural validation


def _validate_top_level(corpus: dict) -> None:
    _require_keys(
        corpus,
        (
            "version",
            "release",
            "release_digest",
            "frozen",
            "split",
            "budgets",
            "runs",
            "families",
            "regression_companions",
        ),
        "corpus",
    )
    _require(
        corpus["version"] == SCHEMA_VERSION,
        f"unsupported corpus version: {corpus['version']!r}",
    )
    _require_str(corpus["release"], "corpus.release")
    _normalize_digest(corpus["release_digest"], "corpus.release_digest")


def _validate_split(split: dict) -> None:
    _require_keys(
        split,
        ("strategy", "tuning", "held_out", "held_out_policy"),
        "corpus.split",
    )
    _require_str(split["strategy"], "corpus.split.strategy")
    _require_str(split["held_out_policy"], "corpus.split.held_out_policy")
    seen: dict[str, str] = {}
    for index in ("tuning", "held_out"):
        _require_list(split[index], f"corpus.split.{index}")
        for case_id in split[index]:
            _require_str(case_id, f"corpus.split.{index} case id")
            _require(
                case_id not in seen,
                f"split lists a case more than once: {case_id}",
            )
            seen[case_id] = "TUNING" if index == "tuning" else "HELD_OUT"


def _validate_frozen(corpus: dict, *, corpus_path, content_root) -> None:
    frozen = corpus["frozen"]
    _require_keys(
        frozen,
        ("date", "tuning_started", "rubric_path", "rubric_digest", "split_digest"),
        "corpus.frozen",
    )
    _require_str(frozen["date"], "corpus.frozen.date")
    _require(
        DATE_RE.fullmatch(frozen["date"]) is not None,
        "corpus.frozen.date must be YYYY-MM-DD",
    )
    try:
        datetime.date.fromisoformat(frozen["date"])
    except ValueError as error:
        raise CorpusError(f"corpus.frozen.date is not a date: {error}") from error
    _require(
        frozen["tuning_started"] is False,
        "a frozen release cannot already have started tuning",
    )
    rubric_path = _require_safe_path(
        frozen["rubric_path"], "corpus.frozen.rubric_path"
    )
    expected_split = split_digest(corpus["split"])
    actual_split = _normalize_digest(
        frozen["split_digest"], "corpus.frozen.split_digest"
    )
    _require(
        actual_split == expected_split,
        "split digest mismatch: the split changed after freezing",
    )
    expected_rubric = _normalize_digest(
        frozen["rubric_digest"], "corpus.frozen.rubric_digest"
    )
    rubric_base = Path(corpus_path).parent if corpus_path is not None else content_root
    if rubric_base is not None:
        rubric_file = Path(rubric_base) / rubric_path
        _require(
            rubric_file.is_file(),
            f"rubric file is missing: {rubric_file}",
        )
        _require(
            _sha256(rubric_file.read_bytes()) == expected_rubric,
            "rubric digest mismatch: the rubric changed after freezing",
        )


def _validate_budgets(budgets: dict) -> None:
    _require_keys(budgets, ("version", "profiles"), "corpus.budgets")
    _require(
        budgets["version"] == BUDGET_VERSION,
        f"unsupported budget version: {budgets['version']!r}",
    )
    profiles = budgets["profiles"]
    _require(isinstance(profiles, dict), "corpus.budgets.profiles must be an object")
    _require(
        set(profiles) == set(REQUIRED_PROFILES),
        f"budget profiles must be exactly {list(REQUIRED_PROFILES)}",
    )
    for name, profile in profiles.items():
        _require(
            isinstance(profile, dict),
            f"budget profile {name} must be an object",
        )
        availability = profile.get("availability")
        _require(
            availability in ("AVAILABLE", "UNAVAILABLE"),
            f"budget profile {name} has an unknown availability",
        )
        if availability == "UNAVAILABLE":
            _require(
                isinstance(profile.get("reason"), str) and profile["reason"] != "",
                f"unavailable budget profile {name} requires a reason",
            )
            continue
        for field in (
            "max_turns",
            "input_tokens",
            "response_bytes",
            "output_tokens",
            "context_bytes",
            "wall_ms",
        ):
            _require_int(profile.get(field), f"budget profile {name}.{field}", 1)


def _available_profiles(budgets: dict) -> set:
    return {
        name
        for name, profile in budgets["profiles"].items()
        if profile.get("availability") == "AVAILABLE"
    }


def _validate_runs(runs: dict, budgets: dict) -> None:
    _require_keys(runs, ("fixed_candidate", "whole_discovery"), "corpus.runs")
    available = _available_profiles(budgets)
    expected_sources = {
        "fixed_candidate": "FROZEN",
        "whole_discovery": "MODEL_DISCOVERY",
    }
    for name, expected_source in expected_sources.items():
        run = runs[name]
        _require_keys(
            run,
            ("candidate_source", "budget_profile", "case_ids", "description"),
            f"corpus.runs.{name}",
        )
        _require(
            run["candidate_source"] == expected_source,
            f"corpus.runs.{name}.candidate_source must be {expected_source}",
        )
        _require_str(run["description"], f"corpus.runs.{name}.description")
        profile = _require_str(
            run["budget_profile"], f"corpus.runs.{name}.budget_profile"
        )
        _require(
            profile in available,
            f"corpus.runs.{name} uses an unavailable budget profile: {profile}",
        )
        case_ids = _require_list(run["case_ids"], f"corpus.runs.{name}.case_ids")
        _require(
            len(set(case_ids)) == len(case_ids),
            f"corpus.runs.{name}.case_ids contains duplicates",
        )
        for case_id in case_ids:
            _require_str(case_id, f"corpus.runs.{name} case id")


# ---------------------------------------------------------------------------
# Case validation


def _validate_artifact(artifact: dict, label: str) -> tuple[str, str]:
    _require_keys(artifact, ("path", "revision", "digest"), label)
    path = _require_safe_path(artifact["path"], f"{label}.path")
    revision = artifact["revision"]
    _require(revision in ("base", "head"), f"{label}.revision must be base or head")
    _normalize_digest(artifact["digest"], f"{label}.digest")
    return revision, path


def _validate_program(program: dict, case: dict, case_id: str) -> None:
    _require_keys(
        program,
        (
            "version",
            "target_key",
            "kind",
            "membership_source",
            "coverage",
            "translation_units",
            "targets",
            "frontier",
        ),
        f"case {case_id}.program",
    )
    _require(
        program["version"] == PROGRAM_VERSION,
        f"case {case_id}.program.version is unsupported",
    )
    target_key = _require_str(program["target_key"], f"case {case_id}.program.target_key")
    _require(
        program["kind"]
        in ("EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY", "TEST"),
        f"case {case_id}.program.kind is unsupported",
    )
    _require(
        program["membership_source"]
        in ("BUILD_LINK_ARTIFACT", "EXPLICIT_DECLARATION", "OUTPUT_HINT"),
        f"case {case_id}.program.membership_source is unsupported",
    )
    _require(
        program["coverage"] in ("DECLARED_CLOSED", "OPEN", "UNRESOLVED"),
        f"case {case_id}.program.coverage is unsupported",
    )
    units = _require_list(
        program["translation_units"], f"case {case_id}.program.translation_units"
    )
    for unit in units:
        _require_safe_path(unit, f"case {case_id}.program.translation_units entry")
    targets = _require_list(program["targets"], f"case {case_id}.program.targets")
    found = False
    target_units_set: set[str] = set()
    for target in targets:
        _require_keys(
            target,
            ("key", "kind", "translation_units", "dependencies", "artifacts"),
            f"case {case_id}.program.target",
        )
        _require_str(target["key"], f"case {case_id}.program.target.key")
        if target["key"] != target_key:
            continue
        found = True
        target_units = _require_list(
            target["translation_units"],
            f"case {case_id}.program.target[{target_key}].translation_units",
        )
        for unit in target_units:
            _require_safe_path(
                unit, f"case {case_id}.program.target[{target_key}] translation unit"
            )
        target_units_set = set(target_units)
    _require(
        found,
        f"case {case_id}.program.targets does not contain target {target_key!r}",
    )
    _require(
        set(units) <= target_units_set,
        f"case {case_id}.program.translation_units are not all in target "
        f"{target_key!r} membership",
    )
    _require_list(program["frontier"], f"case {case_id}.program.frontier", nonempty=False)

    oracle = case["oracle"]
    required_paths: list[str] = []
    if oracle.get("kind") == "SEMANTIC_OBSERVATION":
        for anchor in oracle.get("anchors", []):
            if isinstance(anchor, dict) and isinstance(anchor.get("path"), str):
                required_paths.append(anchor["path"])
    elif oracle.get("kind") == "KNOWN_DEFECT":
        callsite = oracle.get("callsite", {})
        if isinstance(callsite, dict) and isinstance(callsite.get("path"), str):
            required_paths.append(callsite["path"])
    else:
        for evidence in oracle.get("evidence", []):
            if isinstance(evidence, dict) and isinstance(evidence.get("path"), str):
                required_paths.append(evidence["path"])
    for path in required_paths:
        _require(
            path in units,
            f"callsite path {path} is not a program translation unit for case {case_id}",
        )
        _require(
            path in target_units_set,
            f"callsite path {path} is not in program target {target_key!r} "
            f"membership for case {case_id}",
        )


def _validate_case(case: dict, family: dict, family_id: str) -> None:
    _require_keys(
        case,
        (
            "case_id",
            "family",
            "kind",
            "index",
            "repository",
            "base_revision",
            "head_revision",
            "change",
            "program",
            "build",
            "artifacts",
            "oracle",
        ),
        "case",
    )
    case_id = _require_str(case["case_id"], "case.case_id")
    _require(
        case["family"] == family_id,
        f"case {case_id} family does not match {family_id}",
    )
    _require(
        case["kind"] in ("REAL_CHANGE", "REAL_CONTROL"),
        f"case {case_id}.kind must be REAL_CHANGE or REAL_CONTROL",
    )
    _require(
        case["index"] in ("TUNING", "HELD_OUT"),
        f"case {case_id}.index must be TUNING or HELD_OUT",
    )

    repository = case["repository"]
    _require_keys(repository, ("url", "license"), f"case {case_id}.repository")
    url = _require_str(repository["url"], f"case {case_id}.repository.url")
    _require(
        url.startswith("https://") or url.startswith("http://"),
        f"case {case_id}.repository.url must be an HTTP(S) URL",
    )
    license_info = repository["license"]
    _require_keys(
        license_info,
        ("spdx", "path", "digest"),
        f"case {case_id}.repository.license",
    )
    _require_str(license_info["spdx"], f"case {case_id}.repository.license.spdx")
    license_path = _require_safe_path(
        license_info["path"], f"case {case_id}.repository.license.path"
    )
    _normalize_digest(
        license_info["digest"], f"case {case_id}.repository.license.digest"
    )

    for field in ("base_revision", "head_revision"):
        revision = _require_str(case[field], f"case {case_id}.{field}")
        _require(
            REVISION_RE.fullmatch(revision) is not None,
            f"case {case_id}.{field} must be a full 40-character lowercase hex revision",
        )
    _require(
        case["base_revision"] != case["head_revision"],
        f"case {case_id} base and head revisions must differ",
    )

    change = case["change"]
    _require_keys(change, ("summary", "paths"), f"case {case_id}.change")
    _require_str(change["summary"], f"case {case_id}.change.summary")
    change_paths = _require_list(change["paths"], f"case {case_id}.change.paths")
    _require(
        len(set(change_paths)) == len(change_paths),
        f"case {case_id}.change.paths contains duplicates",
    )
    for path in change_paths:
        _require_safe_path(path, f"case {case_id}.change.paths entry")

    _validate_program(case["program"], case, case_id)

    build = case["build"]
    _require_keys(build, ("system", "commands", "compile_commands"), f"case {case_id}.build")
    _require_str(build["system"], f"case {case_id}.build.system")
    commands = _require_list(build["commands"], f"case {case_id}.build.commands")
    for command in commands:
        _require_str(command, f"case {case_id}.build.commands entry")
    if build["compile_commands"] != "":
        _require_safe_path(
            build["compile_commands"], f"case {case_id}.build.compile_commands"
        )

    artifacts = _require_list(case["artifacts"], f"case {case_id}.artifacts")
    artifact_keys: set[tuple[str, str]] = set()
    artifact_by_key: dict[tuple[str, str], str] = {}
    for artifact in artifacts:
        revision, path = _validate_artifact(artifact, f"case {case_id}.artifact")
        key = (revision, path)
        _require(
            key not in artifact_keys,
            f"case {case_id} has a duplicate artifact for {path} at {revision}",
        )
        artifact_keys.add(key)
        artifact_by_key[key] = _normalize_digest(
            artifact["digest"], f"case {case_id}.artifact.digest"
        )
    _require(
        ("head", license_path) in artifact_keys,
        f"license artifact is missing for case {case_id}",
    )
    _require(
        artifact_by_key[("head", license_path)]
        == _normalize_digest(
            license_info["digest"], f"case {case_id}.repository.license.digest"
        ),
        f"license digest does not match the head artifact for case {case_id}",
    )

    oracle = case["oracle"]
    _require(isinstance(oracle, dict), f"case {case_id}.oracle must be an object")
    kind = oracle.get("kind")
    _require(
        kind in ("KNOWN_DEFECT", "SEMANTIC_OBSERVATION", "CLEAN"),
        f"case {case_id}.oracle.kind is unsupported",
    )
    if case["kind"] == "REAL_CHANGE":
        _require(
            kind in ("KNOWN_DEFECT", "SEMANTIC_OBSERVATION"),
            f"real change case {case_id} must declare a known defect or semantic observation",
        )
    else:
        _require(
            kind == "CLEAN",
            f"real control case {case_id} must declare a clean oracle",
        )
    if kind == "KNOWN_DEFECT":
        _validate_known_defect(oracle, case, case_id, artifact_keys)
    elif kind == "SEMANTIC_OBSERVATION":
        _validate_semantic(oracle, case, case_id, artifact_keys)
    else:
        _validate_clean(oracle, case, family, case_id)


def _validate_known_defect(
    oracle: dict, case: dict, case_id: str, artifact_keys: set
) -> None:
    predicate = oracle.get("predicate")
    _require(
        predicate == "copy_bound.v1",
        f"known defect case {case_id} must use copy_bound.v1",
    )
    _require_keys(
        oracle,
        (
            "predicate",
            "defect_revision",
            "callsite",
            "destination",
            "capacity_bytes",
            "inputs",
            "harness",
            "replay",
            "expected_status",
            "universal_safety_forbidden",
        ),
        f"case {case_id}.oracle",
    )
    _require(
        oracle["defect_revision"] in ("base", "head"),
        f"case {case_id}.oracle.defect_revision must be base or head",
    )
    callsite = oracle["callsite"]
    _require_keys(callsite, ("path", "line", "contains"), f"case {case_id}.callsite")
    callsite_path = _require_safe_path(callsite["path"], f"case {case_id}.callsite.path")
    _require_int(callsite["line"], f"case {case_id}.callsite.line", 1)
    _require_str(callsite["contains"], f"case {case_id}.callsite.contains")
    destination = oracle["destination"]
    _require_keys(
        destination,
        ("expression", "declaration", "declaration_line"),
        f"case {case_id}.destination",
    )
    _require_str(destination["expression"], f"case {case_id}.destination.expression")
    _require_str(destination["declaration"], f"case {case_id}.destination.declaration")
    _require(
        "memcpy" in callsite["contains"],
        f"case {case_id}.callsite.contains must cite the copy_bound.v1 memcpy call",
    )
    _require(
        destination["expression"] in destination["declaration"],
        f"case {case_id}.destination.expression must appear in its declaration",
    )
    _require_int(
        destination["declaration_line"],
        f"case {case_id}.destination.declaration_line",
        1,
    )
    capacity = _require_int(
        oracle["capacity_bytes"], f"case {case_id}.oracle.capacity_bytes", 1
    )
    _require(
        ("base" if oracle["defect_revision"] == "base" else "head", callsite_path)
        in artifact_keys,
        f"callsite artifact is missing for case {case_id}",
    )

    inputs = _require_list(oracle["inputs"], f"case {case_id}.oracle.inputs")
    labels: dict[str, dict] = {}
    for item in inputs:
        _require_keys(
            item,
            (
                "label",
                "requested_bytes",
                "readable_bytes",
                "payload",
                "payload_digest",
                "harness_id",
                "expected_status",
            ),
            f"case {case_id}.input",
        )
        label = _require_str(item["label"], f"case {case_id}.input.label")
        _require(label not in labels, f"case {case_id} has duplicate input label {label}")
        labels[label] = item
        _require_int(item["requested_bytes"], f"case {case_id}.{label}.requested_bytes")
        _require_int(item["readable_bytes"], f"case {case_id}.{label}.readable_bytes")
        _require_safe_path(item["payload"], f"case {case_id}.{label}.payload")
        _normalize_digest(
            item["payload_digest"], f"case {case_id}.{label}.payload_digest"
        )
        _require_str(item["harness_id"], f"case {case_id}.{label}.harness_id")
        _require(
            item["expected_status"] in ("UNKNOWN", "REFUTED", "UNSUPPORTED"),
            f"case {case_id}.{label}.expected_status is unsupported",
        )
        if label == "SHORT_SOURCE":
            _require(
                item["requested_bytes"] > item["readable_bytes"],
                f"case {case_id}.SHORT_SOURCE.requested_bytes must exceed readable_bytes",
            )
            _require(
                item["expected_status"] in ("UNKNOWN", "UNSUPPORTED"),
                f"case {case_id}.SHORT_SOURCE must be non-deciding",
            )
        else:
            _require(
                item["requested_bytes"] <= item["readable_bytes"],
                f"case {case_id}.{label}.requested_bytes exceeds readable_bytes",
            )
            _require(
                item["expected_status"] != "REFUTED"
                or item["requested_bytes"] > capacity,
                f"case {case_id}.{label}.requested_bytes must exceed capacity_bytes",
            )
    _require(
        "N" in labels and "N+1" in labels,
        f"copy_bound oracle must declare boundary inputs N and N+1 for case {case_id}",
    )
    _require(
        "SHORT_SOURCE" in labels,
        f"copy_bound oracle must declare a SHORT_SOURCE control for case {case_id}",
    )
    _require(
        labels["N"]["requested_bytes"] == capacity,
        f"case {case_id}.N.requested_bytes must equal capacity_bytes",
    )
    _require(
        labels["N"]["expected_status"] != "REFUTED",
        f"case {case_id}.N must not expect a refutation",
    )
    _require(
        labels["N+1"]["requested_bytes"] == capacity + 1,
        f"case {case_id}.N+1.requested_bytes must equal capacity_bytes + 1",
    )
    _require(
        labels["N+1"]["expected_status"] == "REFUTED",
        f"case {case_id}.N+1 must expect REFUTED",
    )
    _require(
        oracle["expected_status"] == "REFUTED",
        f"case {case_id}.oracle.expected_status must be REFUTED",
    )
    _require(
        oracle["universal_safety_forbidden"] is True,
        f"case {case_id}.oracle.universal_safety_forbidden must be true",
    )

    harness = oracle["harness"]
    _require_keys(
        harness,
        ("source", "source_digest", "entry_point", "linkage"),
        f"case {case_id}.harness",
    )
    _require_safe_path(harness["source"], f"case {case_id}.harness.source")
    _normalize_digest(
        harness["source_digest"], f"case {case_id}.harness.source_digest"
    )
    _require_str(harness["entry_point"], f"case {case_id}.harness.entry_point")
    _require(
        harness["linkage"] in ("C", "CXX"),
        f"case {case_id}.harness.linkage must be C or CXX",
    )

    replay = oracle["replay"]
    _require_keys(
        replay,
        ("revision", "isolation", "status"),
        f"case {case_id}.replay",
    )
    _require(
        replay["revision"] in ("base", "head"),
        f"case {case_id}.replay.revision must be base or head",
    )
    _require_str(replay["isolation"], f"case {case_id}.replay.isolation")
    _require(
        replay["status"]
        in ("NOT_RUN", "BUILD_VERIFIED", "REPLAY_VERIFIED", "GAP"),
        f"case {case_id}.replay.status is unsupported",
    )
    if replay["status"] == "REPLAY_VERIFIED":
        _require_str(
            replay.get("verification_id", ""),
            f"case {case_id}.replay.verification_id",
        )
    elif replay["status"] in ("NOT_RUN", "GAP"):
        _require_str(replay.get("reason", ""), f"case {case_id}.replay.reason")


def _validate_semantic(
    oracle: dict, case: dict, case_id: str, artifact_keys: set
) -> None:
    _require_keys(
        oracle,
        (
            "predicate",
            "expected_status",
            "concern",
            "anchors",
            "not_claimed",
            "verification_ref",
        ),
        f"case {case_id}.oracle",
    )
    _require(
        oracle["predicate"] == "semantic-contract.v1",
        f"semantic case {case_id} must use semantic-contract.v1",
    )
    status = oracle["expected_status"]
    _require(
        status in ("PROPOSED", "CONDITIONAL"),
        f"semantic observation for case {case_id} must be PROPOSED or CONDITIONAL "
        "without an independent verification_ref",
    )
    _require(
        oracle["verification_ref"] is None,
        f"semantic observation for case {case_id} must not carry a verification_ref",
    )
    _require_str(oracle["concern"], f"case {case_id}.oracle.concern")
    not_claimed = _require_list(
        oracle["not_claimed"], f"case {case_id}.oracle.not_claimed"
    )
    _require(
        "universal-safety" in not_claimed,
        f"semantic observation for case {case_id} must explicitly not claim universal safety",
    )
    anchors = _require_list(oracle["anchors"], f"case {case_id}.oracle.anchors")
    roles: set[str] = set()
    for anchor in anchors:
        _require_keys(
            anchor,
            ("role", "path", "revision", "line", "contains"),
            f"case {case_id}.anchor",
        )
        role = _require_str(anchor["role"], f"case {case_id}.anchor.role")
        _require(
            role in SEMANTIC_ROLES,
            f"case {case_id}.anchor.role is unsupported: {role}",
        )
        _require(role not in roles, f"case {case_id} repeats anchor role {role}")
        roles.add(role)
        _require_safe_path(anchor["path"], f"case {case_id}.anchor.path")
        _require(
            anchor["revision"] in ("base", "head"),
            f"case {case_id}.anchor.revision must be base or head",
        )
        _require_int(anchor["line"], f"case {case_id}.anchor.line", 1)
        _require_str(anchor["contains"], f"case {case_id}.anchor.contains")
        _require(
            ("base" if anchor["revision"] == "base" else "head", anchor["path"])
            in artifact_keys,
            f"anchor artifact is missing for case {case_id}",
        )
    _require(
        roles == set(SEMANTIC_ROLES),
        f"case {case_id} semantic anchors must cover {list(SEMANTIC_ROLES)}",
    )


def _validate_clean(oracle: dict, case: dict, family: dict, case_id: str) -> None:
    _require_keys(
        oracle,
        ("predicate", "control_scope", "expected_status", "expectation", "evidence"),
        f"case {case_id}.oracle",
    )
    _require(
        oracle["predicate"] is None,
        f"clean case {case_id} must not declare an executable predicate",
    )
    scope = oracle["control_scope"]
    _require(
        scope
        in ("CORRECT_GUARD", "CORRECT_CANCELLATION", "UNRELATED_CHANGE", "BOUNDARY_SAFE"),
        f"case {case_id}.control_scope is unsupported",
    )
    _require(
        oracle["expected_status"] in ("NO_DEFECT", "NO_CONCERN"),
        f"clean case {case_id} must expect NO_DEFECT or NO_CONCERN",
    )
    _require_str(oracle["expectation"], f"case {case_id}.oracle.expectation")
    evidence = _require_list(oracle["evidence"], f"case {case_id}.oracle.evidence")
    for item in evidence:
        _require_keys(
            item,
            ("path", "revision", "line", "contains"),
            f"case {case_id}.evidence",
        )
        _require_safe_path(item["path"], f"case {case_id}.evidence.path")
        _require(
            item["revision"] in ("base", "head"),
            f"case {case_id}.evidence.revision must be base or head",
        )
        _require_int(item["line"], f"case {case_id}.evidence.line", 1)
        _require_str(item["contains"], f"case {case_id}.evidence.contains")
    if scope == "UNRELATED_CHANGE":
        sensitive = family.get("sensitive_paths", [])
        for path in case["change"]["paths"]:
            _require(
                not any(
                    path == sensitive_path or path.startswith(sensitive_path + "/")
                    for sensitive_path in sensitive
                ),
                f"unrelated control {case_id} touches sensitive path {path}",
            )


def _validate_families(corpus: dict) -> tuple[dict[str, dict], dict[str, str]]:
    families = _require_list(corpus["families"], "corpus.families")
    family_by_id: dict[str, dict] = {}
    case_by_id: dict[str, dict] = {}
    case_split: dict[str, str] = {}
    for family in families:
        _require_keys(
            family,
            (
                "id",
                "required_real_cases",
                "production_qualification",
                "sensitive_paths",
                "cases",
            ),
            "family",
        )
        family_id = _require_str(family["id"], "family.id")
        _require(
            family_id not in family_by_id,
            f"duplicate family id: {family_id}",
        )
        family_by_id[family_id] = family
        required = _require_int(
            family["required_real_cases"], f"family {family_id}.required_real_cases", 1
        )
        _require(
            family["production_qualification"] in ("OPEN", "QUALIFIED"),
            f"family {family_id}.production_qualification is unsupported",
        )
        sensitive = _require_list(
            family["sensitive_paths"], f"family {family_id}.sensitive_paths"
        )
        for path in sensitive:
            _require_safe_path(path, f"family {family_id}.sensitive_paths entry")
        cases = _require_list(family["cases"], f"family {family_id}.cases")
        real_defects: list[dict] = []
        held_out_defects: list[dict] = []
        for case in cases:
            _require(isinstance(case, dict), f"family {family_id} case must be an object")
            _validate_case(case, family, family_id)
            case_id = case["case_id"]
            _require(
                case_id not in case_by_id,
                f"duplicate case id: {case_id}",
            )
            case_by_id[case_id] = case
            case_split[case_id] = case["index"]
            if case["kind"] == "REAL_CHANGE" and case["oracle"]["kind"] in (
                "KNOWN_DEFECT",
                "SEMANTIC_OBSERVATION",
            ):
                real_defects.append(case)
                if case["index"] == "HELD_OUT":
                    held_out_defects.append(case)
        _require(
            len(real_defects) >= required,
            f"family {family_id} has no real known-defect case (required {required})",
        )
        _require(
            len(held_out_defects) >= 1,
            f"family {family_id} has no held-out real known-defect case",
        )
        clean_scopes = {
            case["oracle"]["control_scope"]
            for case in cases
            if case["oracle"]["kind"] == "CLEAN"
        }
        if family_id == "local-bounds":
            _require(
                "CORRECT_GUARD" in clean_scopes,
                f"family {family_id} requires a CORRECT_GUARD clean control",
            )
        if family_id == "release-callback":
            _require(
                "CORRECT_CANCELLATION" in clean_scopes,
                f"family {family_id} requires a CORRECT_CANCELLATION clean control",
            )
            _require(
                "UNRELATED_CHANGE" in clean_scopes,
                f"family {family_id} requires an UNRELATED_CHANGE clean control",
            )
        if family["production_qualification"] == "QUALIFIED":
            _require(
                len(real_defects) >= required,
                f"family {family_id} cannot be qualified without a real known-defect case",
            )
    for required_family in REQUIRED_FAMILIES:
        _require(
            required_family in family_by_id,
            f"corpus is missing the required first-release family {required_family}",
        )
    return case_by_id, case_split


def _validate_split_disjointness(
    split: dict, case_by_id: dict[str, dict]
) -> None:
    def signature(case: dict) -> tuple:
        return (
            case["base_revision"],
            case["head_revision"],
            tuple(sorted(case["change"]["paths"])),
        )

    tuning = {signature(case_by_id[case_id]) for case_id in split["tuning"]}
    for case_id in split["held_out"]:
        _require(
            signature(case_by_id[case_id]) not in tuning,
            f"tuning split reuses the held-out change for {case_id}",
        )


def _validate_case_references(case_by_id: dict[str, dict]) -> None:
    for case_id, case in case_by_id.items():
        oracle = case["oracle"]
        for field in ("control_case", "independent_sink"):
            reference = oracle.get(field)
            if reference is None:
                continue
            _require_str(reference, f"case {case_id}.oracle.{field}")
            _require(
                reference in case_by_id,
                f"case {case_id}.oracle.{field} references unknown case {reference}",
            )
            _require(
                reference != case_id,
                f"case {case_id}.oracle.{field} must reference a different case",
            )
            referenced = case_by_id[reference]
            if field == "control_case":
                expected_scope = (
                    "CORRECT_GUARD"
                    if case["family"] == "local-bounds"
                    else "CORRECT_CANCELLATION"
                )
                _require(
                    referenced["oracle"]["kind"] == "CLEAN"
                    and referenced["oracle"].get("control_scope") == expected_scope,
                    f"case {case_id}.oracle.control_case must reference a "
                    f"{expected_scope} clean control",
                )
            else:
                _require(
                    referenced["family"] == case["family"],
                    f"case {case_id}.oracle.independent_sink must be in the same family",
                )
                _require(
                    referenced["kind"] == "REAL_CHANGE"
                    and referenced["oracle"]["kind"]
                    in ("KNOWN_DEFECT", "SEMANTIC_OBSERVATION"),
                    f"case {case_id}.oracle.independent_sink must reference a real change",
                )
                _require(
                    (referenced["base_revision"], referenced["head_revision"])
                    != (case["base_revision"], case["head_revision"]),
                    f"case {case_id}.oracle.independent_sink must use a different change",
                )


def _validate_split_coverage(
    split: dict, case_by_id: dict[str, dict], case_split: dict[str, str]
) -> None:
    assigned = set(split["tuning"]) | set(split["held_out"])
    _require(
        assigned == set(case_by_id),
        "split must cover every corpus case exactly once",
    )
    for case_id, index in case_split.items():
        expected = "HELD_OUT" if case_id in split["held_out"] else "TUNING"
        _require(
            index == expected,
            f"case {case_id}.index does not match the frozen split",
        )


def _validate_run_coverage(runs: dict, case_by_id: dict[str, dict]) -> None:
    held_out_defects = {
        case_id
        for case_id, case in case_by_id.items()
        if case["kind"] == "REAL_CHANGE"
        and case["oracle"]["kind"] in ("KNOWN_DEFECT", "SEMANTIC_OBSERVATION")
        and case["index"] == "HELD_OUT"
    }
    for name in ("fixed_candidate", "whole_discovery"):
        referenced = set(runs[name]["case_ids"])
        unknown = referenced - set(case_by_id)
        _require(not unknown, f"corpus.runs.{name} references unknown cases: {sorted(unknown)}")
        missing = held_out_defects - referenced
        _require(
            not missing,
            f"corpus.runs.{name} omits held-out real defects: {sorted(missing)}",
        )


# ---------------------------------------------------------------------------
# Local content and upstream git checks


def _read_local(content_root, path: str) -> bytes:
    file = Path(content_root) / path
    _require(file.is_file(), f"local file is missing: {path}")
    return file.read_bytes()


def _validate_regression_companions(companions: list, content_root, case_by_id) -> None:
    _require(isinstance(companions, list), "corpus.regression_companions must be an array")
    seen: set[str] = set()
    for companion in companions:
        _require_keys(
            companion,
            ("id", "role", "case_id", "paths", "digests"),
            "regression companion",
        )
        companion_id = _require_str(companion["id"], "regression companion.id")
        _require(companion_id not in seen, f"duplicate regression companion id: {companion_id}")
        seen.add(companion_id)
        _require_str(companion["role"], f"regression companion {companion_id}.role")
        if companion["case_id"] is not None:
            _require_str(companion["case_id"], f"regression companion {companion_id}.case_id")
            _require(
                companion["case_id"] in case_by_id,
                f"regression companion {companion_id} references unknown case",
            )
        paths = companion["paths"]
        digests = companion["digests"]
        _require(isinstance(paths, dict), f"regression companion {companion_id}.paths")
        _require(isinstance(digests, dict), f"regression companion {companion_id}.digests")
        _require(
            set(paths) == set(digests),
            f"regression companion {companion_id} path/digest keys differ",
        )
        _require(
            len(paths) > 0,
            f"regression companion {companion_id} must declare at least one path",
        )
        for key, path in paths.items():
            _require_safe_path(path, f"regression companion {companion_id}.{key}")
            expected = _normalize_digest(
                digests[key], f"regression companion {companion_id}.{key}.digest"
            )
            if content_root is not None:
                actual = _sha256(_read_local(content_root, path))
                _require(
                    actual == expected,
                    f"local file digest mismatch for {path}",
                )


def _validate_local_content(corpus: dict, content_root, case_by_id) -> None:
    for case_id, case in case_by_id.items():
        oracle = case["oracle"]
        if oracle["kind"] == "KNOWN_DEFECT":
            harness = oracle["harness"]
            expected = _normalize_digest(
                harness["source_digest"], f"case {case_id}.harness.source_digest"
            )
            actual = _sha256(_read_local(content_root, harness["source"]))
            _require(
                actual == expected,
                f"local file digest mismatch for {harness['source']}",
            )
            for item in oracle["inputs"]:
                expected_payload = _normalize_digest(
                    item["payload_digest"], f"case {case_id}.{item['label']}.payload_digest"
                )
                payload = _read_local(content_root, item["payload"])
                _require(
                    _sha256(payload) == expected_payload,
                    f"local file digest mismatch for {item['payload']}",
                )
                _require(
                    item["readable_bytes"] <= len(payload),
                    f"case {case_id}.{item['label']}.readable_bytes exceeds the "
                    "registered payload",
                )


def _git(repository_root, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repository_root), *args],
        capture_output=True,
        text=True,
    )


def _git_commit(repository_root, revision: str, label: str) -> None:
    result = _git(repository_root, "cat-file", "-e", revision + "^{commit}")
    _require(
        result.returncode == 0,
        f"{label} revision is not a commit in the repository: {revision}",
    )


def _git_file(repository_root, revision: str, path: str) -> bytes:
    # Git blobs are raw bytes: do not decode or normalize newlines while
    # hashing an artifact.
    result = subprocess.run(
        ["git", "-C", str(repository_root), "show", f"{revision}:{path}"],
        capture_output=True,
    )
    _require(
        result.returncode == 0,
        f"cannot read {path} at {revision}",
    )
    return result.stdout


def _git_line(repository_root, revision: str, path: str, line: int, token: str, label: str) -> None:
    content = _git_file(repository_root, revision, path).decode("utf-8")
    lines = content.splitlines()
    _require(
        0 < line <= len(lines),
        f"{label} line {line} is outside {path} at {revision}",
    )
    _require(
        token in lines[line - 1],
        f"{label} witness {path}:{line} at {revision} does not contain {token!r}",
    )


def _validate_git(repository_root, case_by_id: dict[str, dict]) -> None:
    for revision in {
        case[field]
        for case in case_by_id.values()
        for field in ("base_revision", "head_revision")
    }:
        _git_commit(repository_root, revision, "case")
    for case_id, case in case_by_id.items():
        base = case["base_revision"]
        head = case["head_revision"]
        changed = set(
            _git(repository_root, "diff", "--name-only", base, head).stdout.splitlines()
        )
        for path in case["change"]["paths"]:
            _require(
                path in changed,
                f"case {case_id} changed path {path} is not present in the base..head diff",
            )
        for artifact in case["artifacts"]:
            revision = base if artifact["revision"] == "base" else head
            expected = _normalize_digest(artifact["digest"], f"case {case_id} artifact")
            actual = _sha256(_git_file(repository_root, revision, artifact["path"]))
            _require(
                actual == expected,
                f"artifact digest mismatch for {artifact['path']} at {artifact['revision']}",
            )
        oracle = case["oracle"]
        if oracle["kind"] == "KNOWN_DEFECT":
            revision = base if oracle["defect_revision"] == "base" else head
            callsite = oracle["callsite"]
            _git_line(
                repository_root,
                revision,
                callsite["path"],
                callsite["line"],
                callsite["contains"],
                f"case {case_id} callsite",
            )
            destination = oracle["destination"]
            _git_line(
                repository_root,
                revision,
                callsite["path"],
                destination["declaration_line"],
                destination["declaration"],
                f"case {case_id} destination",
            )
        elif oracle["kind"] == "SEMANTIC_OBSERVATION":
            for anchor in oracle["anchors"]:
                revision = base if anchor["revision"] == "base" else head
                _git_line(
                    repository_root,
                    revision,
                    anchor["path"],
                    anchor["line"],
                    anchor["contains"],
                    f"case {case_id} anchor {anchor['role']}",
                )
        else:
            for evidence in oracle["evidence"]:
                revision = base if evidence["revision"] == "base" else head
                _git_line(
                    repository_root,
                    revision,
                    evidence["path"],
                    evidence["line"],
                    evidence["contains"],
                    f"case {case_id} control witness",
                )


# ---------------------------------------------------------------------------
# Entry points


def validate_corpus(
    corpus: dict,
    *,
    corpus_path=None,
    content_root=None,
    repository_root=None,
) -> None:
    """Raise :class:`CorpusError` if the corpus violates its frozen contract."""

    _validate_top_level(corpus)
    _validate_split(corpus["split"])
    _validate_frozen(corpus, corpus_path=corpus_path, content_root=content_root)
    _validate_budgets(corpus["budgets"])
    _validate_runs(corpus["runs"], corpus["budgets"])
    case_by_id, case_split = _validate_families(corpus)
    _validate_case_references(case_by_id)
    _validate_split_coverage(corpus["split"], case_by_id, case_split)
    _validate_split_disjointness(corpus["split"], case_by_id)
    _validate_run_coverage(corpus["runs"], case_by_id)
    _validate_regression_companions(
        corpus["regression_companions"], content_root, case_by_id
    )
    if content_root is not None:
        _validate_local_content(corpus, content_root, case_by_id)
    if repository_root is not None:
        _validate_git(repository_root, case_by_id)
    expected = canonical_digest(corpus)
    actual = _normalize_digest(corpus["release_digest"], "corpus.release_digest")
    _require(
        actual == expected,
        "release digest mismatch: the corpus changed after it was frozen",
    )


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, help="corpus.json to validate")
    parser.add_argument(
        "--content-root",
        type=Path,
        default=None,
        help="root for operator-owned harnesses, payloads and regression fixtures",
    )
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=None,
        help="git checkout containing the pinned upstream revisions",
    )
    parser.add_argument(
        "--print-digest",
        action="store_true",
        help="print the canonical release digest and exit",
    )
    args = parser.parse_args(argv)

    try:
        corpus = load_corpus(args.corpus)
        if args.print_digest:
            print(_digest(canonical_digest(corpus)))
            return 0
        validate_corpus(
            corpus,
            corpus_path=args.corpus,
            content_root=args.content_root,
            repository_root=args.repository_root,
        )
    except CorpusError as error:
        print(f"corpus validation failed: {error}", file=sys.stderr)
        return 1
    print(f"corpus valid: {args.corpus}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
