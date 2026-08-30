# Tutorial: Build an Agent-Based Code-Review Tool

This tutorial shows how a future review tool should consume VERITAS SummaryDB
and Evidence IR. It is an integration blueprint for the approved M9, M10B,
M10C, and M12C contracts—not a runnable tutorial in the current tree.

The key design goal is narrow:

> The Agent reasons over a compact, immutable Evidence case and can propose
> hypotheses or proof obligations. It cannot convert its own output into an
> authoritative program fact.

## 1. Wait for the required platform gates

Do not build production Agent integration directly on the current SQLite
schema or native CPG CLI. The safe boundary depends on:

| Gate | Required delivery |
| --- | --- |
| M9 | Durable canonical facts, run bindings, selected rooted witnesses, snapshot reads, and `Explain(run_id, fact_id)` |
| M10A | Recursive domains needed by the first memory-safety demo |
| M10B | Bounded semantic query APIs, query-completion facts, and immutable `EvidenceBuildInput` |
| M10C | Validated `EvidenceCase`, canonical `EvidenceID`, EIR-T, Protobuf, and diagnostic EIR JSON |
| M12C, optional | Pinned provider selection, fusion/conflict records, provider-aware completion and Evidence dependencies |

The current repository has approved designs/plans for these gates, but does not
yet expose `EvidenceQueryService` or `EvidenceCaseBuilder`.

## 2. Choose one registered finding type

Start with the M10B buffer-overflow case rather than a generic “review this
repository” prompt:

```cpp
void copy_payload(Packet* packet, Buffer* destination) {
  memcpy(destination->data, packet->payload, packet->length);
}
```

The registered semantic question is:

```text
Can the size argument reaching this copy exceed the destination capacity on
an admitted path without a dominating bounds check?
```

Its inputs are bounded semantic operations:

```text
value flow: packet.length -> memcpy size
range: packet.length
capacity: destination.data
aliases: destination memory reference
call path: entry -> copy_payload -> memcpy
dominating checks: copy callsite
unknowns: external validators, unresolved aliases, incomplete paths
provenance: selected witnesses for every derived fact
```

A new defect type should register its claim seed, predicate mapping, evidence
queries, negative-evidence policy, and verifier kinds before an Agent can use
it.

## 3. Pin one immutable review snapshot

At review start, resolve and freeze:

```text
repository_id
revision_id
build_variant_id
analysis_run_id
native_projection_id
fact_snapshot_id
ordered provider run/projection bindings, if selected
query implementation and budget
```

Hash the full selection into an input snapshot fingerprint. Every query
completion fact and the final `EvidenceCase` must refer to it.

Do not issue independent “current” reads during one case. If a binding advances
between subqueries, abort with a retryable snapshot-change status instead of
combining facts from two analyses.

## 4. Expose bounded semantic tools

Give the orchestration layer typed tools such as:

```text
get_value_flow(source_id, sink_id, budget)
get_ranges(value_id, budget)
get_capacities(memory_id, budget)
get_aliases(memory_id, budget)
get_dominating_checks(callsite_id, budget)
get_unknowns(scope_id, budget)
explain_fact(run_id, fact_id, budget)
expand_summary(summary_id, component, budget)
```

Each response should use the same envelope:

```json
{
  "input_snapshot_fingerprint": "<canonical-fingerprint>",
  "query_kind": "value_flow",
  "scope": ["value:sha256:<source>", "value:sha256:<sink>"],
  "members": [],
  "supporting_fact_ids": [],
  "contradicting_fact_ids": [],
  "unknown_ids": [],
  "provenance_root_ids": [],
  "completion": "COMPLETE",
  "truncation_reasons": [],
  "examined_items": 0,
  "query_completion_fact_id": "fact:sha256:<digest>"
}
```

This JSON illustrates an application envelope; M10B's C++ types and
`relations.v2` query-completion fact are authoritative. Provider-native fields,
Joern IDs, and GraphSON objects do not belong in it.

### Tool design rules

- IDs are typed stable IDs; reject the wrong domain.
- All budgets are positive and part of the query identity.
- Return canonical prefixes on truncation plus stable reasons.
- Separate supporting evidence, contradiction, unknown, and omission.
- Include expandable summary/provenance references instead of eagerly copying
  the whole program.
