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

"""Unit tests for the executable M9 entry gate.

Exercises the pure JUnit-validation logic directly and the gate's build
rejections end-to-end, so the gate's own correctness is pinned independently of
the ten-criterion live build.
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE_ROOT / "tools"))
import check_m9_entry as gate  # noqa: E402

EXPECTED = {"SummaryV2BuilderTest", "SummaryVersionCompatibilityTest"}


def junit(names, skip=None, failure=None):
    root = ET.Element("testsuite")
    for name in names:
        case = ET.SubElement(root, "testcase", {"name": name, "status": "run"})
        if name == skip:
            ET.SubElement(case, "skipped")
        if name == failure:
            ET.SubElement(case, "failure")
    return root


class M9EntryGateTest(unittest.TestCase):
    def test_validate_report_accepts_exact_membership(self):
        report = junit(EXPECTED)
        self.assertEqual(gate.validate_report(report, EXPECTED, "summary-v2"), [])

    def test_validate_report_rejects_missing_member(self):
        report = junit({"SummaryV2BuilderTest"})
        errors = gate.validate_report(report, EXPECTED, "summary-v2")
        self.assertTrue(any("membership mismatch" in e for e in errors))

    def test_validate_report_rejects_extra_member(self):
        report = junit(EXPECTED | {"UnexpectedExtraTest"})
        errors = gate.validate_report(report, EXPECTED, "summary-v2")
        self.assertTrue(any("membership mismatch" in e for e in errors))

    def test_validate_report_rejects_skipped(self):
        report = junit(EXPECTED, skip="SummaryV2BuilderTest")
        errors = gate.validate_report(report, EXPECTED, "summary-v2")
        self.assertTrue(any("skipped or disabled" in e for e in errors))

    def test_validate_report_rejects_failure(self):
        report = junit(EXPECTED, failure="SummaryVersionCompatibilityTest")
        errors = gate.validate_report(report, EXPECTED, "summary-v2")
        self.assertTrue(any("failures or errors" in e for e in errors))

    def test_gate_rejects_cpp_emergency_build(self):
        build = pathlib.Path(tempfile.mkdtemp())
        (build / "CMakeCache.txt").write_text(
            "VERITAS_WPA_ENGINE:STRING=cpp-emergency\n"
        )
        result = subprocess.run(
            [sys.executable, str(SOURCE_ROOT / "tools" / "check_m9_entry.py"),
             "--build-dir", str(build)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("production engine is not souffle", result.stderr)

    def test_gate_rejects_missing_provenance(self):
        build = pathlib.Path(tempfile.mkdtemp())
        (build / "CMakeCache.txt").write_text("VERITAS_WPA_ENGINE:STRING=souffle\n")
        result = subprocess.run(
            [sys.executable, str(SOURCE_ROOT / "tools" / "check_m9_entry.py"),
             "--build-dir", str(build)],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("provenance manifest is missing", result.stderr)


if __name__ == "__main__":
    unittest.main()
