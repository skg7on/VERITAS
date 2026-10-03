# VERITAS Review Agent Architecture

**Status:** Proposed direction, following the requested design review; no runtime change
**Date:** 2026-10-03
**Specification:** [Review-driven analysis](../specs/veritas-review-driven-analysis-design-spec.md)
**Delivery direction:** [Review roadmap](../plans/veritas-review-agent-milestone-roadmap.md)

## 1. Product goal

VERITAS helps an LLM investigate code changes using bounded, provenance-backed
program evidence, and applies stronger analysis when needed to support a review
claim. Success means actionable findings with inspectable reasons at a usable
review cost. Comprehensive whole-program fact production is a supporting
capability, not an entry requirement for every review.

The initial language remains C/C++. The initial analytical demonstration remains
buffer overflow. The first real review must also exercise a semantic contract
concern that is not already supplied as an analyzer finding.

## 2. Review loop

```text
base revision + pinned working snapshot + selected program/build
                          |
              bounded diff and source context
                          |
        deterministic seeds + LLM candidate concerns
                          |
              candidate admission and deduplication
                          |
              claim-specific investigation controller
               /              |                 \
     source/tests/docs    SummaryDB/EIR     analysis capabilities
               \              |                 /
              evidence, contradictions and unknowns
                          |
          bounded refinement / configured verification
                          |
         supported review report + coverage statement
```

The controller owns snapshot binding, capability selection, request validation,
resource limits and reporting. The LLM proposes concerns, candidate API models
and evidence requests; it cannot write authoritative facts or verification
states. Existing `veritas-build analyze` remains the full native analysis path.

## 3. Three boundaries

**Context is readable data.** The reviewer receives bounded diffs, source,
tests and documentation through controller-mediated reads. EIR organizes the
analytical argument; it does not exclude source semantics from reasoning.
Repository text never changes tools, budgets or producer authority.

**Candidates are hypotheses.** A structural seeder or the LLM may suggest a
claim. Admission checks the snapshot, anchors and scope without pretending to
prove the claim. An unresolved semantic candidate may remain a report-level
observation before an EIR case is available. Existing EIR grammar is unchanged.

**Evidence has scoped authority.** Analysis and verification results retain
their producer, model assumptions, execution domain and completeness. A test
can reproduce a defect for one input; it cannot establish universal safety.
An abstract may-flow is a reason to investigate, not proof of a feasible bug.

## 4. Analysis selection

Use structural/local facts first when available. Ask for interprocedural
summaries, SVF-backed alias/call refinement, or configured solver/replay support
when missing evidence affects a finding. These are requested capability levels,
not promises that every deeper level is implemented or cheaper on demand.

The pinned SVF/Soufflé contracts remain mandatory for the existing native
analysis command. A future lightweight review provider requires a separate,
versioned contract and coverage record. It must not produce full-analysis
identities, silently omit calls or certify absence across an unexamined boundary.
SVF-backed demand-driven refinements may still require a whole-program base;
their setup, retained memory and execution costs are all measured.

## 5. Reuse and persistence

Retain immutable Function Summary IR, SummaryDB publication, rooted provenance,
thin CPG, explicit epistemic states and EIR validation. A missing capability
becomes an explicit unknown or unsupported result.

Future bounded queries should prefer compact summaries and reconstructible
witnesses to eagerly persisted all-pairs closure. Any change to relation
meaning, coverage, identity or publication atomicity requires its own versioned
design. The existing whole-run `AnalysisFactBatch` is not silently replaced by
partially published review results.

Review occurrence, LLM hypothesis, evidence identity and authoritative fact
identity remain separate. Evidence caches record negative-query scopes and
source/model dependencies, including additions that could invalidate absence.
Until that dependency system is implemented, reuse is limited to one pinned
snapshot and exact compatible analysis bindings.

## 6. Product results

Reports distinguish verified findings, supported concerns, semantic review
observations and inconclusive investigations. These presentation categories do
not add EIR enum values or change its epistemic lattice. Only configured,
obligation-bound authority can establish `VERIFIED_DEFECT` or `VERIFIED_SAFE`.
Semantic observations cite source and an explicit contract; uncertain intent
remains labeled. Severity is independent of authority.

No findings means no actionable concern was found within declared coverage.
It never means that the repository or even the whole changed function is safe.

## 7. Direction and precedence

This proposal refines product goal, context access, candidate origins and work
priority in architecture documents 01–04. Their data/engine contracts, the EIR
formal specification, approved milestone acceptance tests and schema versions
remain binding until a separately reviewed implementation amendment changes
them. Existing plans are historical or capability-specific guidance; they are
not an automatic requirement to complete M11–M19 before building a reviewer.

The cross-cutting specification records the issue disposition and implementation
gates. This document does not claim those gates have passed or update remote
issue state.
