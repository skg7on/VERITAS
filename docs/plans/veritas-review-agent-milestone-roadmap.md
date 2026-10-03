# VERITAS Review Agent Delivery Roadmap

**Status:** Proposed implementation direction; not an executable implementation plan
**Date:** 2026-10-03
**Design:** [Review-driven analysis](../specs/veritas-review-driven-analysis-design-spec.md)
**Architecture:** [Review Agent](../architecture/06-review-agent-architecture.md)

## 1. Sequencing rule

Prioritize a real, inspectable review loop and measure whether additional program
analysis improves it. Existing milestone numbers retain their delivery history.
The stages below are product gates, not new M-number assignments or claims that
M11–M19 are canceled. Each code change requires a reviewed focused plan.

## 2. Delivery stages

| Stage | Work | Existing tracking / missing tracker | Exit evidence |
| --- | --- | --- | --- |
| R0 — reconcile contracts | Repair native containment, completeness, allowed-epistemic checks, summary references and provenance-depth enforcement; document actual readiness | #149 owns reconciliation; focused child trackers required | Opaque validator reaches EIR; truncated/complete-empty cases preserve unknowns; forbidden authority is rejected; real summary refs and depth limits are exercised. |
| R1 — produce one real analytical case | Exact sink call-site/operand scoping and the needed range/capacity/alias/dominance producers | #132 workstreams 1–3; #124 | Real unsafe and guarded-safe fixtures distinguish their conditions; multiple callers/externals cannot contaminate a claim; unresolved alias/path stays explicit. |
| R2 — review with source and evidence | Host-neutral backend boundary, selected program membership, isolated controller, bounded context, dual candidate discovery, evidence requests and local reporting | Amend #79; #149 identifies untracked production backend work | Real base/working-snapshot review yields a supported analytical concern and a semantic contract observation; fake scenarios remain visibly practice. |
| R3 — evaluate added analysis | Freeze corpus/rubric/budgets; compare source-only, local evidence and deeper analysis; fixed-candidate and full-discovery runs | New review-evaluation tracker needed under design reconciliation | Independently adjudicated quality/cost results, misses and uncertainty; cold/warm costs and model-run spread reported. |
| R4 — add justified refinement | Capability negotiation, claim-specific alias/path/solver/replay operations with exact authority and coverage | New selective-analysis/verifier tracker needed; #124 remains owner of its producer scope | An obligation is checked by a configured real backend; unsupported/timeout results never promote; measured improvement on held-out changes. |
| R5 — make repeated review economical | Operational invalidation, cache integrity/retention, dependency-bound evidence lifecycle and source/model invalidation | #143/#144; new Evidence lifecycle tracker under #149; #125 before portable reuse | No-change/relevant/irrelevant edits distinguish work; CAS growth bounded; stale evidence rejected; whole-attempt cost includes frontend/SVF setup. |

R0 and R1 gate production analytical claims, not source-only evaluation or
practice-controller development. R2 may be developed against fake contracts in
parallel with the correctness work, but production qualification uses real
evidence and zero fake substitution. R3's corpus/rubric should be prepared before
R2 is tuned. R4 is conditional on R3's result. Small measured responsiveness
repairs from R5 may land earlier; cross-revision evidence reuse remains gated.

## 3. Immediate implementation slices

First amend the relevant designs and open focused trackers; do not start from
stale literal file/function examples in historical plans.

1. **Evidence integrity:** split #149's native correctness gaps into small
   deliveries with tests that discriminate missing containment, incomplete
   queries, forbidden epistemic values and inert provenance limits.
2. **Candidate scoping:** #132's independent work precedes its producer-gated
   automatic qualifying predicate. Keep existing single-seed compatibility;
   new candidate/report protocols use explicit version negotiation.
3. **Producer slice:** #124 supplies enough real facts for the first analytical
   review; keep the remaining mixed-path/alias oracles tracked until qualified.
4. **Real review API:** amend #79's specification and plan. Define bounded context
   artifact references, LLM candidate admission, report annotations and backend
   coverage. Bind interprocedural scope to explicit program/build membership;
   compilation output paths alone cannot certify library linkage. Preserve capability isolation and existing verification authority.
5. **Evaluation:** qualify the real loop and compare deeper analysis before
   funding general-purpose precision upgrades.

## 4. Placement of existing programmes

- **M0–M9:** reuse delivered identities, summaries, CPG, fact/provenance and
  engine boundaries. M7 infrastructure delivery does not resolve #143.
- **M10A–M10C:** delivered foundations with remaining producer/semantic
  correctness work under #124/#132/#149. Preserve their accepted tests.
- **M11 / #20:** useful acquisition/cache improvements can follow measured
  review costs. External bitcode support is not needed to review the first
  source-based C/C++ change. Split the approved scope before implementing a subset.
- **M12A–M12C / #21/#74/#75:** retain approved provider design; defer from first
  review path until a concrete provider improves a measured case.
- **M13:** delivered scale-profile/store-equivalence instrument; never reuse
  this number for pointer-analysis research.
- **M14 / #142:** retain regime and Release/Debug measurements. Run a bounded
  diagnostic scope; add review-cone experiments without silently changing its
  fixed acceptance thresholds.
- **M15–M19 / #140:** conditional full-analysis scaling programme. Hierarchical
  identity and incrementality remain useful, but full closure/representation
  redesign is justified by measurements. No automatic dependency from all of
  these milestones to the first review.
- **PTA research:** unnumbered future work. Stronger precision is funded by
  alias-dependent review failures and comparisons against existing SVF.

Post-overflow null dereference, use-after-free, unchecked-return and tainted-sink
production support is deferred pending the evaluation stage, with #149 retaining
roadmap reconciliation ownership. These families remain benchmark cases; being
in the evaluation corpus does not claim their producers/verifiers are delivered.

## 5. Status and issue management

The specification's issue table captures all 14 open issues read on 2026-10-03.
It is a proposed reprioritization, not a remote mutation. #149 remains the audit
umbrella until correctness gaps are implemented, assigned focused acceptance
owners, or explicitly deferred. New backend, evaluation, verifier, lifecycle and
link-unit/program-boundary trackers must be created before their implementation
work is assigned; #149 remains their reconciliation umbrella.

Issue #133 is closed; #140 remains the scaling tracker. Historical performance
thresholds are not silently revised or described as met. Existing closed
delivery issues retain their history; the open gap trackers govern unfinished
semantic acceptance.

## 6. Definition of a useful first release

A user invokes review for one pinned comparison/program/build. The agent reads
bounded context, proposes and admits a real concern, requests analytical evidence,
inspects counterevidence, and emits a locally inspectable report. The report names
unknowns and exact coverage. A genuine semantic observation can be reported
without fabrication of EIR facts. Any verified claim is backed by a configured
authority bound to its obligation.

Evaluation demonstrates the benefit and full cost of analysis beyond source
retrieval. Completion of infrastructure alone, a fake-backend transcript or a
green serializer test is insufficient release evidence.
