# VERITAS Review-Driven Analysis Design Specification

**Status:** Proposed refinement requested by the repository owner; implementation requires review
**Date:** 2026-10-03
**Scope:** Product goal, agent context/candidates, selective analysis, reporting,
evaluation and disposition of existing work
**Architecture:** [Review Agent](../architecture/06-review-agent-architecture.md)
**Roadmap:** [Review delivery direction](../plans/veritas-review-agent-milestone-roadmap.md)

## 1. Design brief

Build an LLM combined with static analysis code review agent. Use the LLM's
semantic understanding to interpret changes, infer candidate contracts and
select investigations. Use deterministic tools to establish structural and
program relationships, expose uncertainty, and check supported proof obligations.

The product goal is:

> VERITAS helps an LLM investigate code changes using bounded, provenance-backed
> program evidence, and applies stronger analysis when needed to support a
> review claim.

Assumptions for the first delivery: explicitly invoked local review; C/C++;
one selected program and build context; read-only reviewed source; local report;
no automatic fixes or remote comment publication. The design review does not
authorize implementation, change engine defaults or complete an existing issue.

## 2. Evidence reviewed and limits

The review covers architecture 01–05; the cross-cutting specifications; the
M1–M12 contracts and M13 acceptance record; backbone and scaling roadmaps;
agent/plugin designs; and all 14 open GitHub issues on 2026-10-03. Historical
performance and qualification documents supply the engine baseline. This is a
design/contract audit, not a fresh executable source audit or benchmark run.
Implementation gaps below are documented in PRs/issues, not independently
reproduced here.

Important evidence:

