# Claim-Driven Redesign Program Implementation Plan

> **For agentic workers:** Read the relevant detailed spec and focused plan
> before implementation. Use superpowers:executing-plans inline, or
> superpowers:subagent-driven-development when explicitly selected. One focused
> delivery/issue per implementation branch; this document coordinates them.

**Goal:** Deliver a useful LLM-guided review product with bounded local evidence,
independently checked findings and selectively justified deeper analysis.
**Architecture:** Keep the full native analyzer as a provider and conformance
baseline. Add a separately identified local provider and one mediated controller;
then evolve query storage, invalidation and semantic contracts behind evidence gates.
**Tech Stack:** Existing C++20/Clang/LLVM/SVF/Soufflé/SQLite/RocksDB/Protobuf/Z3
plus Python 3.11+ standard-library host/controller and configured local replay.
**Specs:** [high-level redesign](../specs/veritas-claim-driven-redesign-design-spec.md),
[native integrity](../specs/veritas-native-evidence-integrity-design-spec.md),
[lightweight runtime](../specs/veritas-lightweight-review-runtime-design-spec.md),
[demand lifecycle](../specs/veritas-demand-analysis-lifecycle-design-spec.md),
[verification/qualification](../specs/veritas-review-verification-qualification-design-spec.md).

## Global constraints and scope

P1 must not wait for P2/P3 or all M11–M19 work. Native commands keep mandatory
SVF/production Soufflé; local results do not masquerade as native FactIDs/runs.
Keep EIR grammar/epistemic states, immutable identities, provenance and existing
native acceptance obligations. New contracts are proposed until reviewed, not
implemented merely because this plan exists. Native-specific integrity defects
gate the `native-eir` capability; local-only review can qualify independently
when snapshot/authority/local/replay gates pass and native EIR is disabled.
This capability-specific refinement does not weaken native M10B/M10C acceptance.

This delivery includes the real stable/dense authority and unresolved-call projection fixes with their tests. RDX02 now also supplies compiler-owned containment and exact callsite bindings, captured before SVF normalization.
All other tasks remain tracked work unless their implementation evidence says
otherwise. No undocumented delivery dates, model provider or public source
disclosure is assumed. Before external-model use, apply the operator's approved
source-disclosure policy.

Review focus: forced native identity in local mode; model-authored verification;
closed-world claims across open program boundaries; lost negative dependencies;
and measurements that hide setup or discovery misses. The focused plans name
the exact tests owning each failure mode.

## 1. Execution map

