# Real-change review corpus adjudication rubric

**Frozen:** 2026-11-01, before any tuning run. The corpus release digest in
`corpus.json` includes this rubric's digest; edits require a new release.

This rubric governs adjudication of the real-change corpus in `corpus.json`.
It is an evaluation contract, not a claim that the model has detected a case.
A corpus release is a prerequisite for production qualification; it is not
proof of detection quality, and it never grants analytical authority.

## 1. Outcome categories

Every case/attempt receives exactly one outcome.

| Outcome | Meaning |
| --- | --- |
| `TRUE_POSITIVE_SUPPORTED` | The reported concern is correct for the admitted change and is supported by the case's declared evidence: a deciding `REFUTED` replay at the exact local-bounds callsite, or a scoped `PROPOSED`/`CONDITIONAL` release/callback observation whose four anchors are cited. |
| `TRUE_POSITIVE_UNPROVEN` | The concern is plausibly correct but remains unsupported, unknown or outside the declared scope. It is a useful hypothesis, not a deciding true positive. |
| `FALSE_POSITIVE` | The report claims a defect, verification or authority that the admitted evidence does not support, or claims universal safety, a concurrent schedule that was not observed, or a native fact from model text. |
| `TRUE_NEGATIVE` | A clean control produces no verified finding and no universal-safety claim. |
| `FALSE_NEGATIVE` | A held-out known defect or declared concern is missed in whole-discovery mode. |
| `ABSTENTION` | The report explicitly preserves UNKNOWN/INCONCLUSIVE. Counted separately; it is not a false positive and not a true positive. |
| `UNSUPPORTED` | The capability, provider, runtime or harness needed to decide is unavailable. It is not a negative and cannot be converted into one. |

Model confidence, model agreement or a deterministic-but-unauthorized response
does not change an outcome. Only an exact registered producer/capability and
its independently admitted witness can support `TRUE_POSITIVE_SUPPORTED`.

## 2. Local-bounds family

A local-bounds case is supported only when all of the following hold:

1. The reported callsite is the admitted `copy_bound.v1` callsite: exact
   captured path and line, and the case's declared destination extent.
2. The reported destination capacity equals the compiler-established local
   constant array extent; pointer, VLA and heap extents are not admitted.
3. The requested length exceeds that capacity and the source is readable for
   the claimed input. The N+1 input is the deciding input.
4. The result is a real sanitizer `REFUTED` at that exact callsite, under the
   operator-qualified isolation profile. A different site, a different
   snapshot, a short source, an unrelated crash, a timeout or a forged
   clean-exit transcript is `UNKNOWN`/`ERROR`/`UNSUPPORTED`, never a finding.
5. The N input (requested length exactly equal to capacity) produces no
   finding. A report that treats N as a defect is a false positive.
6. A passing replay never proves the bound for other inputs. `PROVED` at
   concrete input scope means "no defect observed on this input", never
   universal safety.

The clean `CORRECT_GUARD` control must cite the dominating guard that rejects
the overflow. Adjudication rejects a control whose cited witness does not
contain the guard (`control_scope = CORRECT_GUARD`).

## 3. Release/callback family

A release/callback case is a semantic observation, not a concurrency proof.

1. The report must cite all four anchors: enqueue, cancel, dispatch and
   release. Missing anchors leave the concern `TRUE_POSITIVE_UNPROVEN` at
   best.
2. The observation is scoped to the cited source change and schedule. It must
   remain `PROPOSED` or `CONDITIONAL` when there is no independent trace or
   formal schedule check. `VALIDATED`, `REFUTED` or a native `FactID` requires
   its own registered verifier and exact witness; the corpus does not provide
   one for these cases.
3. The report must not claim universal safety, an unbounded concurrency proof,
   or that a model-authored invariant is an authoritative program fact.
4. A clean `CORRECT_CANCELLATION` control must cite the guard or cancellation
   that prevents the queued operation from running after release. A control
   whose witness does not contain that guard is invalid.
