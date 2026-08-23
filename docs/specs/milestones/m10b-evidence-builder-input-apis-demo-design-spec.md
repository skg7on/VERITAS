# M10B Evidence Builder Input APIs and First Demo Design Spec

**Status:** Draft
**Milestone:** M10B
**Depends on:** M6 thin CPG, M9 fact/provenance store, M10A recursive domain expansion
**Feeds:** M10C Evidence IR semantic model and serialization, then future Review Agent tools

---

# 1. Purpose

M10B exposes semantic slices and a typed, immutable handoff that M10C needs. It
does not implement the Evidence IR semantic model or serialization. It proves
the backbone can produce compact,
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
EvidenceFactSet {
    facts
    completeness = COMPLETE | TRUNCATED
    truncation_reasons
}

ClaimSeed {
    finding_id
    claim_kind = BUFFER_OVERFLOW
    severity
    subject_ref
    source_ref
    sink_ref
}

EvidenceBuildInput {
    claim_seed
    flow_slice
    ranges
    capacities
    aliases
    dominating_checks
    unknowns
    provenance
}
```

M10B owns the seed-level `ClaimKind` and `Severity` enums because the finding
seed crosses the M10B/M10C boundary. The initial `ClaimKind` contains
`BUFFER_OVERFLOW`; severity contains `CRITICAL`, `HIGH`, `MEDIUM`, `LOW`, and
`INFO`. Both have invalid sentinels, stable text conversion, and rejecting parse
helpers. M10C reuses these types rather than declaring look-alike enums.

```text
EvidenceQueryBudget {
    max_depth
    max_nodes
    max_paths
    max_provenance_depth
}
```

Budget truncation is visible. A truncated slice must not masquerade as complete
evidence. Every fact lookup returns an `EvidenceFactSet`; complete-empty and
truncated-empty results are distinct. `EvidenceBuildInput` is the only typed
handoff M10C consumes, so M10C never parses M10B's diagnostic JSON.

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

  StatusOr<EvidenceFactSet> GetRanges(core::StableId value_ref) const;
  StatusOr<EvidenceFactSet> GetCapacities(core::StableId memory_ref) const;
  StatusOr<EvidenceFactSet> GetAliases(core::StableId memory_ref) const;
  StatusOr<EvidenceFactSet> GetUnknowns(core::StableId scope_ref) const;
  StatusOr<EvidenceFactSet> GetDominatingChecks(core::StableId callsite_ref) const;
  StatusOr<facts::ProvenanceGraph> Explain(
      core::StableId run_id,
      core::StableId fact_id,
      facts::ExplainBudget budget) const;

  StatusOr<EvidenceBuildInput> BuildEvidenceInput(
      const ClaimSeed& claim_seed,
      EvidenceQueryBudget budget) const;
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
per-query completeness and truncation reasons
```

This JSON is diagnostic. M10C owns EIR-T, Protobuf, and full-EIR diagnostic
JSON.

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
complete-empty and truncated-empty fact results remain distinguishable
typed EvidenceBuildInput contains every required query result and provenance graph
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

M10C can then wrap these into:

```text
Claim
Evidence
Constraints
Assumptions
Unknowns
Provenance
Proof Obligations
```

M10B is complete when the first buffer-overflow `EvidenceBuildInput` can be
generated from M9 facts and M10A relations without an LLM and without loading
entire source files into a prompt. M10C is responsible for turning that input
into an `EvidenceCase`.

See the
[M10C Evidence IR semantic model and serialization specification](m10c-evidence-ir-semantic-model-serialization-design-spec.md)
for validation, EIR-L0/L1/L2 assembly, content identity, and representation
boundaries.

The cross-cutting
[Evidence IR Agent security use cases](../veritas-evidence-ir-agent-security-use-cases-design-spec.md)
show how this deterministic slice becomes progressive EIR-L0/L1/L2 input to a
future Review Agent and extend the same interaction contract to other common C
and C++ security patterns.