Execution umbrella: [#154](https://github.com/skg7on/VERITAS/issues/154).
RDX codes are stable work-package keys, not new milestone numbers. The following
issues are published with concrete acceptance criteria and dependency links.

| Key | Stage | Deliverable / focused tasks | Prerequisites |
| --- | --- | --- | --- |
| [RDX01 / #155](https://github.com/skg7on/VERITAS/issues/155) | P0 | Relation-authority admission; native plan Task 1 | Existing registry; runtime slice in this delivery |
| [RDX02 / #156](https://github.com/skg7on/VERITAS/issues/156) | P0 | Compiler ownership and exact native callsites; native Task 2 | RDX01; full automatic qualification still needs #124 |
| [RDX03 / #157](https://github.com/skg7on/VERITAS/issues/157) | P0 | Semantic coverage and complete-empty handling; native Task 3 | RDX02 for owner-complete native queries |
| [RDX04 / #158](https://github.com/skg7on/VERITAS/issues/158) | P0 | Real provenance depth and summary handoff; native Tasks 4–6 | RDX03; native acceptance profile |
| [RDX05 / #159](https://github.com/skg7on/VERITAS/issues/159) | P0/P1 | v2 wire types, snapshot and program membership; runtime Tasks 1–2 | Reviewed protocol/membership amendment |
| [RDX06 / #160](https://github.com/skg7on/VERITAS/issues/160) | P1 | Structural/local compiler provider; runtime Task 3 | [RDX05 / #159](https://github.com/skg7on/VERITAS/issues/159) |
| [RDX07 / #161](https://github.com/skg7on/VERITAS/issues/161) | P1 | Public v2 backend plus verifier authority registry; runtime Task 4, verification Task 1 | RDX05/RDX06 |
| [RDX08 / #162](https://github.com/skg7on/VERITAS/issues/162) | P1 | Candidate/context/budget controller and report annotations; runtime Tasks 5–6, verification Task 3 P1 portion | RDX07, RDX10 frozen corpus before tuning; plugin host remains #79 |
| [RDX09 / #163](https://github.com/skg7on/VERITAS/issues/163) | P1 | Real sanitizer replay; verification Task 2 | RDX07; local evidence from RDX06 |
| [RDX10 / #164](https://github.com/skg7on/VERITAS/issues/164) | P0/P1 | Real-change corpus, controls and rubric; verification Task 4 | RDX05 manifest contract |
| [RDX11 / #165](https://github.com/skg7on/VERITAS/issues/165) | P1 gate | A–E evaluator and real release qualification; verification Task 5, runtime Task 7 | RDX08/RDX09/RDX10; native mode additionally RDX01–04 |
| [RDX12 / #166](https://github.com/skg7on/VERITAS/issues/166) | P2 or early perf | Indexed pinned reads; lifecycle Task 1 | Stable read-snapshot contract; independent of full storage migration |
| [RDX13 / #167](https://github.com/skg7on/VERITAS/issues/167) | P2 | Query scope, boundary summaries, seeded execution/proofs; lifecycle Tasks 2–3 | RDX05/RDX06; RDX11 decides investment; RDX12 for native reads |
| [RDX14 / #168](https://github.com/skg7on/VERITAS/issues/168) | P2 | Chunk/seal/current query publication; lifecycle Task 4 | RDX13 artifact contract |
| [RDX15 / #169](https://github.com/skg7on/VERITAS/issues/169) | P3 | Evidence dependencies, freshness and retention; lifecycle Tasks 5–6 | RDX14; coordinates #143/#144, #125 before portable reuse |
| [RDX16 / #170](https://github.com/skg7on/VERITAS/issues/170) | P4 | Persistent semantic contracts and selected formal adapters; verification Task 3 P4 portion | RDX11 measured benefit; RDX15 for persistent reuse |

Focused plans: [native](veritas-native-evidence-integrity-implementation-plan.md),
[runtime](veritas-lightweight-review-runtime-implementation-plan.md),
[lifecycle](veritas-demand-analysis-lifecycle-implementation-plan.md), and
[verification](veritas-review-verification-qualification-implementation-plan.md).

## 2. First executable delivery order

- [ ] Review the v2 binding/ID/program-membership and local-evidence contracts.
  Native authority validation is a correction to the existing registry contract,
  not a prerequisite to approving a new model transport.
- [ ] Complete RDX05 types/capture and freeze RDX10's real changes/rubric.
  Begin source-only corpus evaluation while native repairs continue.
- [ ] Deliver RDX06 local evidence and RDX07 public backend/authority boundary.
  An unsupported verifier can be advertised as absent during transport tests.
- [ ] Deliver RDX08 admission/controller and RDX09 real replay; #79 adapts the
  same controller rather than implementing another review state machine.
- [ ] Run RDX11: require an actual deciding bounds replay and base rejection,
  a cited release/callback observation, clean controls and discovery evaluation.
  Native-EIR integration is independently qualified after RDX01–04.
- [ ] Fund RDX13/RDX14 and RDX16 only from recorded evidence; RDX12 or narrowly
  measured no-op fixes may land early without silently changing #140's gates.

## 3. Existing-work disposition

| Existing owner | Disposition and preserved acceptance |
| --- | --- |
| #151 | Architectural decision/history; link merged #152/#153 and this program. New umbrella owns execution, not another copy of that decision |
| #149 | Native audit/reconciliation; RDX01–04 supply focused owners and evidence, without closing unrelated gaps |
| #124 | Full native range/capacity/alias/positive-check producers; local `copy_bound.v1` does not discharge its required native oracles |
| closed #132 | Preserve history; RDX02 owns unresolved implementation and exact-callsite work; no false inference from closure |
| #79 | Plugin packaging, host capability isolation and adapter tests; amend its v1-first plan for v2 discovery, one shared controller |
| #125 | Portable AnalysisTarget/identity; required before cross-host semantic reuse, not local-only P1 |
| #143 | Full native operational invalidation and identity policing; RDX15 coordinates without weakening M15/baseline requirements |
| #144 | Physical CAS growth and verified unchanged writes; RDX15 adds Evidence-rooted retention, not a competing CAS implementation |
| #140/#142/#145 | Keep measurement hypotheses and thresholds; demand-query growth is an additional experiment |
| #20/#21/#74/#75 | Retain approved M11/M12 provider/acquisition work; not collectively P1 dependencies; bring in when a measured case needs it |

## 4. Review and revision of current specifications/plans

| Current document family | Concrete revision in this delivery | Runtime status |
| --- | --- | --- |
| Architecture 06; review-driven spec and roadmap | Point to precise v2/local/native profiles, execution map and real qualification gates | Product target, not implemented |
| Plugin spec and 12-task plan | Preserve v1 baseline; define v2 initial context/candidate path, nullable native binding and task reuse/replacement map | New v2 implementation tasks pending |
| Link-unit spec | Replace output-path authority with explicit/build-link membership, OPEN fallback and separately versioned identity | Existing multiple-main guard retained |
| M10B/M10C specs and plans | Identify delivered baseline; route authority/coverage/ownership/provenance/summary repairs to the focused native plan | RDX01 authority fixes and RDX02 compiler/callsite repairs included; coverage/provenance/summary work pending |
| SummaryDB/WPA architecture and scaling roadmap | Separate native whole-run receipts from query manifests, review cache from native worklist, and P1 from scaling prerequisites | Schema-v5/query kernels pending |
| High-level claim-driven spec | Replace its future-plan handoff with these focused plans and capability-specific native integrity gate | Proposed detailed contracts |

Historical implementation tasks are not deleted or checked merely because a
foundation shipped. Follow amendment precedence; do not rerun old Create steps
over existing files or use stale line numbers as current code instructions.

## 5. Verification and completion evidence

Verification for this delivery (2026-10-24), runtime commit `35fedb2`:
Task 1 native tests were first observed RED (4/4 failing), then GREEN (9/9 in
AnalysisFactTest including exact existing identity goldens). The first full run exposed 29 upstream/fixture failures; the conservative
producer correction and admitted Alias counterevidence now pass 42 targeted
tests. Final clean build passed all 655 steps; full CTest passed 904/904 in
339.90 seconds with zero failures, errors or skips (JUnit/internal GoogleTest
skip inspection). The retained-source FactID goldens and native differential/CLI
suites pass; the one existing duplicate-library linker warning remains. The issue batch is
tracking, not execution of its unimplemented tasks.

Every implementation issue closes only on its named required/forbidden real
outcomes, reviewed diff, full task-appropriate tests and explicit remaining
coverage. P1 requires real corpus and replay evidence; P2 requires scoped
answer/coverage/proof comparison; representation-only baseline work uses M13's
exact equivalence instrument. Model-only claims and two empty equal stores
cannot satisfy these gates.

### RDX02 / #156 execution qualification (2026-10-24)

Runtime commit `3c636e2` implements native Task 2. The final clean Debug build
completed **656/656 steps**, zero errors and one existing duplicate-library
linker warning. Full CTest passed **908/908** in **340.16 s**, with zero failures,
errors or skips, including JUnit/internal GoogleTest skip inspection. Host:
Apple Clang 21 with LLVM/Clang 24 libraries and pinned SVF/Soufflé.

Eighteen projection tests and six query tests cover real native two-caller/two-
external binding, pre-SVF identities, exact selection/constant scope, foreign
source refusal, typed persistence/rollback/tamper, legacy coverage and rejected
ABI/scalable-memory forms. Valid native identity and differential suites remain
exact. The restored unsafe slice keeps five flow nodes, four edges and seventeen
GlobalFlow supports and now includes two actual caller-scoped unknowns. Explained
CPG, query, finding and EIR artifact changes are documented in the native spec.

Independent whole-branch review found three Important issues; each was observed
RED and fixed: foreign source substitution, new scalable-size assertion paths
and incompatible byval ABI classification. One Minor remains deferred: validate
ownership of optional `result_ref` before any result-consuming backend is
qualified. The native producer supplies the correct result; current consumers do
not use it. This does not close #124/#157/#158, enable automatic qualifying
fan-out, fix portable identities (#125), or qualify the entire reviewer.