5. An independent sink case must be adjudicated from its own anchors and
   change. Success on one callback path does not transfer to another sink.

## 4. Controls

The following controls are mandatory adjudication inputs:

| Control | Required adjudication |
| --- | --- |
| `CORRECT_GUARD` | No verified finding; the cited guard dominates the copy. |
| `CORRECT_CANCELLATION` | No verified concern; the cited guard/cancellation is present. |
| Boundary N | No finding at exactly capacity. |
| Boundary N+1 | A finding only with the exact admitted callsite and extent. |
| Short source | Non-deciding (`UNKNOWN`/`UNSUPPORTED`), not a finding. |
| Wrong site / different callsite | Non-deciding; never attributed to the case. |
| Unrelated sanitizer crash | Non-deciding; never a verified overflow. |
| Unrelated change | No callback/release or copy-bound source witness; no finding. |
| Independent sink | Adjudicated separately on its own anchors. |

A supposedly negative control that still violates its source contract is a
corpus defect, not a model success. The validator rejects a control whose
witness does not contain its declared guard or cancellation token.

## 5. Fixed-candidate versus whole-discovery

Both runs are frozen and reported separately.

- `fixed_candidate` starts from the frozen known candidate. It isolates
  verification and authority transitions. Success here does not demonstrate
  discovery.
- `whole_discovery` starts from model discovery over the bounded context. It
  measures missed candidates and false attributions. A fixed-candidate success
  never compensates for a discovery miss.

Both runs must include every held-out real defect. The fixed-candidate run may
not silently substitute a synthetic fixture for a real case.

## 6. Freeze and held-out discipline

1. The corpus was frozen before tuning: `frozen.tuning_started` is false and
   the release digest commits to the rubric, split, budgets, cases and
   artifacts.
2. Held-out cases are excluded from prompts, examples, candidate lists and
   tuning. Only the bounded source/context and the task framing are visible to
   the model.
3. The tuning split contains only the unrelated version-bump control. Every
   real defect and every guarded control is held out. A tuning case may not
   reuse a held-out revision pair or change-path set; the validator rejects
   such a split.
4. Model, prompts/context policy, total tokens, allowed time and benchmark are
   constant within a comparison. Available A/B/C budgets are frozen; D/E are
   explicitly unavailable rather than replaced by fabricated data.
5. Any corpus edit requires a new release and a new digest. A stale digest
   invalidates the release; the validator refuses it.
6. Hold-out labels and expected predicates are adjudication data. They are
   never offered to the model as evidence.

## 7. Metrics and zero false promotion

Report at least:

- adjudicated precision and recall, including missed critical defects;
- evidence accuracy (callsite, extent, anchors, provenance);
- unknown/abstention and unsupported rates;
- reviewer effort and useful hypotheses;
- deciding outcomes per family;
- cold setup, warm tool and end-to-end latency, with cold/warm separated;
- aggregate parent/child peak RSS, physical footprint and store bytes where
  the capability is available;
- model turns, tokens and cost, with repeated-run variability.

Zero false promotion is a release requirement: unsupported, open-frontier,
truncated, mismatched or model-authored authority must never become verified
state. No confidence threshold substitutes for independent checking. At least
one real analytical obligation must decide in P1; an all-inconclusive output
is insufficient.

## 8. Integrity and qualification result

The release pins full 40-character base/head revisions, file and license
digests, program membership, build selection and every registered harness and
payload digest. Short or nonexistent revisions, digest mismatches, a callsite
outside program membership, a fabricated commit identity or a missing real
family is a corpus failure. The validator also rejects a clean control whose
cited witness does not contain its guard, a split that leaks a case, and a
release edited after freezing.

Synthetic fixtures are regression companions only. They do not satisfy a real
family requirement.

Production qualification remains **OPEN** unless both first-release families
have real cases, the local-bounds replay is executed by an operator-qualified
producer, the release/callback observations are adjudicated from their
anchors, and the A–E evaluation records the measured results and misses.
This corpus alone is not that qualification.
