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

"""Enforce exact membership of the `wpa-qualification` aggregate set.

CTest emits JUnit XML for the `wpa-qualification` label via
`ctest -L wpa-qualification --output-junit <file>`. This checker parses that
file and fails on any missing, extra, disabled, skipped, failed, errored, or
duplicate aggregate name. Individually discovered GoogleTest cases do not
substitute for aggregate membership, so the expected set is the five
executable-level test names.
"""

import argparse
import sys
import xml.etree.ElementTree as ET

EXPECTED = frozenset(
    (
        "WpaDifferentialQualificationTest",
        "WpaDeterminismQualificationTest",
        "WpaFailureQualificationTest",
        "WpaMigrationQualificationTest",
        "WpaPerformanceQualificationTest",
    )
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--junit", required=True, help="CTest JUnit XML output")
    args = parser.parse_args()

    try:
        root = ET.parse(args.junit).getroot()
    except (ET.ParseError, OSError) as exc:
        print(f"ERROR: cannot parse JUnit file: {exc}", file=sys.stderr)
        return 1

    names = []
    non_passing = []
    for testcase in root.iter("testcase"):
        name = testcase.get("name", "")
        names.append(name)
        if testcase.find("skipped") is not None:
            non_passing.append((name, "skipped"))
        if testcase.find("failure") is not None:
            non_passing.append((name, "failure"))
        if testcase.find("error") is not None:
            non_passing.append((name, "error"))

    observed = set(names)
    missing = sorted(EXPECTED - observed)
    extra = sorted(observed - EXPECTED)
    duplicates = sorted({name for name in names if names.count(name) > 1})

    errors = []
    if missing:
        errors.append(f"missing aggregate tests: {missing}")
    if extra:
        errors.append(f"extra aggregate tests: {extra}")
    if duplicates:
        errors.append(f"duplicate aggregate tests: {duplicates}")
    if non_passing:
        errors.append(f"non-passing aggregate tests: {non_passing}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"OK: exact wpa-qualification set verified ({len(EXPECTED)} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
