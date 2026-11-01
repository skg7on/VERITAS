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

"""Tests for the frozen real-change review corpus validator.

The fixture builds a temporary git repository so revision existence, diff
membership, artifact digests and witness lines are exercised against real git
objects, not against the validator's own assumptions.
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

VALIDATOR_DIR = Path(__file__).resolve().parents[1]
SOURCE_ROOT = Path(
    os.environ.get("VERITAS_SOURCE_DIR", str(Path(__file__).resolve().parents[3]))
)
sys.path.insert(0, str(VALIDATOR_DIR))

from validate_corpus import (  # noqa: E402
    CorpusError,
    canonical_digest,
    load_corpus,
    validate_corpus,
)

GIT_IDENTITY = (
    "-c",
    "user.name=Corpus Fixture",
    "-c",
    "user.email=corpus@example.invalid",
)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value: dict) -> str:
    payload = {key: item for key, item in value.items() if key != "release_digest"}
    encoded = json.dumps(
        payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return sha256_bytes(encoded)


def split_digest(split: dict) -> str:
    entries = [
        {"case_id": case_id, "index": "TUNING"} for case_id in split["tuning"]
    ]
    entries += [
        {"case_id": case_id, "index": "HELD_OUT"} for case_id in split["held_out"]
    ]
    entries.sort(key=lambda entry: entry["case_id"])
    encoded = json.dumps(
        entries, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return sha256_bytes(encoded)


def tagged(digest: str) -> str:
    return "sha256:" + digest


class Fixture:
    """A tiny git history with one commit per corpus case revision pair."""

    LOCAL_BASE = """#include <string.h>

int local_copy(const char *src, unsigned int len) {
  char buf[16];
  memcpy(buf, src, len);
  return buf[0];
}
"""

    LOCAL_HEAD = """#include <string.h>

int local_copy(const char *src, unsigned int len) {
  char buf[16];
  if (len > sizeof(buf))
    return -1;
  memcpy(buf, src, len);
  return buf[0];
}
"""

    CALLBACK_BASE = """#include <stddef.h>

struct handle {
  int active;
  int pollerr;
  void (*recv_cb)(void);
};

void release(struct handle *h) {
  h->active = 0;
}

void dispatch(struct handle *h) {
  if (h->active)
    h->recv_cb();

  if (h->pollerr)
    h->recv_cb();
}

void pollerr(struct handle *h) {
  if (h->pollerr)
    h->recv_cb();
}
"""

    CALLBACK_HEAD = """#include <stddef.h>

struct handle {
  int active;
  int pollerr;
  void (*recv_cb)(void);
};

void release(struct handle *h) {
  h->active = 0;
}

void dispatch(struct handle *h) {
  if (h->active)
    h->recv_cb();

  if (h->active && h->pollerr)
    h->recv_cb();
}

void pollerr(struct handle *h) {
  if (h->pollerr)
    h->recv_cb();
}
"""

    CALLBACK_SINK_HEAD = """#include <stddef.h>

struct handle {
  int active;
  int pollerr;
  void (*recv_cb)(void);
};

void release(struct handle *h) {
  h->active = 0;
}

void dispatch(struct handle *h) {
  if (h->active)
    h->recv_cb();

  if (h->active && h->pollerr)
    h->recv_cb();
}

