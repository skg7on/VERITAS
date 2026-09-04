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

"""The documentation-consistency gate member (M9 criterion 10).

Asserts that the canonical architecture/milestone/README documents record the
same engine-ownership statements, that the generated Souffle provenance
manifest records the pinned source revision, and that the compiled production
targets exist.
"""

import json
import os
import pathlib
import sys
import unittest

SOURCE_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE_ROOT / "tools"))
import check_m9_entry as gate  # noqa: E402


def build_dir() -> pathlib.Path:
    return pathlib.Path(os.environ.get("VERITAS_BUILD_DIR", str(SOURCE_ROOT / "build")))


class M9DocumentationConsistencyTest(unittest.TestCase):
    def test_ownership_statements_consistent(self):
        for rel, phrases in gate.OWNERSHIP_DOCS.items():
            doc = SOURCE_ROOT / rel
            self.assertTrue(doc.exists(), f"documentation missing: {rel}")
            text = doc.read_text()
            for phrase in phrases:
                self.assertIn(phrase, text, f"{rel} missing ownership statement {phrase!r}")

    def test_provenance_records_pinned_revision(self):
        manifest = build_dir() / "souffle-provenance.json"
        self.assertTrue(manifest.exists(), "souffle provenance manifest missing")
        data = json.loads(manifest.read_text())
        self.assertEqual(data.get("source_revision"), gate.PINNED_SOUFFLE_REVISION)

    def test_compiled_production_targets_exist(self):
        bin_dir = build_dir() / "bin"
        self.assertTrue((bin_dir / "souffle").exists(), "souffle executable missing")
        self.assertTrue(
            (bin_dir / "veritas-souffle-worker").exists(),
            "veritas-souffle-worker missing",
        )


if __name__ == "__main__":
    unittest.main()
