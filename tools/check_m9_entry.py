#!/usr/bin/env python3
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

"""The executable M9 entry gate.

Checks, in order, that:

1. the build was configured with ``VERITAS_WPA_ENGINE=souffle`` (compiled
   Souffle is the normal production recursive WPA executor);
2. the generated Souffle provenance manifest records the pinned source
   revision and the actual built executable digest;
3. every one of the ten M9 criterion CTest labels reports exactly its expected
   aggregate members, with no missing, extra, disabled, skipped, failed, or
   errored tests; and
4. the canonical documentation records the same engine-ownership statements.

Exits non-zero on any failure so it can gate the M9 handoff in CI and locally.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET
from typing import Dict, List, Set

PINNED_SOUFFLE_REVISION = "5682a9f12e2668ecdd26348fe63cc508bc0fcf47"

REQUIRED_CTEST_LABELS: Dict[int, str] = {
    1: "summary-v2",
    2: "indirect-calls",
    3: "stable-identity",
    4: "relations-v2",
    5: "souffle-production",
    6: "engine-conformance",
    7: "witness-closure",
    8: "failure-atomicity",
    9: "run-identity",
    10: "documentation-consistency",
}

EXPECTED_TESTS_BY_LABEL: Dict[str, Set[str]] = {
    "summary-v2": {"SummaryV2BuilderTest", "SummaryVersionCompatibilityTest"},
    "indirect-calls": {"SvfFactMapperTest", "CallGraphTest"},
    "stable-identity": {
        "StableValueMapperTest",
        "AbstractMemoryBuilderTest",
        "DenseIdMapTest",
    },
    "relations-v2": {
        "RelationSchemaTest",
        "WpaInputMaterializerTest",
        "WpaDeterminismQualificationTest",
    },
    "souffle-production": {
        "SouffleWpaExecutorTest",
        "ProjectAnalyzerWpaTest",
        "WpaPerformanceQualificationTest",
    },
    "engine-conformance": {
        "WpaExecutorConformanceTest",
        "WpaDifferentialQualificationTest",
    },
    "witness-closure": {"ResultCanonicalizerTest", "AnalysisFactBusTest"},
    "failure-atomicity": {"WpaFailureQualificationTest", "WpaOrchestratorTest"},
    "run-identity": {"AnalysisRunTest", "WpaMigrationQualificationTest"},
    "documentation-consistency": {"M9DocumentationConsistencyTest"},
}

# Canonical docs that must agree on engine ownership. Each maps a source file
# (repo-relative) to phrases that must all appear in it.
OWNERSHIP_DOCS: Dict[str, List[str]] = {
    "README.md": ["Souffl", "production"],
    "docs/architecture/01-platform-architecture.md": ["Souffl", "SVF"],
    "docs/architecture/02-whole-program-analysis-architecture.md": [
        "Souffl",
        "SVF",
        "conformance",
    ],
    "docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md": [
        "Souffl",
        "SVF",
    ],
    "docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md": [
        "AnalysisFactBatch"
    ],
}


def fail(message: str) -> None:
    raise SystemExit(f"check_m9_entry: {message}")


def check_cache(build_dir: pathlib.Path) -> None:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.exists():
        fail(f"CMakeCache.txt not found under {build_dir}")
    cache = cache_path.read_text()
    if "VERITAS_WPA_ENGINE:STRING=souffle" not in cache:
        fail("production engine is not souffle")


def check_provenance(build_dir: pathlib.Path) -> None:
    manifest_path = build_dir / "souffle-provenance.json"
    if not manifest_path.exists():
        fail("souffle provenance manifest is missing")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("source_revision") != PINNED_SOUFFLE_REVISION:
        fail("souffle provenance source revision mismatch")
    souffle_bin = build_dir / "bin" / "souffle"
    if not souffle_bin.exists():
        fail("souffle executable is missing")
    digest = hashlib.sha256(souffle_bin.read_bytes()).hexdigest()
    if manifest.get("executable_sha256") != digest:
        fail("souffle provenance executable digest mismatch")


def check_docs(build_dir: pathlib.Path) -> None:
    source_dir = build_dir.parent
    for rel, phrases in OWNERSHIP_DOCS.items():
        doc = source_dir / rel
        if not doc.exists():
            fail(f"documentation missing: {rel}")
        text = doc.read_text()
        for phrase in phrases:
            if phrase not in text:
                fail(f"{rel} is missing the ownership statement {phrase!r}")


def validate_report(
    report: ET.Element, expected: Set[str], label: str
) -> List[str]:
    """Returns a list of membership/skip/failure errors for a JUnit report.

    Pure: it never runs ctest, so the criteria logic is unit-testable against a
    hand-built report.
    """
    errors: List[str] = []
    cases = report.findall(".//testcase")
    names = {case.attrib["name"] for case in cases}
    if names != expected:
        errors.append(
            f"criterion {label} membership mismatch: got {sorted(names)}, "
            f"want {sorted(expected)}"
        )
    if any(case.find("skipped") is not None for case in cases):
        errors.append(f"criterion {label} contains skipped or disabled tests")
    if report.findall(".//failure") or report.findall(".//error"):
        errors.append(f"criterion {label} contains failures or errors")
    return errors


def run_criteria(build_dir: pathlib.Path) -> None:
    for number, label in REQUIRED_CTEST_LABELS.items():
        junit = build_dir / f"m9-entry-{number}.xml"
        completed = subprocess.run(
            [
                "ctest",
                "-L",
                label,
                "--no-tests=error",
                "--output-on-failure",
                "--output-junit",
                junit.name,
            ],
            cwd=build_dir,
            text=True,
            capture_output=True,
        )
        if completed.returncode != 0:
            fail(
                f"criterion {number} ({label}) failed:\n"
                f"{completed.stdout}\n{completed.stderr}"
            )
        if not junit.exists():
            fail(f"criterion {number} ({label}) produced no JUnit report")
        report = ET.parse(junit).getroot()
        errors = validate_report(report, EXPECTED_TESTS_BY_LABEL[label], label)
        if errors:
            fail(f"criterion {number} ({label}): " + "; ".join(errors))
        print(f"criterion {number}: {label} OK")


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir", type=pathlib.Path, required=True, help="VERITAS build directory"
    )
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    check_cache(build_dir)
    check_provenance(build_dir)
    check_docs(build_dir)
    run_criteria(build_dir)

    print("check_m9_entry: all ten criteria pass")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