void pollerr(struct handle *h) {
  if (h->active && h->pollerr)
    h->recv_cb();
}
"""

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self._git("init", "--quiet")
        self.commits: dict[str, str] = {}
        self.rubric_path = self.root / "rubric.md"
        self.rubric_bytes = (
            b"# Fixture rubric\n\n"
            b"Frozen before tuning. Known defects are the base revisions.\n"
        )
        self.rubric_path.write_bytes(self.rubric_bytes)
        self._build_history()

    def __enter__(self) -> "Fixture":
        return self

    def __exit__(self, *_exc) -> None:
        self._tmp.cleanup()

    def _git(self, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True,
            capture_output=True,
            text=True,
        ).stdout

    def _write(self, files: dict[str, bytes | str]) -> None:
        for relative, content in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, str):
                path.write_text(content, encoding="utf-8")
            else:
                path.write_bytes(content)

    def _commit(self, name: str, files: dict[str, bytes | str]) -> str:
        self._write(files)
        self._git("add", "--all")
        self._git(*GIT_IDENTITY, "commit", "--quiet", "-m", name)
        revision = self._git("rev-parse", "HEAD").strip()
        self.commits[name] = revision
        return revision

    def _build_history(self) -> None:
        self._commit(
            "base",
            {
                "LICENSE": "MIT License fixture\n",
                "harnesses/local.cc": "// trusted fixture harness\nint main(){return 0;}\n",
                "payloads/n.bin": "0123456789abcdef",
                "payloads/n_plus_1.bin": "0123456789abcdefg",
                "payloads/short.bin": "0123",
                "src/local.c": self.LOCAL_BASE,
                "src/callback.c": self.CALLBACK_BASE,
                "include/version.h": "VERSION 1\n",
            },
        )
        self._commit("local-fix", {"src/local.c": self.LOCAL_HEAD})
        self._commit("callback-fix", {"src/callback.c": self.CALLBACK_HEAD})
        self._commit("sink-fix", {"src/callback.c": self.CALLBACK_SINK_HEAD})
        self._commit("version-bump", {"include/version.h": "VERSION 2\n"})

    def revision(self, name: str) -> str:
        return self.commits[name]

    def show(self, revision: str, path: str) -> bytes:
        return subprocess.run(
            ["git", "-C", str(self.root), "show", f"{revision}:{path}"],
            check=True,
            capture_output=True,
        ).stdout

    def line_of(self, revision: str, path: str, token: str) -> int:
        for number, line in enumerate(
            self.show(revision, path).decode("utf-8").splitlines(), start=1
        ):
            if token in line:
                return number
        raise AssertionError(f"{token!r} not found in {path} at {revision}")

    def artifact(self, label: str, revision: str, path: str) -> dict:
        return {
            "path": path,
            "revision": label,
            "digest": sha256_bytes(self.show(revision, path)),
        }

    def local_digest(self, relative: str) -> str:
        return sha256_bytes((self.root / relative).read_bytes())

    def corpus(self) -> dict:
        base = self.revision("base")
        local_fix = self.revision("local-fix")
        callback_fix = self.revision("callback-fix")
        sink_fix = self.revision("sink-fix")
        version_bump = self.revision("version-bump")
        license_path = "LICENSE"

        local_case = {
            "case_id": "lb-fixture-local-copy",
            "family": "local-bounds",
            "kind": "REAL_CHANGE",
            "index": "HELD_OUT",
            "repository": {
                "url": "https://github.com/libuv/libuv",
                "license": {
                    "spdx": "MIT",
                    "path": license_path,
                    "digest": sha256_bytes(self.show(local_fix, license_path)),
                },
            },
            "base_revision": base,
            "head_revision": local_fix,
            "change": {
                "summary": "Bounds the local copy in local_copy.",
                "paths": ["src/local.c"],
            },
            "program": self.program("src/local.c"),
            "build": self.build(),
            "artifacts": [
                self.artifact("base", base, "src/local.c"),
                self.artifact("head", local_fix, "src/local.c"),
                self.artifact("head", local_fix, license_path),
            ],
            "oracle": {
                "kind": "KNOWN_DEFECT",
                "predicate": "copy_bound.v1",
                "defect_revision": "base",
                "callsite": {
                    "path": "src/local.c",
                    "line": self.line_of(base, "src/local.c", "memcpy(buf, src, len);"),
                    "contains": "memcpy(buf, src, len);",
                },
                "destination": {
                    "expression": "buf",
                    "declaration": "char buf[16];",
                    "declaration_line": self.line_of(
                        base, "src/local.c", "char buf[16];"
                    ),
                },
                "capacity_bytes": 16,
                "inputs": [
                    {
                        "label": "N",
                        "requested_bytes": 16,
                        "readable_bytes": 16,
                        "payload": "payloads/n.bin",
                        "payload_digest": self.local_digest("payloads/n.bin"),
                        "harness_id": "fixture-n",
                        "expected_status": "UNKNOWN",
                    },
                    {
                        "label": "N+1",
                        "requested_bytes": 17,
                        "readable_bytes": 17,
                        "payload": "payloads/n_plus_1.bin",
                        "payload_digest": self.local_digest("payloads/n_plus_1.bin"),
                        "harness_id": "fixture-n-plus-1",
                        "expected_status": "REFUTED",
                    },
                    {
                        "label": "SHORT_SOURCE",
                        "requested_bytes": 17,
                        "readable_bytes": 4,
                        "payload": "payloads/short.bin",
                        "payload_digest": self.local_digest("payloads/short.bin"),
                        "harness_id": "fixture-short",
                        "expected_status": "UNSUPPORTED",
                    },
                ],
                "harness": {
                    "source": "harnesses/local.cc",
                    "source_digest": self.local_digest("harnesses/local.cc"),
                    "entry_point": "local_copy",
                    "linkage": "C",
                },
                "replay": {
                    "revision": "base",
                    "isolation": "macos-sandbox",
                    "status": "NOT_RUN",
                    "reason": "fixture is not executed by the validator",
                },
                "expected_status": "REFUTED",
                "universal_safety_forbidden": True,
            },
        }

        local_control = copy.deepcopy(local_case)
        local_control.update(
            {
                "case_id": "lb-fixture-local-copy-guarded",
                "kind": "REAL_CONTROL",
                "index": "HELD_OUT",
                "oracle": {
                    "kind": "CLEAN",
                    "predicate": None,
                    "control_scope": "CORRECT_GUARD",
                    "expected_status": "NO_DEFECT",
                    "expectation": "the head rejects len > sizeof(buf) before the copy",
                    "evidence": [
                        {
                            "path": "src/local.c",
                            "revision": "head",
                            "line": self.line_of(
                                local_fix, "src/local.c", "if (len > sizeof(buf))"
                            ),
                            "contains": "if (len > sizeof(buf))",
                        },
                        {
                            "path": "src/local.c",
                            "revision": "head",
                            "line": self.line_of(
                                local_fix, "src/local.c", "memcpy(buf, src, len);"
                            ),
                            "contains": "memcpy(buf, src, len);",
                        },
                    ],
                },
            }
        )

        callback_case = {
            "case_id": "rc-fixture-dispatch-after-release",
            "family": "release-callback",
            "kind": "REAL_CHANGE",
            "index": "HELD_OUT",
            "repository": {
                "url": "https://github.com/libuv/libuv",
                "license": {
                    "spdx": "MIT",
                    "path": license_path,
                    "digest": sha256_bytes(self.show(callback_fix, license_path)),
                },
            },
            "base_revision": local_fix,
            "head_revision": callback_fix,
            "change": {
                "summary": "Guards the queued dispatch after a release.",
                "paths": ["src/callback.c"],
            },
            "program": self.program("src/callback.c"),
            "build": self.build(),
            "artifacts": [
                self.artifact("base", local_fix, "src/callback.c"),
                self.artifact("head", callback_fix, "src/callback.c"),
                self.artifact("head", callback_fix, license_path),
            ],
            "oracle": {
                "kind": "SEMANTIC_OBSERVATION",
                "predicate": "semantic-contract.v1",
                "expected_status": "PROPOSED",
                "concern": (
                    "A release in the first callback leaves the queued dispatch "
                    "operating on released state."
                ),
                "anchors": [
                    {
                        "role": "enqueue",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(local_fix, "src/callback.c", "if (h->active)"),
                        "contains": "if (h->active)",
                    },
                    {
                        "role": "cancel",
                        "path": "src/callback.c",
                        "revision": "head",
                        "line": self.line_of(
                            callback_fix, "src/callback.c", "h->active && h->pollerr"
                        ),
                        "contains": "h->active && h->pollerr",
                    },
                    {
                        "role": "dispatch",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(
                            local_fix, "src/callback.c", "h->recv_cb();"
                        ),
                        "contains": "h->recv_cb();",
                    },
                    {
                        "role": "release",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(
                            local_fix, "src/callback.c", "void release(struct handle"
                        ),
                        "contains": "void release(struct handle",
                    },
                ],
                "control_case": "rc-fixture-dispatch-after-release-guarded",
                "independent_sink": "rc-fixture-pollerr-after-release",
                "not_claimed": [
                    "universal-safety",
                    "concurrency-proof",
                    "native-fact-authority",
                ],
                "verification_ref": None,
            },
        }

        callback_control = copy.deepcopy(callback_case)
        callback_control.update(
            {
                "case_id": "rc-fixture-dispatch-after-release-guarded",
                "kind": "REAL_CONTROL",
                "index": "HELD_OUT",
                "oracle": {
                    "kind": "CLEAN",
                    "predicate": None,
                    "control_scope": "CORRECT_CANCELLATION",
                    "expected_status": "NO_CONCERN",
                    "expectation": "the head skips the queued dispatch while inactive",
                    "evidence": [
                        {
                            "path": "src/callback.c",
                            "revision": "head",
                            "line": self.line_of(
                                callback_fix, "src/callback.c", "h->active && h->pollerr"
                            ),
                            "contains": "h->active && h->pollerr",
                        }
                    ],
                },
            }
        )

        sink_case = copy.deepcopy(callback_case)
        sink_case.update(
            {
                "case_id": "rc-fixture-pollerr-after-release",
                "index": "HELD_OUT",
                "base_revision": callback_fix,
                "head_revision": sink_fix,
                "change": {
                    "summary": "Guards the error-queue dispatch while inactive.",
                    "paths": ["src/callback.c"],
                },
                "artifacts": [
                    self.artifact("base", callback_fix, "src/callback.c"),
                    self.artifact("head", sink_fix, "src/callback.c"),
                    self.artifact("head", sink_fix, license_path),
                ],
            }
        )
        sink_case["oracle"] = copy.deepcopy(callback_case["oracle"])
        sink_case["oracle"].update(
            {
                "concern": "The error-queue sink runs after a release.",
                "anchors": [
                    {
                        "role": "enqueue",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(
                            callback_fix, "src/callback.c", "void pollerr"
                        ),
                        "contains": "void pollerr",
                    },
                    {
                        "role": "cancel",
                        "path": "src/callback.c",
                        "revision": "head",
                        "line": self.line_of(
                            sink_fix, "src/callback.c", "h->active && h->pollerr"
                        ),
                        "contains": "h->active && h->pollerr",
                    },
                    {
                        "role": "dispatch",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(
                            callback_fix, "src/callback.c", "h->recv_cb();"
                        ),
                        "contains": "h->recv_cb();",
                    },
                    {
                        "role": "release",
                        "path": "src/callback.c",
                        "revision": "base",
                        "line": self.line_of(
                            callback_fix, "src/callback.c", "void release(struct handle"
                        ),
                        "contains": "void release(struct handle",
                    },
                ],
                "control_case": None,
                "independent_sink": None,
            }
        )

        version_control = {
            "case_id": "rc-fixture-version-bump",
            "family": "release-callback",
            "kind": "REAL_CONTROL",
            "index": "TUNING",
            "repository": {
                "url": "https://github.com/libuv/libuv",
                "license": {
                    "spdx": "MIT",
                    "path": license_path,
                    "digest": sha256_bytes(self.show(sink_fix, license_path)),
                },
            },
            "base_revision": sink_fix,
            "head_revision": version_bump,
            "change": {
                "summary": "Version bump with no callback or release source change.",
                "paths": ["include/version.h"],
            },
            "program": self.program("include/version.h"),
            "build": self.build(),
            "artifacts": [
                self.artifact("base", sink_fix, "include/version.h"),
                self.artifact("head", version_bump, "include/version.h"),
                self.artifact("head", version_bump, license_path),
            ],
            "oracle": {
                "kind": "CLEAN",
                "predicate": None,
                "control_scope": "UNRELATED_CHANGE",
                "expected_status": "NO_DEFECT",
                "expectation": "no source callback or release path changed",
                "evidence": [
                    {
                        "path": "include/version.h",
                        "revision": "head",
                        "line": self.line_of(
                            version_bump, "include/version.h", "VERSION 2"
                        ),
                        "contains": "VERSION 2",
                    }
                ],
            },
        }

        corpus = {
            "version": "veritas.review.eval.corpus.v1",
            "release": "fixture-2026-11-01",
            "release_digest": "",
            "frozen": {
                "date": "2026-11-01",
                "tuning_started": False,
                "rubric_path": "rubric.md",
                "rubric_digest": tagged(sha256_bytes(self.rubric_bytes)),
                "split_digest": "",
            },
            "split": {
                "strategy": "family-held-out",
                "tuning": ["rc-fixture-version-bump"],
                "held_out": [
                    "lb-fixture-local-copy",
                    "lb-fixture-local-copy-guarded",
                    "rc-fixture-dispatch-after-release",
                    "rc-fixture-dispatch-after-release-guarded",
                    "rc-fixture-pollerr-after-release",
                ],
                "held_out_policy": "held-out cases are never used for tuning",
            },
            "budgets": {
                "version": "veritas.review.eval.budgets.v1",
                "profiles": {
                    "A": {
                        "availability": "AVAILABLE",
                        "max_turns": 8,
                        "input_tokens": 32000,
                        "response_bytes": 65536,
                        "output_tokens": 4096,
                        "context_bytes": 262144,
                        "wall_ms": 120000,
                    },
                    "B": {
                        "availability": "AVAILABLE",
                        "max_turns": 8,
                        "input_tokens": 48000,
                        "response_bytes": 65536,
                        "output_tokens": 4096,
                        "context_bytes": 262144,
                        "wall_ms": 180000,
                    },
                    "C": {
                        "availability": "AVAILABLE",
                        "max_turns": 8,
                        "input_tokens": 64000,
                        "response_bytes": 65536,
                        "output_tokens": 4096,
                        "context_bytes": 262144,
                        "wall_ms": 240000,
                    },
                    "D": {
                        "availability": "UNAVAILABLE",
                        "reason": "formal/selected adapters are not qualified",
                    },
                    "E": {
                        "availability": "UNAVAILABLE",
                        "reason": "native Evidence mode is not part of P1",
                    },
                },
            },
            "runs": {
                "fixed_candidate": {
                    "candidate_source": "FROZEN",
                    "budget_profile": "A",
                    "case_ids": [
                        "lb-fixture-local-copy",
                        "rc-fixture-dispatch-after-release",
                        "rc-fixture-pollerr-after-release",
                    ],
                    "description": "verification is isolated from discovery",
                },
                "whole_discovery": {
                    "candidate_source": "MODEL_DISCOVERY",
                    "budget_profile": "B",
                    "case_ids": [
                        "lb-fixture-local-copy",
                        "rc-fixture-dispatch-after-release",
                        "rc-fixture-pollerr-after-release",
                    ],
                    "description": "discovery misses are measured separately",
                },
            },
            "families": [
                {
                    "id": "local-bounds",
                    "required_real_cases": 1,
                    "production_qualification": "OPEN",
                    "sensitive_paths": ["src/local.c"],
                    "cases": [local_case, local_control],
                },
                {
                    "id": "release-callback",
                    "required_real_cases": 1,
                    "production_qualification": "OPEN",
                    "sensitive_paths": ["src/callback.c"],
                    "cases": [callback_case, callback_control, sink_case, version_control],
                },
            ],
            "regression_companions": [
                {
                    "id": "fixture-regression-companion",
                    "role": "SYNTHETIC_REGRESSION",
                    "case_id": "lb-fixture-local-copy",
                    "paths": {
                        "harness": "harnesses/local.cc",
                        "payload": "payloads/n_plus_1.bin",
                    },
                    "digests": {
                        "harness": self.local_digest("harnesses/local.cc"),
                        "payload": self.local_digest("payloads/n_plus_1.bin"),
                    },
                }
            ],
        }
        corpus["frozen"]["split_digest"] = tagged(split_digest(corpus["split"]))
        corpus["release_digest"] = tagged(canonical(corpus))
        return corpus

    @staticmethod
    def program(path: str) -> dict:
        return {
            "version": "veritas.review.program.v1",
            "target_key": "fixture-target",
            "kind": "STATIC_LIBRARY",
            "membership_source": "EXPLICIT_DECLARATION",
            "coverage": "OPEN",
            "translation_units": [path],
            "targets": [
                {
                    "key": "fixture-target",
                    "kind": "STATIC_LIBRARY",
                    "translation_units": [path],
                    "dependencies": [],
                    "artifacts": [],
                }
            ],
            "frontier": ["fixture membership is limited to the reviewed unit"],
        }

    @staticmethod
    def build() -> dict:
        return {
            "system": "cmake",
            "commands": ["cmake -S . -B build", "cmake --build build"],
            "compile_commands": "build/compile_commands.json",
        }


class CorpusValidatorTest(unittest.TestCase):
    fixture: Fixture

    @classmethod
    def setUpClass(cls) -> None:
        cls.fixture = Fixture()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.fixture.__exit__()

    def corpus(self) -> dict:
        return copy.deepcopy(self.fixture.corpus())

    def seal(self, corpus: dict) -> dict:
        corpus["frozen"]["split_digest"] = tagged(split_digest(corpus["split"]))
        corpus["release_digest"] = tagged(canonical(corpus))
        return corpus

    def validate(self, corpus: dict, *, git: bool = True) -> None:
        validate_corpus(
            corpus,
            content_root=self.fixture.root,
            repository_root=self.fixture.root if git else None,
        )

    def assert_rejected(self, corpus: dict, pattern: str, *, git: bool = True) -> None:
        with self.assertRaisesRegex(CorpusError, pattern):
            self.validate(corpus, git=git)

    def test_accepts_frozen_fixture_corpus(self) -> None:
        self.validate(self.corpus())

    def test_canonical_digest_matches_independent_helper(self) -> None:
        corpus = self.corpus()
        self.assertEqual(canonical_digest(corpus), canonical(corpus))
        self.assertEqual(
            corpus["release_digest"], tagged(canonical(corpus))
        )

    def test_rejects_short_revision(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["base_revision"] = "abc1234"
        self.assert_rejected(corpus, "full 40-character")

    def test_rejects_nonexistent_revision(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["base_revision"] = "0" * 40
        self.assert_rejected(corpus, "not a commit")

    def test_rejects_artifact_digest_mismatch(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["artifacts"][0]["digest"] = "0" * 64
        self.assert_rejected(corpus, "artifact digest mismatch")

    def test_rejects_changed_path_not_in_diff(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["change"]["paths"].append(
            "src/not-changed.c"
        )
        self.assert_rejected(corpus, "not present in the base..head diff")

    def test_rejects_missing_program_membership(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["program"] = Fixture.program("src/callback.c")
        self.assert_rejected(corpus, "not a program translation unit")

    def test_rejects_program_units_outside_target_membership(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["program"]["targets"][0]["translation_units"] = ["src/callback.c"]
        self.assert_rejected(corpus, "not all in target")

    def test_rejects_family_without_real_case(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"] = [
            case
            for case in corpus["families"][0]["cases"]
            if case["kind"] != "REAL_CHANGE"
        ]
        corpus["split"]["held_out"].remove("lb-fixture-local-copy")
        for run in corpus["runs"].values():
            run["case_ids"] = [
                case_id
                for case_id in run["case_ids"]
                if case_id != "lb-fixture-local-copy"
            ]
        self.seal(corpus)
        self.assert_rejected(corpus, "no real known-defect case")

    def test_rejects_control_witness_that_does_not_show_its_guard(self) -> None:
        corpus = self.corpus()
        control = corpus["families"][0]["cases"][1]
        control["oracle"]["evidence"][0]["contains"] = "if (len <= sizeof(buf))"
        self.assert_rejected(corpus, "does not contain")

    def test_rejects_split_reusing_held_out_change(self) -> None:
        corpus = self.corpus()
        corpus["split"]["held_out"].remove("lb-fixture-local-copy-guarded")
        corpus["split"]["tuning"].append("lb-fixture-local-copy-guarded")
        for case in corpus["families"][0]["cases"]:
            if case["case_id"] == "lb-fixture-local-copy-guarded":
                case["index"] = "TUNING"
        self.seal(corpus)
        self.assert_rejected(corpus, "reuses the held-out change")

    def test_rejects_missing_clean_control(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"] = [
            case
            for case in corpus["families"][0]["cases"]
            if case["case_id"] != "lb-fixture-local-copy-guarded"
        ]
        corpus["split"]["held_out"].remove("lb-fixture-local-copy-guarded")
        self.seal(corpus)
        self.assert_rejected(corpus, "CORRECT_GUARD")

    def test_rejects_short_source_refuted(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        for item in case["oracle"]["inputs"]:
            if item["label"] == "SHORT_SOURCE":
                item["expected_status"] = "REFUTED"
        self.seal(corpus)
        self.assert_rejected(corpus, "SHORT_SOURCE")

    def test_rejects_missing_short_source(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["oracle"]["inputs"] = [
            item for item in case["oracle"]["inputs"] if item["label"] != "SHORT_SOURCE"
        ]
        self.seal(corpus)
        self.assert_rejected(corpus, "SHORT_SOURCE")

    def test_rejects_non_memcpy_callsite(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["oracle"]["callsite"]["contains"] = "return buf[0];"
        self.seal(corpus)
        self.assert_rejected(corpus, "memcpy")

    def test_rejects_independent_sink_not_a_real_change(self) -> None:
        corpus = self.corpus()
        callback = corpus["families"][1]["cases"][0]
        callback["oracle"]["independent_sink"] = (
            "rc-fixture-dispatch-after-release-guarded"
        )
        self.seal(corpus)
        self.assert_rejected(corpus, "independent_sink")

    def test_rejects_split_overlap(self) -> None:
        corpus = self.corpus()
        corpus["split"]["tuning"].append("lb-fixture-local-copy")
        self.seal(corpus)
        self.assert_rejected(corpus, "more than once")

    def test_rejects_split_digest_mismatch(self) -> None:
        corpus = self.corpus()
        corpus["frozen"]["split_digest"] = "sha256:" + "0" * 64
        self.assert_rejected(corpus, "split digest mismatch")

    def test_rejects_rubric_digest_mismatch(self) -> None:
        corpus = self.corpus()
        corpus["frozen"]["rubric_digest"] = "sha256:" + "0" * 64
        self.assert_rejected(corpus, "rubric digest mismatch")

    def test_rejects_missing_run_mode(self) -> None:
        corpus = self.corpus()
        del corpus["runs"]["fixed_candidate"]
        self.seal(corpus)
        self.assert_rejected(corpus, "fixed_candidate")

    def test_rejects_unavailable_profile_without_reason(self) -> None:
        corpus = self.corpus()
        corpus["budgets"]["profiles"]["D"]["reason"] = ""
        self.seal(corpus)
        self.assert_rejected(corpus, "requires a reason")

    def test_rejects_semantic_observation_claiming_validation(self) -> None:
        corpus = self.corpus()
        callback = corpus["families"][1]["cases"][0]
        callback["oracle"]["expected_status"] = "VALIDATED"
        self.seal(corpus)
        self.assert_rejected(corpus, "PROPOSED or CONDITIONAL")

    def test_rejects_release_digest_mismatch(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["change"]["summary"] += " (edited)"
        self.assert_rejected(corpus, "release digest mismatch")

    def test_rejects_missing_license_artifact(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["artifacts"] = [
            artifact
            for artifact in case["artifacts"]
            if artifact["path"] != "LICENSE"
        ]
        self.seal(corpus)
        self.assert_rejected(corpus, "license artifact")

    def test_rejects_requested_not_greater_than_capacity(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        for item in case["oracle"]["inputs"]:
            if item["label"] == "N+1":
                item["requested_bytes"] = 16
        self.seal(corpus)
        self.assert_rejected(corpus, "must exceed capacity_bytes")

    def test_rejects_missing_boundary_input(self) -> None:
        corpus = self.corpus()
        case = corpus["families"][0]["cases"][0]
        case["oracle"]["inputs"] = [
            item for item in case["oracle"]["inputs"] if item["label"] != "N"
        ]
        self.seal(corpus)
        self.assert_rejected(corpus, "boundary inputs")

    def test_rejects_local_file_digest_mismatch(self) -> None:
        corpus = self.corpus()
        corpus["families"][0]["cases"][0]["oracle"]["harness"][
            "source_digest"
        ] = "0" * 64
        self.assert_rejected(corpus, "local file digest mismatch")

    def test_rejects_duplicate_json_keys(self) -> None:
        path = self.fixture.root / "duplicate.json"
        path.write_text('{"version": "a", "version": "b"}', encoding="utf-8")
        with self.assertRaisesRegex(CorpusError, "duplicate"):
            load_corpus(path)

    def test_registered_harness_accepts_the_producer_argv_contract(self) -> None:
        harness = (
            SOURCE_ROOT
            / "tests/qualification/review/harnesses/libuv_inet_pton.cc"
        )
        source = harness.read_text(encoding="utf-8")
        self.assertIn("argc != 3", source)
        self.assertIn("argv[2]", source)
        self.assertIn("strtoul", source)
        self.assertIn("percent - colon - 1", source)

    def test_real_corpus_is_frozen_and_valid(self) -> None:
        path = SOURCE_ROOT / "tests/qualification/review/corpus.json"
        corpus = load_corpus(path)
        validate_corpus(corpus, corpus_path=path, content_root=SOURCE_ROOT)
        self.assertEqual(
            corpus["release_digest"], tagged(canonical_digest(corpus))
        )


if __name__ == "__main__":
    unittest.main()