- A complete empty open-world query is not negative evidence.
- A truncated empty query is always incomplete.
- Imported provider absence never establishes closed-world absence.

## 5. Assemble `EvidenceBuildInput`

The orchestrator creates one typed claim seed:

```text
ClaimSeed {
  finding_id
  kind = BUFFER_OVERFLOW
  severity
  subject_ref
  source_ref
  sink_ref
}
```

Then `BuildEvidenceInput` gathers the flow, ranges, capacities, aliases,
dominating checks, unknowns, completion facts, run bindings, and selected
witnesses under the pinned snapshot.

The result is immutable. M10C must not parse diagnostic JSON or query the
database again. Otherwise the case could contain a flow from one snapshot and
a range or witness from another.

## 6. Convert to a validated Evidence case

`EvidenceCaseBuilder` performs deterministic assembly:

```text
validate context and completion metadata
  -> assign deterministic case-local references
  -> map typed facts without epistemic strengthening
  -> preserve support and contradiction separately
  -> construct paths and summary expansion markers
  -> attach provenance and dependencies
  -> materialize unknowns and omissions
  -> create proof obligation
  -> project requested EIR level
  -> validate and compute EvidenceID
```

For the first unsafe fixture, a simplified L1 case should communicate:

```text
claim
  possible buffer overflow at the memcpy callsite

support
  value flow from packet.length to copy size
  range upper bound greater than destination capacity
  complete scoped query proving no admitted dominating check

uncertainty
  unresolved external validator semantics, if present
  alias or path truncation, if present

proof obligation
  prove or refute size <= capacity on every feasible admitted path

state
  POSSIBLE_DEFECT
```

The deterministic builder may initialize `POSSIBLE_DEFECT`. It may not emit a
`LIKELY_*` or `VERIFIED_*` state on its own.

## 7. Give the Agent a narrow contract

The Agent receives canonical or pretty EIR plus a small tool catalog for
bounded expansions. A useful instruction contract is:

```text
1. Reason only from the supplied Evidence case and explicit tool results.
2. Distinguish MUST, MAY, MUST_NOT, INFERRED, ASSUMED, and UNKNOWN.
3. Treat truncation and omission as missing information, never as absence.
4. Cite Evidence member and provenance IDs for every conclusion.
5. Output only findings, hypotheses, requested expansions, and proof
   obligations allowed by the response schema.
6. Never claim VERIFIED_DEFECT or VERIFIED_SAFE.
```

The response should be typed, for example:

```json
{
  "evidence_id": "evidence:sha256:<digest>",
  "assessment": "LIKELY_DEFECT",
  "rationale_refs": ["@flow_1", "@range_1", "@capacity_1"],
  "hypotheses": [
    {
      "predicate": "value(@copy_length) > capacity(@destination)",
      "epistemic": "INFERRED",
      "support_refs": ["@flow_1", "@range_1", "@capacity_1"]
    }
  ],
  "requested_expansions": [],
  "proof_obligations": ["@prove_copy_bound"]
}
```

`assessment` is an Agent recommendation, not the authoritative Evidence-case
verification state. Store the response separately with model, prompt, policy,
tool-call, and input Evidence identities.

## 8. Dispatch deterministic verification

Map each proof obligation to a registered verifier adapter:

| Obligation | Candidate verifier |
| --- | --- |
| Range implication | SMT over typed constraints |
| Dominance/path condition | Native CFG/dominator and symbolic path check |
| Alias premise | Qualified SVF refinement or another registered static analysis |
| Concrete failing input | Instrumented replay, test, or bounded symbolic execution |

The verifier request contains:

```text
proof obligation ID
EvidenceID and snapshot fingerprint
typed predicate/constraints
referenced facts and witnesses
verifier name/version/configuration
resource budget
```

The verifier result contains:

```text
PROVED | REFUTED | UNKNOWN | INCOMPLETE
exact verifier/toolchain identity
canonical inputs and result digest
checkable witness or counterexample reference
diagnostics and truncation
```

Only a policy-authorized state-transition component may use this result to
promote or refute a claim. It validates that the result belongs to the same
Evidence snapshot and that every cited dependency is still current.

## 9. Handle provider observations safely

When M12C is enabled, a review explicitly selects providers:

