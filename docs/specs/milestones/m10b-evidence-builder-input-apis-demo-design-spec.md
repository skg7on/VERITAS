# M10B Evidence Builder Input APIs and First Demo Design Spec

**Status:** Draft
**Milestone:** M10B
**Depends on:** M6 thin CPG, M9 fact/provenance store, M10A recursive domain expansion
**Feeds:** Evidence IR implementation and future Review Agent tools

---

# 1. Purpose

M10B exposes semantic slices that Evidence IR needs. It does not implement full
EIR serialization. It proves the backbone can produce compact,
provenance-backed evidence inputs for a concrete memory-safety case.

M10A is a separate prerequisite milestone. It expands the compiled-Souffle
recursive domains with `MayRead`, `GlobalFlow`, `UnknownEffect`, and
`SoundnessCoverage`, their models, and conformance suites. M10B builds the
Evidence/API surface over those M10A relations and the durable M9 fact/witness
store; it does not move recursive analysis into the query layer.

Target demo:

```text
packet.length
    -> decode argument
    -> copy length
    -> memcpy size

range(packet.length) = [0, 65535]
capacity(destination) = 2048
no dominating bounds check
```

---

# 2. Evidence Query Model

Evidence queries are semantic operations:

```text
get value flow from source to sink
get range facts for value
get memory facts for object
get aliases for memory ref
get call path
get dominating checks
get unknowns
get provenance closure
```

The query layer hides whether facts came from Clang, LLVM, SVF, Souffle, CPG projection, or direct SummaryDB lookup.

---

# 3. Slice Types

```text
FlowSlice {
    nodes
    edges
    supporting_facts
    contradicting_facts
    unknowns
    provenance_refs
    truncation
}
```

```text
EvidenceQueryBudget {
    max_depth
    max_nodes
    max_paths
    max_provenance_depth
}
```

Budget truncation is visible. A truncated slice must not masquerade as complete evidence.

---

# 4. API Contract

```cpp
namespace veritas::evidence {
class EvidenceQueryService {
 public:
  StatusOr<FlowSlice> GetValueFlow(
      core::StableId src,
      core::StableId dst,
      EvidenceQueryBudget budget) const;

  StatusOr<std::vector<facts::Fact>> GetRanges(core::StableId value_ref) const;
  StatusOr<std::vector<facts::Fact>> GetAliases(core::StableId memory_ref) const;
  StatusOr<std::vector<facts::Fact>> GetUnknowns(core::StableId scope_ref) const;
  StatusOr<std::vector<facts::Fact>> GetDominatingChecks(core::StableId callsite_ref) const;
  StatusOr<facts::ProvenanceGraph> Explain(
      core::StableId run_id,
      core::StableId fact_id,
      facts::ExplainBudget budget) const;
};
}
```

---

# 5. Demo Fixture

Files:

```text
tests/fixtures/cpp/overflow/packet.h
tests/fixtures/cpp/overflow/unsafe_packet.cpp
tests/fixtures/cpp/overflow/safe_packet.cpp
```

Unsafe shape:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  memcpy(b->data, p->payload, p->length);
}
```

Safe shape:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  if (p->length <= b->capacity) {
    memcpy(b->data, p->payload, p->length);
  }
}
```

The exact fixture can be adjusted for compile simplicity, but it must produce the semantic pattern above.

---

# 6. Query CLI

```bash
veritas-query evidence overflow \
    --sink memcpy \
    --format json \
    --max-depth 8 \
    --max-paths 5
```

Output includes:

```text
claim seed
source value
sink callsite
value-flow path
range facts
capacity facts
dominating check facts
unknowns
provenance refs
truncation
```

This JSON is diagnostic. Full EIR-T or Protobuf EIR is a later milestone.

---

# 7. Evidence Correctness Rules

* A slice is not proof by itself.
* Missing evidence must be represented as unknown.
* Summary edges must be marked expandable.
* Facts keep epistemic state.
* Provenance refs are included, but recursive provenance expansion is budgeted.
* Source text is optional and requested by reference, not copied by default.

---

# 8. Acceptance Tests

Required tests:

```text
unsafe fixture returns packet.length -> memcpy.size flow
unsafe fixture returns range wider than capacity
unsafe fixture reports no dominating bounds check
safe fixture reports dominating bounds check
flow slice includes summary IDs and provenance IDs
unknown external validation remains unknown
path budget truncates explicitly
JSON output is deterministic for golden fixture
```

---

# 9. Handoff to Evidence IR

M10B produces:

```text
FlowSlice
Fact refs
Provenance refs
Unknown refs
Summary refs
Source anchors
```

The Evidence IR milestone can then wrap these into:

```text
Claim
Evidence
Constraints
Assumptions
Unknowns
Provenance
Proof Obligations
```

M10B is complete when the first buffer-overflow evidence case can be generated
from M9 facts and M10A relations without an LLM and without loading entire
source files into a prompt.
