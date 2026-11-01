# Real-change review corpus (RDX10 / #164)

This directory freezes the first real-change review corpus and its
adjudication rubric. It is a qualification input, not a model benchmark, not a
production verifier and not evidence that the model detects the cases.

The release is `corpus.json`; the adjudication contract is `rubric.md`.
`corpus.json` stores the rubric digest, the split digest and a canonical
`release_digest`, so edits after freezing are refused until the release is
deliberately refrozen.

## Pinned real cases

| Family | Case | Base | Head | Change |
| --- | --- | --- | --- | --- |
| local-bounds | `lb-libuv-scoped-ipv6` | `e7b3c3f…` | `4fac942…` | Bounds the scope-id copy into the 16-byte `tmp` in `src/inet.c`. |
| release/callback | `rc-libuv-udp-send-after-close` | `e8458b2…` | `a7c44d6…` | Skips the queued UDP send path when a recv callback closed the handle. |
| release/callback | `rc-libuv-pollerr-after-close` | `e62ccbf…` | `b030b02…` | Requires an active handle before the UDP error-queue read after close. |

The periods in the table abbreviate the full 40-character revisions stored in
`corpus.json`. The repository is [libuv](https://github.com/libuv/libuv), MIT
licensed; every case pins the full base/head revisions, the source/test/license
artifact digests, the program target and translation-unit membership, the build
selection and the registered harness/payload digests. The local-bounds case
declares the unsafe destination as the base revision's
`char tmp[sizeof "255.255.255.255"]` (16 bytes) and the callsite
`memcpy(tmp, curtok, pch - curtok);`.

Controls:

- `lb-libuv-scoped-ipv6-guarded` cites the head's
  `UV__INET6_ADDRSTRLEN-1` guard.
- `rc-libuv-udp-send-after-close-guarded` cites the head's
  `uv__is_closing(handle)` guard.
- `rc-libuv-version-bump` is an unrelated release-control change.
- `lb-libuv-scoped-ipv6` freezes N (16), N+1 (17) and a short-source payload;
  only N+1 may be REFUTED. `rc-libuv-pollerr-after-close` is an independent
  sink execution for the release/callback family.
- The guarded controls and all real defects are held out. Only the unrelated
  version-bump control is in the tuning split, so tuning cannot see a held-out
  base/head diff, revision pair or change-path set.

`tests/fixtures/review/asan_replay/` remains the synthetic regression
companion. Those fixtures do not satisfy a real-family requirement.

## Validation

From the repository root:

```sh
python3 tools/review_eval/validate_corpus.py \
  tests/qualification/review/corpus.json --content-root "$PWD"
```

This checks the schema, freeze digests, split, budgets, run modes, family
requirements, oracle/control shape, boundary inputs, local harness/payload
digests and synthetic companion digests. It is deterministic and needs no
network.

When the pinned upstream git objects are available, also check revision
existence, base..head changed paths, artifact digests and every witness line:

```sh
git clone https://github.com/libuv/libuv /tmp/libuv-corpus
git -C /tmp/libuv-corpus fetch origin <full-base> <full-head>
python3 tools/review_eval/validate_corpus.py \
  tests/qualification/review/corpus.json --content-root "$PWD" \
  --repository-root /tmp/libuv-corpus
```

The unit tests are:

```sh
python3 -m unittest discover -s tools/review_eval/tests -p test_corpus.py -v
```

CTest runs the same suite through `run_corpus_tests.py` as
`ReviewCorpusValidatorTest`; the wrapper fails if discovery finds no tests.
`ReviewCorpusContentTest` runs the CLI content check.

## Replay status and the remaining gap

The local-bounds case registers a trusted harness
(`harnesses/libuv_inet_pton.cc`) and three payloads. The corpus freezes them;
it does **not** execute third-party code during CI. `oracle.replay.status` is
`NOT_RUN` and the reason is recorded in `corpus.json`. The harness accepts the
producer's `{program, payload_path, requested_bytes}` argv contract and
re-checks that the payload's scope-id prefix length equals the requested
length before calling `uv_inet_pton`, so a request cannot be attributed to a
different copy size.

The operator-qualified replay path is the existing `asan-replay.v1` /
`copy_bound.v1` producer. A qualified run must capture the pinned base
revision, build `src/inet.c` with the project's captured compile command,
compile the registered harness, run N (no finding) and N+1 (REFUTED), and
retain the deciding record. Missing compiler/runtime/harness/payload/isolation
is UNSUPPORTED, never a finding. A passing replay is never universal safety.

Until that run and the A–E measurement in #165 are recorded, the families stay
`production_qualification: "OPEN"`. This corpus is a prerequisite, not the
qualification itself.

## Refreezing

1. Edit the corpus, rubric, harness or payload deliberately.
2. Recompute the rubric digest, the split digest and the release digest:

   ```sh
   python3 tools/review_eval/validate_corpus.py \
     tests/qualification/review/corpus.json --print-digest
   ```

3. Put the printed `sha256:…` value in `release_digest`, and update
   `frozen.rubric_digest`/`frozen.split_digest` if those changed.
4. Re-run the structural and upstream checks. A stale digest is a hard
   failure, not a warning.

Licensing: the pinned libuv artifacts are MIT licensed (the license file and
digest are stored per case). VERITAS-authored corpus tooling and harnesses are
Apache-2.0.
