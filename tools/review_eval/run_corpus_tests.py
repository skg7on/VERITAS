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

"""Run the corpus tests and fail when discovery finds nothing.

``python3 -m unittest discover`` exits zero when its pattern matches no tests.
CTest would then record a vacuous green for the validator's only regression
suite, so the gate uses this wrapper instead.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


def main() -> int:
    tests = Path(__file__).resolve().parent / "tests"
    suite = unittest.TestLoader().discover(str(tests), pattern="test_corpus.py")
    discovered = suite.countTestCases()
    if discovered == 0:
        print("no corpus tests discovered", file=sys.stderr)
        return 1
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
