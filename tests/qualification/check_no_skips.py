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

"""Reject skips, disabled tests, and membership drift in CTest JUnit output.

Parses a CTest ``--output-junit`` XML report and fails (non-zero exit) on any
of:

* a ``<skipped>`` element (a test that reported SKIP),
* a test whose ``status`` is not ``run`` (a disabled or not-run test),
* a ``<failure>`` or ``<error>`` element,
* a duplicate test name,
* a test name missing from, or extra to, the exact expected set.

This is the machine-readable gate the M9 entry checker reuses: a green CTest
summary does not prove that every member of a label actually ran, because a
``GTEST_SKIP`` still reports as passed to CTest.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import List, Set


def parse_expected(raw: str) -> Set[str]:
    names: List[str] = [name.strip() for name in raw.split(",") if name.strip()]
    if not names:
        raise SystemExit("--expect must list at least one expected test name")
    seen: Set[str] = set()
    for name in names:
        if name in seen:
            raise SystemExit(f"duplicate expected test name: {name}")
        seen.add(name)
    return seen


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="CTest JUnit XML report path")
    parser.add_argument(
        "--expect",
        required=True,
        help="comma-separated exact test names expected in the report",
    )
    args = parser.parse_args(argv)

    if not args.report.exists():
        print(f"check_no_skips: report not found: {args.report}", file=sys.stderr)
        return 1

    expected = parse_expected(args.expect)
    errors: List[str] = []

    try:
        root = ET.parse(args.report).getroot()
    except ET.ParseError as exc:
        print(f"check_no_skips: failed to parse {args.report}: {exc}", file=sys.stderr)
        return 1

    if root.tag != "testsuite":
        errors.append(f"unexpected root element <{root.tag}> (expected <testsuite>)")

    names: List[str] = []
    for case in root.findall("testcase"):
        name = case.attrib.get("name")
        if name is None:
            errors.append("a <testcase> element is missing its 'name' attribute")
            continue
        names.append(name)

        status = case.attrib.get("status", "run")
        if status != "run":
            errors.append(f"test not run: {name} (status={status})")
        if case.find("skipped") is not None:
            errors.append(f"test skipped: {name}")
        if case.find("failure") is not None:
            errors.append(f"test failed: {name}")
        if case.find("error") is not None:
            errors.append(f"test errored: {name}")

    actual = set(names)
    duplicates = [name for name in names if names.count(name) > 1]
    if duplicates:
        errors.append(f"duplicate test names: {sorted(set(duplicates))}")

    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing:
        errors.append(f"missing expected tests: {missing}")
    if extra:
        errors.append(f"unexpected extra tests: {extra}")

    if errors:
        for message in errors:
            print(f"check_no_skips: {message}", file=sys.stderr)
        return 1

    print(f"check_no_skips: {len(names)}/{len(expected)} expected tests, no skips")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
