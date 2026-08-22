# M9 Provenance-Aware Fact Store and Explain API Design Spec

**Status:** Draft target; blocked until every M8R entry criterion passes
**Milestone:** M9
**Depends on:** All M8R.1-M8R.5 gates and all ten executable M9 entry criteria
**Feeds:** M10A recursive domain expansion, M10B Evidence APIs, and future Evidence IR

---

# 1. Purpose

M9 stores complete analysis runs, current and historical facts, generic rooted
witness DAGs, diagnostics, semantic hashes, and stale state so every derived
fact can answer:

```text
why is this true?
which summaries and source anchors contributed?
which rule derived it?
what uncertainty or assumption is visible?
```

M9 does not start until the
[M8R bridge](m8r-souffle-wpa-remediation-design-spec.md) passes all ten entry
criteria without missing, extra, disabled, skipped, failed, or errored tests.
It does not recompute recursive WPA facts or accept engine-native rows.

This milestone turns SummaryDB from a cache into a proof-producing semantic infrastructure.

---

# 2. Only Input Contract: `AnalysisFactBatch`

M9 accepts one immutable `AnalysisFactBatch` constructed from a successful
`WpaRunResult`:

```text
AnalysisFactBatch {
    RunId
    BatchId
    expected_component_keys[]
    completed_components[]       // key + logical/fixpoint/external hashes
    rooted_input_fact_ids[]
    canonical_facts[]
    witnesses[]
    diagnostics[]
}
```

The producer and Fact Bus must prove before M9 persistence:

* expected and completed component sets are identical;
* every witness is finite, acyclic, and closed over published facts and the
  declared rooted-input set;
* all facts and witnesses belong to the same run manifest;
* batch identity is canonical and multi-sink delivery is idempotent at least
  once under `(RunId, BatchId)`;
* partial fan-out is recorded per sink and retry cannot duplicate logical
  publication.

Raw `FactTuple` vectors, partial component results, and mixed-run envelopes are
not M9 inputs.

---

# 3. Fact Identity

Two hashes are required:

```text
FactID:
    exact fact in one revision/build/analyzer/provenance context

semantic_fact_hash:
    fact equivalence across revisions without revision or provenance
```

`FactID` includes:

```text
revision_id
build_variant_id
predicate_kind
canonical predicate
subject
epistemic
producer
analyzer_run_id
analysis_run_id
scope
provenance_hash
```

`analysis_run_id` binds revision, build variant, summary/relation schemas,
rule/model bundles, SVF/WPA configurations, engine identity, and exact
engine/toolchain identity. Production Souffle and C++ conformance/emergency
runs are always distinct.

---

# 4. Fact Store

Logical row:

```text
Fact {
    fact_id
    semantic_fact_hash
    revision_id
    build_variant_id
    predicate_kind
    predicate_canonical
    subject_kind
    subject_id
    epistemic
    confidence
    producer_kind
    analyzer_run_id
    analysis_run_id
    scope_kind
    scope_id
    provenance_id
    is_current
}
```

Current fact replacement is metadata mutation. Historical facts remain readable.

Batch publication is atomic: either the complete validated batch becomes
visible or no new run facts do. An incomplete/failed run retains diagnostics
and may mark the previous successful result stale, but never replaces or mixes
with it.

---

# 5. Provenance Model

Provenance is the generic rooted witness DAG selected by the M8R canonicalizer:

```text
input fact or summary component
    -> producer/rule/analyzer
    -> output fact
```

Node:

```text
ProvenanceNode {
    provenance_id
    producer_kind
    producer_id
    rule_id
    rule_version
    analyzer_run_id
    source_anchor_id
    summary_id
    fact_id
    description
}
```

`explainFact` reads this persisted witness. It never re-runs C++ or Souffle and
does not reconstruct relation-specific joins.

Edge:

```text
ProvenanceEdge {
    output_provenance_id
    input_kind
    input_id
    input_role
}
```

---

# 6. Epistemic Propagation

States:

```text
MUST
MAY
MUST_NOT
INFERRED
ASSUMED
UNKNOWN
```

Rules:

```text
MUST + sound rule -> MUST
MAY + sound rule -> MAY
ASSUMED input -> output records assumption dependency
INFERRED input -> output remains INFERRED unless verified
UNKNOWN input -> output is UNKNOWN or MAY according to rule policy
```

Confidence is stored separately from epistemic state.

Negative semantic information is not absence: for example, `NO_ALIAS + MUST`
is distinct from `UNKNOWN_ALIAS`, an unknown epistemic state, or no row.

---

# 7. Explain API

```cpp
namespace veritas::facts {
class ProvenanceStore {
 public:
  Status PutNode(ProvenanceNode node);
  Status PutEdge(ProvenanceEdge edge);
  StatusOr<ProvenanceGraph> Explain(
      core::StableId provenance_id,
      ExplainBudget budget) const;
};
}
```

Budget:

```text
max_depth
max_nodes
include_source_anchors
include_summary_ids
include_datalog_derivation
```

If truncated, the explanation graph must include an explicit truncation marker.

---

# 8. CLI Contract

```bash
veritas-explain fact <fact_id> --max-depth 5 --max-nodes 100
```

Output sections:

```text
Fact
Epistemic
Confidence
Producer
Rule
Inputs
Assumptions
Unknowns
Source anchors
Truncation notice, if any
```

---

# 9. Acceptance Tests

Required tests:

```text
same semantic predicate with different provenance -> distinct FactID
semantic_fact_hash matches across equivalent revisions
MAY input produces MAY derived fact
INFERRED input cannot become MUST
ASSUMED input appears in explanation
budgeted explanation truncates explicitly
current fact replacement keeps historical fact readable
expected/completed component mismatch rejects the batch
unrooted witness leaf rejects the batch
same (RunId, BatchId) delivered twice is logically idempotent
partial multi-sink delivery resumes without duplicate publication
failed run leaves the prior successful run queryable as stale
```

---

# 10. Handoff to M10A and M10B

M10A adds new recursive domains using the same run, fact, and rooted-witness
contracts. M10B consumes:

```text
FactStore
ProvenanceStore
Explain API
epistemic states
source anchors
summary refs
```

M9 is complete when every accepted `AnalysisFactBatch` is published atomically,
idempotently, and explainably through stable APIs, with run history and stale
state preserved.