| Evidence | Consequence for direction |
| --- | --- |
| [M10B delivery #123](https://github.com/skg7on/VERITAS/pull/123), [M10C #126](https://github.com/skg7on/VERITAS/pull/126), hardening [#128](https://github.com/skg7on/VERITAS/pull/128) | Evidence representation exists, but missing producers, handoff and completeness gaps prevent treating delivery as complete semantic coverage. |
| [Scaling design #146](https://github.com/skg7on/VERITAS/pull/146) | Closure materialization and representation costs require separate experiments; the shallow LevelDB fixture does not establish the growth regime. |
| [M13 #148](https://github.com/skg7on/VERITAS/pull/148) | Store comparison and perturbation controls are useful compatibility instruments, not measures of review effectiveness. Rebuild-dependent identity differences must be accounted for explicitly. |
| [Latest optimization #150](https://github.com/skg7on/VERITAS/pull/150) | Reported Debug CPU 390.855 s / wall 391.463 s on the measured fixture; payload arena reverted; memory improvement not established. Earlier performance numbers are historical baselines. |
| [Audit #149](https://github.com/skg7on/VERITAS/issues/149) | Incrementality, containment, allowed epistemic states, provenance-depth enforcement, summary handoff, backend/verifier and lifecycle gaps need explicit ownership. |
| [Plugin #79](https://github.com/skg7on/VERITAS/issues/79) and its approved spec | Already permits non-verified likely dispositions and mediated source access. The refinement broadens candidate discovery and context, rather than inventing that authority separation. |

Issue #133 is closed as of this review; its historical limits are not claimed
achieved. #140 remains open. M13 now means scale profile/store equivalence;
older references assigning M13 to PTA research are obsolete milestone numbering.

## 3. Alternatives and decision

| Direction | Benefit | Limitation |
| --- | --- | --- |
| Comprehensive analysis before review | Rich reusable global facts and a known native engine contract | Delays product feedback and pays for relations a review may never need. |
| LLM review using source alone | Small initial system; broad semantic observations | Program relationships and executable paths are hard to substantiate; model confidence is insufficient evidence. |
| LLM investigation with selective deterministic analysis — recommended | Semantic breadth plus inspectable evidence; additional analysis is justified by an unresolved claim | Requires explicit capability/coverage contracts and evaluation of missed candidates. |

Keep the full native analyzer as a provider and baseline. Do not replace SVF,
rewrite Soufflé, or lower existing acceptance criteria to obtain a fast demo.
First prove whether stronger analysis improves actionable review on real changes.

## 4. Preserved contracts and scoped amendments

Preserve P1–P7, immutable summaries, stable typed identity, explicit uncertainty,
rooted provenance, pinned read snapshots, validated publication, native/provider
separation and fail-closed verification authority. P5 remains a target; #143
documents incomplete operational WPA incrementality.

Refine P8: model assertions remain `INFERRED`; supported review comments may be
reported without promotion to a verified program fact. Deterministic execution
alone does not establish truth: authority depends on the soundness/coverage of
the analysis, admitted assumptions and exact proposition checked. Concrete
replay establishes the observed execution, not a universal `MUST` proposition.

The proposal changes these product assumptions:

- The agent may read bounded source, diff, tests and documentation alongside EIR.
- Candidates may originate from the LLM or deterministic seeders.
- Review usefulness is evaluated before building every optional capability.
- Precision is selected for a claim, not maximized unconditionally.

It does not change `eir.v1`, EIR-T, relation registries, fact IDs, the mandatory
native SVF stage, production Soufflé ownership, or batch publication. Future
implementations need explicit schema/protocol version negotiation where needed.

## 5. Snapshot and context contract

A review attempt pins the base revision and working snapshot, including dirty
tracked content and the explicitly selected untracked files, program membership,
build variant/target, available analysis runs and their configuration/toolchain.
Relevant source outside the diff is bound to the same snapshot. Excluded or
unreadable files remain part of the coverage record rather than disappearing.

The controller provides bounded initial context: diff hunks, containing symbols,
nearby contracts and selected tests/docs. Subsequent reads use validated
snapshot-bound anchors and typed requests. Documents/tests that lack program
entity IDs use controller-issued artifact references with content digests;
they never impersonate EIR entities or permit arbitrary filesystem reads.

Retrieval records artifact identity, excerpt range, selection reason and omitted
frontier. Repository content is untrusted data. Preserve the plugin's capability
isolation, configured executables, process limits, output validation and tool
allowlist. Bounded source access does not grant ambient Bash/file/web tools.

Compilation database output paths can suggest build targets but do not establish
the linked program's complete library membership. Explicit build/link metadata
or operator selection is required for authoritative interprocedural scope.
Ambiguous membership yields a partial scope or refusal. The existing link-unit
proposal must address this before a scoped analysis claims whole-program coverage.

## 6. Candidate discovery and admission

Use two channels:

1. Deterministic seeds from changed calls, guards, flows and sensitive operations.
2. LLM concerns from the diff and bounded context, including API contracts,
   authorization/resource mismatches and state/lifetime conventions.

A candidate includes origin, snapshot-bound anchor(s), proposed claim kind,
supporting source/evidence references, assumptions and the next question.
Admission validates references, context and budgets, deduplicates overlapping
concerns and rejects invented program IDs. Candidate identity is an auditable
review identity; it is not an authoritative `FactID` or fabricated `EvidenceID`.

For analytical claims, resolve source anchors to exact program entities and a
call site before asking the Evidence Builder for a case. Failure to resolve is
explicitly inconclusive. A semantic contract observation may remain outside EIR
in the review report, with cited context and no manufactured program facts.

Ranking may use change relevance, plausible impact, contradictions and estimated
investigation cost. Record candidates denied or deferred by budgets. An LLM's
ranking cannot remove the only recall control: the evaluation corpus must include
known defects hidden behind apparently uninteresting changes.

## 7. Capability-driven investigation

These are proposed review capabilities, separate from existing alias tiers and
EIR-L0/L1/L2 presentation levels:

| Capability | Answers | Initial availability |
| --- | --- | --- |
| Context/structure | What changed; which symbol/call/check is involved? | Source retrieval and existing CPG where available; new review API pending. |
| Local effects | Def-use, branch/dominance, object extent and simple ranges | Existing extraction plus missing producers owned by #124. |
| Interprocedural effects | Parameter/return flow, external effects and callers | Existing native summaries/WPA through a selected analysis run. |
| Alias/call refinement | Does ambiguous memory/dispatch affect this claim? | Existing SVF base; narrower or deeper refinement requires separate qualification. |
| Verification/replay | Is the predicate proved/refuted under its declared domain? | Only configured backends; production dispatch is pending. |

Controller request/result contracts include scope, exact snapshot, requested
predicate, capability/version, model bundle, assumptions, coverage, completeness,
resource use, provenance and unsupported/error status. Results cannot be promoted
solely because they are returned by a deterministic tool.

Escalate when missing evidence could change a disposition. Read existing
counterevidence before issuing another expensive request. Stop at sufficient
support, contradiction, exhausted resources or unsupported theory. Analysis time,
memory, output and model tokens are bounded for the whole attempt as well as each
request; repeated cheap requests must not bypass the aggregate budget.

The first production slice may use a precomputed full native store. Its cold
analysis cost is reported. This proves integration, not a fast lightweight
review. A future structural/local provider must publish its narrower coverage
and different configuration identity; skipped SVF never means `NoAlias` or
resolved indirect calls. Deep analysis is not guaranteed cheap merely because
the agent requests it on demand.

## 8. LLM-proposed models and constraints

The LLM may propose source/sink labels, validator postconditions, ownership
conventions, lock protocols or state-transition contracts. Each proposal records
source/document references, exact predicate, version and assumptions.

These remain candidate models outside the authoritative production model bundle.
They can guide searches and hypothetical checks. If used as a premise, the
result is explicitly conditional and cannot promote the premise to `MUST`.
Unmodeled external functions retain unknown effects.

Admission as an authoritative model requires independently checked behavior or
explicit trusted model policy, versioned bundle updates and qualification tests
including counterexamples. Model agreement, naming, confidence and self-review
are not validation. The authoritative verifier must not assume the very
postcondition it is asked to prove.

## 9. Review presentation and authority

| Report category | Required support | EIR compatibility |
| --- | --- | --- |
| Verified finding | Configured authority bound to exact obligation, snapshot, dependencies and declared execution domain | Existing `VERIFIED_DEFECT`; replay establishes a concrete defect, not universal safety. |
| Supported concern | Valid cited evidence/source, explicit unresolved blocker and plausible impact | Existing non-verified likely/possible states when represented in EIR. |
| Semantic review observation | Source/test/doc references and stated contract or clearly labeled inferred intent | Report annotation; no new EIR state. |
| Inconclusive investigation | Exact missing fact, unsupported operation, contradiction or truncation | Existing `INCONCLUSIVE` where applicable. |

Confidence and severity do not change authority. A verified-safe result applies
only to its predicate, scope and assumptions; it never dismisses unrelated
semantic concerns. Missing dominating checks do not alone prove overflow; may
alias does not alone prove use-after-free; may-flow does not prove path feasibility.

Reports cite supporting and contradicting evidence, show assumptions and scope,
and state coverage at attempt level. Zero candidates or zero findings never
certifies safety. Canonical reporting is deterministic for identical admitted
inputs/responses, but real LLM runs and candidate sets need not be identical.

## 10. SummaryDB, caching and bounded queries

Reuse the existing store and public evidence/query boundary. Future scope-driven
queries should traverse compact effects and expand witnesses as needed instead
of requiring eager global closure publication. This is a separate semantics and
storage design, not a performance edit that preserves every old row.

No partial review result enters the whole-run `AnalysisFactBatch` as a complete
analysis. A separate bounded-result envelope requires versioned coverage and
publication contracts. Until implemented, the reviewer reads validated existing
analysis and holds occurrence-specific candidates separately.

Future evidence cache keys cover snapshot/build/program, analysis capability and
configuration, producer/model versions, predicate/scope, source/evidence inputs,
query budgets and completeness. Negative queries depend on the examined scope
and relevant additions, not just returned rows. Contract observations depend on
docs/tests as well as code. Unstable host/toolchain identities restrict reuse;
#125 must be resolved before portable cache claims. Cross-revision reuse requires
dependency revalidation; until then use exact-snapshot reuse only.

## 10.1 Worked review: modified code with user input and buffer sinks

Use this source-to-sink change as the first analytical agent example. It is a
proposed review/evaluation fixture, not an already implemented detector or a
verified result from this documentation change.

**Source contract:** a request reader provides user-controlled payload bytes and
parses a user-controlled length into `requested_len` in `[0, 4096]`. It records
`payload_bytes` as the actual readable payload extent. This contract is part of
the example's input model; a real review must inspect the reader or retain an
unknown. Input validity does not establish destination-buffer capacity.

The existing function uses a 32-byte local buffer and selects one sink per call:

```cpp
enum class SinkKind { Copy, Access, Write };

unsigned char handle_request(const unsigned char* payload,
                             std::size_t payload_bytes,
                             std::size_t requested_len,
                             SinkKind kind) {
    unsigned char buffer[32] = {};
    if (requested_len == 0 || requested_len > sizeof(buffer) ||
        requested_len > payload_bytes) {
        return 0;
    }

    switch (kind) {
    case SinkKind::Copy:
        std::memcpy(buffer, payload, requested_len);
        return buffer[0];
    case SinkKind::Access:
        return buffer[requested_len - 1];
    case SinkKind::Write:
        buffer[requested_len - 1] = payload[0];
        return buffer[0];
    }
    return 0;
}
```

The modification removes the destination bound while keeping the source bound:

```diff
-    if (requested_len == 0 || requested_len > sizeof(buffer) ||
-        requested_len > payload_bytes) {
+    if (requested_len == 0 || requested_len > payload_bytes) {
         return 0;
     }
```

The reader supplies valid storage for at least `payload_bytes` bytes, and
`payload_bytes` may exceed 32. Thus the remaining check establishes that the
source is long enough; it does not protect the local destination or indexed
access. `kind` selects independent executions: the access/write examples do not
rely on execution continuing after an earlier overflowing copy.

| Candidate | User-input flow | Sink condition to check | Potential defect |
| --- | --- | --- | --- |
| Copy | Payload bytes and `requested_len` reach the copy operands | `requested_len <= capacity(buffer)` | Destination buffer overflow in `memcpy`. |
| Access | `requested_len` controls `requested_len - 1` | `0 <= index < capacity(buffer)` | Out-of-bounds read; distinguish this from an overflowing write. |
| Write | `requested_len` controls the index; payload supplies the byte | `0 <= index < capacity(buffer)` | Out-of-bounds write. Here write means a memory store, not a `write()` syscall. |

The LLM connects the removed guard to all three uses and proposes separate
`INFERRED` candidates. User-controlled payload content and user-controlled
extent/index are different flows; the report must name the one that drives the
bounds violation. Arbitrary indexed accesses are proposed additional review
fixtures, not automatically supported by the current memcpy-specific resolver.

**Agent investigation:**

1. Read the modified hunk, enclosing function and bounded request-reader/test
   context. Propose that the change removes a buffer-capacity invariant; do not
   assume that user input alone proves a vulnerability.
2. Admit each candidate with the exact copy/access/store site, containing
   function, length/index and buffer entity. Another caller or external exit
   cannot supply this claim's operands (#132). The new access/write sink mapping
   requires its own registered predicate and capability qualification.
3. Request source-flow evidence, the 32-byte object extent, the post-guard length
   range and relevant dominating-check evidence. Read the unchanged source-bound
   guard as counterevidence to source over-read, not destination safety. Missing
   #124 producers remain explicit unknowns rather than invented facts.
4. Check the proposed counterexample: `requested_len = 33`, `payload_bytes = 33`
   with 33 readable payload bytes. The modified guard permits it; the copy would
   write 33 bytes into 32, and the indexed branches would use index 32. The base
   revision rejects the same request. This is a reasoned witness proposal until
   an authoritative backend checks or reproduces the corresponding execution.
5. For this direct local array, do not require whole-program Andersen analysis
   merely to know its declared capacity. Request alias/interprocedural refinement
   only if a real variant uses a pointer, wrapper, callback or mutation that
   makes the object's identity, capacity or reaching guard uncertain. A narrowed
   scope still reports unexamined boundary effects.
6. Ask a configured verifier/replay backend to check the exact candidate and
   snapshot. A concrete reproduction supports the observed defect, not universal
   safety; unsupported operations or missing facts produce a supported concern
   or inconclusive investigation with the precise blocker.

**Expected non-verified review comment:**

> Removing the destination-capacity check allows a valid 33-byte request through
> the remaining payload-length check. The local buffer has 32 bytes: the copy
> uses length 33, and the indexed branches use index 32. Restore a bound against
> the local buffer before these operations. Source inspection supports this
> concern; execution feasibility has not yet been verified by a configured backend.

A production report emits a comment only for sites present and relevant in the
selected change, cites their actual snapshot-bound anchors, and states any
unconfirmed reader contract. It does not copy the example's numbers into an
unrelated case or claim three independent findings when they share one root cause;
related sink candidates can be grouped into one finding with three affected sites.

**Evaluation controls:** base vs modified code; lengths 0, 31, 32 and 33; a short
payload rejected by the source bound; each sink kind independently; an equivalent
restored guard; a sibling guard that does not dominate the sink; an opaque
validator; and a pointer/wrapper variant requiring additional evidence. A separate
terminator variant `buffer[requested_len] = 0` must require
`requested_len < capacity(buffer)`, even when the copy's `<=` bound is satisfied.
Declare the `memcpy` input non-overlapping in the copy fixture so overlap does
not confound the bounds question. Use a real backend and observable fixture
outputs when implementing replay/analysis tests; illustrative source is not proof
that optimization preserves a particular IR sink.

Separately, a diff may compare the requesting user's account ID with an invoice
owner while updating a different invoice selected by a request parameter. The
LLM can recognize the resource mismatch from source and API documentation and
propose a semantic concern without waiting for a buffer-overflow seeder. A cited
contract observation remains non-verified; if it needs program flow evidence,
the same admission/investigation boundary applies.

## 11. Disposition of every open issue

This table is local proposed priority, not a remote issue edit or a claim that
an existing acceptance contract changed. Numbers remain the canonical trackers.

| Issue | Direction | Gate or required refinement |
| --- | --- | --- |
| [#149](https://github.com/skg7on/VERITAS/issues/149) audit | Immediate contract reconciliation | Own the documented containment/completeness, allowed-epistemic, provenance-depth and summary-handoff repairs; split focused trackers before implementation. Map backend, verifier, lifecycle and link-unit gaps to the review stages below. |
| [#132](https://github.com/skg7on/VERITAS/issues/132) sink call sites | First production review correctness | Implement independent scoping/refusal workstreams 1–3 without waiting for all #124. Existing automated qualifying seeds still depend on producers; an LLM candidate does not bypass scoped evidence resolution. |
| [#124](https://github.com/skg7on/VERITAS/issues/124) missing facts | First real analytical review | Supply the range/capacity/alias/positive-check facts needed by unsafe, safe, mixed-path and alias-uncertain fixtures; preserve the full remaining issue scope. Staged deliveries cannot close unmet oracles. |
| [#79](https://github.com/skg7on/VERITAS/issues/79) review plugin | Product priority | Retain isolated controller and practice/real distinction. Amend protocols for bounded initial context and LLM candidates; add real backend and semantic-observation/report contracts before production qualification. |
| [#125](https://github.com/skg7on/VERITAS/issues/125) portable identity | Shared correctness priority | Keep architecture 05 and additive migration. A host-bound pilot is possible with explicit restrictions; portable/distributed caches remain blocked. |
| [#143](https://github.com/skg7on/VERITAS/issues/143) discarded invalidation | Review responsiveness priority | Retain hierarchical identity dependency and corruption policing. Measure cold/warm/no-change/changed-cone behavior; do not claim end-to-end cone cost merely from skipping WPA, because frontend/SVF may remain global. |
| [#144](https://github.com/skg7on/VERITAS/issues/144) CAS growth | Review repeated-run hygiene | Measure unchanged-run growth, implement verified reuse and retention. Do not turn a local concern into an unmeasured assertion that it dominates storage. |
| [#140](https://github.com/skg7on/VERITAS/issues/140) scaling umbrella | Retained capability programme; reprioritized | Separate full-analysis compatibility from review-budget experiments. M15–M19 require measurement and review value; changing fact coverage requires a versioned contract. |
| [#142](https://github.com/skg7on/VERITAS/issues/142) M14 corpus | Retain bounded diagnostic measurement | Keep chain/star/DAG and Release/Debug experiments; add review scope/candidate-cone measurements as a separately declared experiment. Completing the entire scaling programme is not a review entry gate. |
| [#145](https://github.com/skg7on/VERITAS/issues/145) closure measurement | Conditional diagnostic capability | Implement when needed to explain regime findings or justify closure redesign. Reuse native decoding; no duplicate canonical encoders. |
| [#20](https://github.com/skg7on/VERITAS/issues/20) M11 acquisition | Useful enabling work, staged priority | Artifact reuse/reporting can aid review cost. Full external bitcode ingestion is not a first review dependency. Split scope only with explicit tracker/spec amendment; existing acceptance remains intact. |
| [#21](https://github.com/skg7on/VERITAS/issues/21) M12A substrate | Deferred from first review critical path | Trigger when a named provider contributes evidence needed by measured review cases; preserve native/provider authority separation. |
| [#74](https://github.com/skg7on/VERITAS/issues/74) M12B Joern | Deferred from first review critical path | Requires M12A plus measured provider value, not merely available graph formats. |
| [#75](https://github.com/skg7on/VERITAS/issues/75) M12C fusion | Deferred from first review critical path | Requires qualified provider input and correct native evidence handoff; corroboration never promotes authority. |

Historical M0–M10 acceptance tests, #13/#70 delivery limitations and #116's
remaining producer scope are preserved. #124/#149 carry the unresolved work;
closed delivery issues are not proof of semantic completion. PTA research has
no active milestone number here and is deferred until a review benchmark
demonstrates a need existing SVF cannot meet.

## 12. Evaluation contract

Compare on the same pinned changes and build/program contexts:

- A: LLM with bounded source/diff/test/doc retrieval.
- B: same model and context policy plus structural/local evidence.
- C: same baseline plus selective interprocedural/alias/verification capabilities.

Run both a fixed-candidate evidence ablation and end-to-end discovery evaluation.
The former isolates whether evidence improves decisions; the latter measures
the additional defects each discovery path finds or misses. Report candidate
recall separately from adjudication precision. No arm silently drops a failing
analysis case or substitutes fake evidence.

Use pinned real changes with independently adjudicated defects and benign
counterexamples, plus controlled fixtures: overflow with a true guard, sibling
guard, narrowed allocation, alias-dependent lifetime, opaque validator,
cross-file API behavior, and a semantic resource/authorization contract.
Hold out projects or changes from model-policy tuning. Repeat stochastic model
runs and report spread; record model/prompt/provider versions and token budgets.

Measure actionable findings per reviewed change, adjudicated precision, known
defects missed, reviewer acceptance, investigation completion/truncation,
analysis/model wall time (median/p95), CPU, peak memory, store growth and tokens.
Include cold setup and warm review separately; reuse cannot hide native analysis
cost. Measure resource usage in Release as well as the historical Debug baseline.

Before running, freeze the fixture manifest, adjudication rubric and a numerical
latency/memory/token envelope appropriate to the target workstation. Expansion
is accepted only when it improves defect coverage or materially reduces false
concerns within that envelope. This review does not invent unsupported numerical
targets or claim a benchmark has already qualified the direction.

## 13. Acceptance for this design refinement

- One coherent product goal and a distinction between proposed and shipped behavior.
- Bounded source/context and dual candidate origins are specified without granting authority.
- Selective capability requests expose coverage, setup cost and failure.
- Existing EIR states, engine ownership and publication contracts remain intact.
- Every open issue has an explicit disposition; missing backend/lifecycle work has stage ownership.
- Roadmap starts with one real review and evaluation, rather than mandatory completion of all adapters/scaling work.
- Navigation and milestone status accurately distinguish delivered foundations from unresolved correctness.

The link-unit/program-boundary and post-overflow defect-family gaps recorded by
#149 remain explicit roadmap work: R2 owns selected-program scoping, while
additional production defect families are deferred until R3 evaluation justifies
them. Add dedicated trackers before implementation; evaluation fixtures are not
a claim of supported production domains.

Implementation starts after this specification is reviewed and capability-specific
plans/contracts are amended. No product code, protocol, issue state or remote
review comment is changed by this documentation refinement.

## 14. Specification review dispositions

| Document family | Disposition and reason |
| --- | --- |
| Platform / WPA / SummaryDB architecture (01–03) | Refine thesis and delivery status; retain native engine, summary and publication boundaries. Mark million-line sizing unvalidated and global incrementality incomplete. |
| Evidence architecture / EIR formal spec / M10C | Retain grammar, canonicalization and authority checks; add review context outside EIR and report annotations without new enum values. Missing handoff/completeness remains a correctness repair. |
| Portable target architecture (05) / M2 identities | Retain explicit target and additive migration; no portable-cache promise before #125. |
| Engineering backbone / M1–M9 | Retain accepted data, engine and qualification contracts; remove the product sequencing assumption that full scale must precede agent feedback. |
| M5 SVF / alias tiers / historical PTA proposal | Retain full native provider; separate future lightweight coverage from disabling required SVF. No new PTA implementation without measured review need. |
| M10A / M10B / API-to-EIR test contract | Prioritize real producer facts and scoped evidence. Keep deferred oracles and no-fabrication assertions until qualified; do not repurpose representation fixtures as end-to-end defect proof. |
| Agent security use cases | Replace deterministic-only discovery and exceptional-source assumptions; keep EIR-L0-first for admitted cases and bounded expansions. Existing defect patterns remain evaluation targets. |
| Claude Code plugin spec/plan | Preserve transport/capability/authority isolation; require explicit context/candidate/report protocol amendments and a real backend. Fake practice cannot qualify production. |
| Link-unit/program-boundary spec | Retain need for program isolation; output-directory inference is advisory and must be checked against real linked membership before authoritative whole-program claims. |
| M11 unified ingest | Retain approved acquisition/fidelity/cache invariants; optional external input breadth follows the first source-based review. |
| M12 provider/Joern/fusion specs | Retain separate provider authority and identity; defer from the first reviewer until a named provider improves measured cases. |
| WPA qualification / witness identity / in-process execution | Retain compatibility and corruption controls; correct historical delivery labels. Qualification protects analytical semantics, not review efficacy. |
| Performance rounds / observability / M13 record | Retain measurements and perturbation controls; mark delivered or reverted status and preserve historical measurements. Use new review-cost experiments for proposed scope changes. |
| Scaling roadmap / M14 plans | Retain falsifiable regime experiments; separate full-analysis preservation from intentionally narrower review coverage. Conditional redesign cannot silently alter accepted facts. |
| CI / documentation information architecture specs | Retain build/verification and canonical paths; no product redesign required. |

The review updates directional entry points and stale status assertions rather
than rewriting completed milestone plans or claiming their unchecked examples
are current implementation instructions. Capability-specific executable plans
are prepared only after this refinement is reviewed.
