# RDX11 review evaluator

This is the execution and adjudication harness for qualification-plan Task 5.
It does **not** supply a model adapter or establish production qualification.
An operator must choose and authenticate the model/provider, approve the source
policy, implement the profile adapters against the shared review controller,
and collect independent judgments. The committed `rdx11-decision` record is
OPEN with zero attempts. No costs, P95 or replay outcomes have been invented.

## Pending decision

From the repository root:

```sh
python3 tools/review_eval/run.py tests/qualification/review/corpus.json \
  --output /tmp/rdx11-run.json
python3 tools/review_eval/report.py tests/qualification/review/corpus.json \
  /tmp/rdx11-run.json --output /tmp/rdx11-decision
```

The report command writes JSON/Markdown and currently returns **2** for OPEN.
Production qualification is unconditionally OPEN until the real release
validator exists; `evaluation_status` separately reports administrative
completeness. This administrative result never grants native/EIR authority.
Unavailable D/E retain the frozen corpus's reason. The corpus and rubric are
unchanged; this tool never tunes held-out cases.

## Operator execution contract

`run.py --config /absolute/operator.json --output /new/results/run.json`
executes trusted adapter argv arrays without a shell, one process session per
attempt. The output and wall limits come from the frozen corpus. Each attempt
pins request/response bytes, saves bounded stderr, and records elapsed time and
sampled simultaneous runner/adapter/descendant RSS. This is sampled RSS, not an
OS-enforced memory cap or a physical-footprint measurement. Missing sampling
is explicit and prevents qualification. Artifact directories and run bundles
must be new; prior results are never overwritten. Requests are nonce-bound to
case/profile/mode/run/sample and repeated artifact identities are rejected.
Cleanup handles the adapter session and separate compiler process groups.
Adapters that daemonize into new sessions are unsupported: sampling cannot
promise to catch fast orphan processes, so an enforcing containment wrapper is
still required for production deployment.

The operator JSON requires `model`, `provider`, `source_disclosure_policy`,
`prompt_policy_digest`, `context_policy_digest`, `compiler`, `build_type`,
`target`, `host`, and `adapter_revision`, plus:

- `adapters`: A/B/C maps to absolute executable argv arrays.
- `contexts`: each frozen case ID maps A/B/C to absolute context JSON paths.
  These are operator-prepared bounded contexts. A contains source; B adds the
  real structural index/summaries; C adds real local compiler evidence. No
  unqualified adapter or prewritten answer is a substitute for these profiles.

Adapters receive a single JSON request on stdin and write one JSON object on
stdout. Requests contain the bounded context, full revision pair, changed paths,
frozen budget, comparison configuration, profile/mode and an opaque per-attempt
nonce. They omit the case name, rubric, split, oracle and expected answer.
Fixed-candidate mode adds only its declared candidate/predicate; discovery mode
never receives it. The operator is responsible for excluding oracle/labels
from the context files and for providing captured, correctly bounded artifacts.

A production adapter must call the shared controller/backend and the selected
model, retain controller/native authority receipts, enforce profile capability
restrictions, scrub private paths before disclosure, and emit:

```json
{
  "origin": "PRODUCTION_CONTROLLER",
  "measurements": {
    "cold_setup_ms": 100, "cold_index_ms": 200,
    "provider_bootstrap_ms": 300, "model_ms": 400,
    "warm_query_ms": 50, "end_to_end_ms": 1050,
    "aggregate_peak_rss_bytes": 1000000,
    "physical_footprint_bytes": 1000000,
    "live_store_bytes": 1000, "physical_store_bytes": 4096,
    "model_turns": 1, "input_tokens": 100, "output_tokens": 50,
    "model_cost_usd": 0.001, "reviewer_ms": 100,
    "load_average": 1, "swap_bytes": 0
  }
}
```

Those numbers illustrate the wire shape only. They are not measurements.
Unknown/unavailable measurements must not be replaced with zeros. Phase costs
are disjoint; bootstrap and indexing cannot be hidden inside a warm query.
Native adapters must reuse the existing RunMetrics/M13/M14 instruments; none
is supplied here and D/E remain unavailable. Per-layer store measurement and
hard process isolation remain adapter responsibilities. The origin tag is an
adapter assertion, **not** an analytical authority certificate.

The default scheduler currently requests 100 attempts in each of three run
cohorts for every case/mode/profile (8100 attempts for this corpus). A smoke
run can use `--repeats 1 --runs 1`; it cannot qualify P95 or release. Operators
must approve their paid-run budget before execution. No paid API calls are
made by a pending run. Independently measured fresh deterministic builds and
production profile adapters still have to be supplied before the real release.

## Independent adjudication

`report.py --judgments /absolute/judgments.json` consumes an array of trusted
operator judgments. Each binds `attempt_id` and the exact `response_digest`,
names an independent `reviewer`, sets an outcome from the frozen rubric,
records `false_promotion: false`, `evidence_accurate: true`, and lists immutable
`evidence_refs` (`path`, SHA-256 `digest`). The operator must inspect actual
controller/producer receipts, captured inputs and source anchors; a model may
not generate this file or adjudicate itself.

For a supported local-bounds outcome, the independently checked `checks` must
include `exact_site_refuted`, `qualified_isolation`, `base_rejection`,
`boundary_n`, `short_source`, `wrong_site` and `unrelated_crash`. The corpus's
vulnerable revision is called `base`; `base_rejection` here means rejection of
the guarded/control implementation, not rejecting that vulnerable revision.
The callback observation requires `enqueue`, `cancel`, `dispatch`, `release`
and `semantic_status: PROPOSED` or `CONDITIONAL`. These names are the reviewer's
attestations; their truth must be established from the digest-pinned evidence.
No claim of universal safety or concurrent-schedule proof follows.

Fixed-candidate and whole-discovery precision/recall, misses, useful hypotheses,
abstention/unsupported rates, analytically deciding bounds replays (separate from supported semantic reports), per-run variability
and per-workload cost distributions remain separate in the JSON report.
Fewer than 100 samples yields a null P95. Failed controls, missing deciding
replay, missing adjudication, altered artifacts or missing costs keep the gate
OPEN. P4 investment stays OPEN pending a separate held-out quality/cost decision;
#142/#145 scaling thresholds are unchanged.

## Verification

```sh
python3 tools/review_eval/run_corpus_tests.py --pattern test_evaluation.py
ctest --test-dir build-homebrew-clang -R 'ReviewCorpus|ReviewEvaluation' \
  --output-on-failure
```

Process-tree tests need OS `ps` access. Synthetic subprocess responses exist
only in the tests, and are never published as qualification measurements.

## Operator selection (2026-11-01)

The user approved OpenAI `gpt-6.1-sol`, medium reasoning, disclosure limited to
the frozen public libuv source/tests/license and bounded derived evidence,
and a $5 smoke-run cap. The user subsequently requested that model runs be deferred while infrastructure
and CI are completed. No paid run has occurred. A direct API credential,
real controller adapters and independent adjudication remain required. The
existing ChatGPT-authenticated CLI was detected; it is not being substituted
for the controlled API transport. See the operator-selection artifact beside
the OPEN decision. This approval does not authorize the 8100-attempt default.