```text
native
native + joern:<configuration-id>
native + phasar:<configuration-id>
```

The exact ordered selection participates in the Evidence snapshot. Fusion
produces comparison records rather than rewriting facts:

```text
SameProgramEntity
Corroborates
Contradicts
Refines
UnresolvedIdentity
```

Two external providers agreeing is corroboration, not verification. A positive
external contradiction must remain visible and can block native negative
evidence. An absent external relation proves nothing.

## 10. Produce a review finding

Only after verification should the presentation layer construct a code-review
finding. Keep the Evidence and verification references attached:

```text
title
severity and authoritative state
stable subject/callsite ID
source anchor, if available
short explanation
supporting and contradicting Evidence refs
unknowns/assumptions that remain
verifier result and counterexample/proof ref
EvidenceID, snapshot ID, analysis run ID
```

Source text is optional presentation data retrieved through a stable anchor.
It is not a substitute for semantic evidence and should not be copied into the
canonical Evidence identity.

## 11. Incremental review

Each case records dependencies on summaries, facts, type layouts,
configurations, specifications, and selected provider components. On a new
revision:

```text
component delta
  -> Dependency Index
  -> mark dependent Evidence STALE or PARTIALLY_STALE
  -> rebuild the bounded input under a new snapshot
  -> compute new EvidenceID
  -> compare support, contradiction, unknowns, and verification state
```

Evidence-only changes may refresh anchors or explanations without rerunning
semantic WPA. Provider deltas invalidate only cases that selected those
provider components.

Never serve a stale case as current without a visible freshness state.

## 12. Test the review system

Use the isolated M10B fixture family:

```text
unsafe copy
safe dominating check
non-dominating sibling check
mixed checked/unchecked paths
opaque external validator
uncertain alias
cross-translation-unit summary flow
```

Assert required and forbidden typed outcomes:

- unsafe and safe cases remain distinguishable;
- non-dominating checks do not establish safety;
- truncated no-check queries never produce negative evidence;
- opaque validators and uncertain aliases become explicit unknowns;
- every derived fact resolves a finite rooted witness;
- reordering inputs does not change `EvidenceID`;
- a semantic change does change `EvidenceID`;
- EIR-T and Protobuf round-trip to identical semantic bytes;
- malformed Agent output cannot alter authoritative case state;
- mismatched verifier snapshot/result is rejected;
- imported absence cannot produce negative evidence;
- provider contradiction remains visible; and
- a dependency change marks only the affected cases stale.

Red-team the orchestration boundary as well: prompt-injection text from source
or provider properties is untrusted data, tool budgets cannot be bypassed, raw
file/SQL access is unavailable, and the Agent response parser rejects unknown
or authority-escalating fields.

## 13. Minimal service decomposition

Keep the implementation split into independently testable units:

```text
SnapshotResolver
  pins native/fact/provider state

EvidenceQueryService
  performs bounded semantic queries and completion accounting

EvidenceCaseBuilder
  assembles, validates, canonicalizes, and serializes EIR

ReviewAgentAdapter
  sends one Evidence case and validates typed Agent output

VerifierRegistry
  dispatches proof obligations to deterministic engines

VerificationTransitionService
  validates authority and changes claim state

ReviewFindingRenderer
  presents source-anchored results without changing semantics
```

This split prevents the Agent adapter from becoming a second analysis engine
or a privileged database client.

## 14. Completion checklist

Before calling an Agent-based reviewer production-ready, confirm:

- M9/M10B/M10C gates and required conformance suites pass;
- every Agent input has one validated `EvidenceID` and immutable snapshot;
- every tool response exposes completeness and provenance;
- Agent output is schema-validated and stored as non-authoritative;
- verifier adapters are registered, versioned, deterministic, and budgeted;
- only the transition service can set authoritative verification states;
- provider selection and assumptions are explicit;
- stale dependency handling is enforced;
- secrets, raw databases, arbitrary filesystem access, and network tools are
  absent from the Agent boundary unless separately authorized; and
- safe, unsafe, uncertain, truncated, contradicted, and stale cases all have
  executable tests.

Read the [M10B design](../specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md),
[M10C design](../specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md),
and [Evidence IR architecture](../architecture/04-evidence-ir-architecture.md)
before implementing this tutorial.
