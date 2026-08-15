# M9 Provenance-Aware Fact Store and Explain API Design Spec

**Status:** Draft
**Milestone:** M9
**Depends on:** M8 derived fact tuples
**Feeds:** M10 Evidence APIs and future Evidence IR

---

# 1. Purpose

M9 stores current facts and provenance DAGs so every derived fact can answer:

```text
why is this true?
which summaries and source anchors contributed?
which rule derived it?
what uncertainty or assumption is visible?
```

This milestone turns SummaryDB from a cache into a proof-producing semantic infrastructure.

---

# 2. Fact Identity

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
scope
provenance_hash
```

---

# 3. Fact Store

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
    scope_kind
    scope_id
    provenance_id
    is_current
}
```

Current fact replacement is metadata mutation. Historical facts remain readable.

---

# 4. Provenance Model

Provenance is a derivation DAG:

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

# 5. Epistemic Propagation

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

---

# 6. Explain API

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

# 7. CLI Contract

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

# 8. Acceptance Tests

Required tests:

```text
same semantic predicate with different provenance -> distinct FactID
semantic_fact_hash matches across equivalent revisions
MAY input produces MAY derived fact
INFERRED input cannot become MUST
ASSUMED input appears in explanation
budgeted explanation truncates explicitly
current fact replacement keeps historical fact readable
```

---

# 9. Handoff to M10

M10 consumes:

```text
FactStore
ProvenanceStore
Explain API
epistemic states
source anchors
summary refs
```

M9 is complete when every M8 fact can be published and explained through a stable API.

