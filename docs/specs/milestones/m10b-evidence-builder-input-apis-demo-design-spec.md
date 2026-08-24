# M10B Evidence Builder Input APIs and First Demo Design Spec

**Status:** Approved
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
    metadata
}
```

```text
EvidenceFactSet {
    facts
    metadata
}

QueryResultMetadata {
    completeness = UNSPECIFIED | COMPLETE | TRUNCATED
    truncation_reasons
    examined_items
    analysis_run_id
    query_provenance_id
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
    query_completion_facts
    query_completion_bindings
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
    max_facts_per_query
    max_provenance_depth
}
```

All budget limits are positive. Reaching a limit exactly is complete when the
backend proves there is no additional matching result; discovering one more
result returns the canonical prefix and reports the corresponding stable
truncation reason. Budget truncation is visible, and a truncated slice must not
masquerade as complete evidence.

Every flow or fact result carries the same `QueryResultMetadata` shape.
`query_provenance_id` is a `core::IdKind::kFact` reference to an M9-backed
`evidence.query_completion.v1` completion fact. The completion fact records the
query kind, ordered scope, budget, implementation version,
`input_snapshot_fingerprint`, completeness, ordered reasons, examined count,
and canonical `returned_member_digest`. Its M9 run binding and selected witness
must resolve in the returned provenance closure. Complete-empty and
truncated-empty results are therefore distinct and auditable.

M12C extends `input_snapshot_fingerprint` without changing this completion-fact
shape. When external providers are explicitly selected, the fingerprint also
binds their ordered provider run/projection bindings, capability digests,
mapping versions, and assumption-set digests. Provider selection and exact
provider provenance are therefore as reproducible and stale-detectable as the
native CPG/fact snapshot.

`EvidenceBuildInput` explicitly carries the query-completion `facts::Fact`
values, their M9 `RunFactBinding` values, and the selected witnesses in its
provenance graph. It is the only typed handoff M10C consumes, so M10C never
parses M10B's diagnostic JSON or performs a second provenance lookup. The
[M10B–M10C API-to-Evidence-IR test contract](m10b-m10c-api-to-evidence-ir-test-design-spec.md)
defines the exact metadata validation, query-completion relation, case IDs,
oracles, and milestone ownership.

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

  StatusOr<EvidenceFactSet> GetRanges(
      core::StableId value_ref, EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetCapacities(
      core::StableId memory_ref, EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetAliases(
      core::StableId memory_ref, EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetUnknowns(
      core::StableId scope_ref, EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetDominatingChecks(
      core::StableId callsite_ref, EvidenceQueryBudget budget) const;
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

`BuildEvidenceInput` pins one immutable native CPG projection, one M9 read
snapshot, and—when M12 providers are enabled—an ordered set of immutable
provider run/projection bindings before issuing any subquery. Repository,
revision, build variant, analysis configuration, analysis run, native
projection, provider runs/projections, and fact snapshot must agree for every
returned member. Any selected binding change produces a stable retryable
failure instead of a mixed handoff.

---

# 5. Demo Fixture

Use isolated project fixtures so test setup, rather than a fixture-specific CLI
flag, selects one deterministic finding:

```text
tests/fixtures/projects/evidence_overflow_unsafe/...
tests/fixtures/projects/evidence_overflow_safe/...
tests/fixtures/projects/evidence_overflow_non_dominating/...
tests/fixtures/projects/evidence_overflow_mixed_paths/...
tests/fixtures/projects/evidence_overflow_opaque_validator/...
tests/fixtures/projects/evidence_overflow_alias_uncertain/...
tests/fixtures/projects/evidence_overflow_summary/...
```

Unsafe shape:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  memcpy(b->data, p->payload, p->length);
}
```

Safe dominating shape:

```cpp
void copy_payload(Packet* p, Buffer* b) {
  if (p->length <= sizeof(b->data)) {
    memcpy(b->data, p->payload, p->length);
  }
}
```

The remaining projects isolate a sibling-branch non-dominating check, mixed
checked and unchecked paths, an unmodeled external validator, uncertain alias,
and a cross-translation-unit summary flow. Exact fixture requirements and
forbidden outcomes are governed by section 6 of the companion test contract.

---

# 6. Query CLI

```bash
veritas-query evidence overflow \
    --sink memcpy \
    --format json \
    --max-depth 8 \
    --max-nodes 256 \
    --max-paths 5 \
    --max-facts 64 \
    --max-provenance-depth 8
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
* A complete empty open-world query becomes an unknown or omission.
* A truncated empty query never becomes negative evidence.
* Only a complete, scoped dominating-check query with a matching query
  completion fact may feed the registered
  `evidence.closed_world.dominating_check_absence.v1` rule.
* The derived negative fact retains the completion fact as a provenance input.
* External-provider absence never contributes to closed-world completeness or
  negative evidence.
* A selected provider's positive contradiction or unresolved in-scope
  candidate prevents an unqualified negative result even when the native query
  is complete-empty.

---

# 8. Acceptance Tests

The M10B completion gate is the companion contract's 23 owned cases:

```text
AC-001 through AC-006     contract and deterministic diagnostic JSON
QRY-001 through QRY-010  semantic queries, budgets, aliases, and provenance
HND-001 through HND-006  immutable typed handoff and snapshot consistency
DEM-001                  M10B slice-JSON compatibility
```

Every case asserts typed required and forbidden outputs before any golden
comparison. The focused cases run under the `evidence-contract` and
`evidence-integration` CTest labels, and M10B completion also requires the full
repository suite.

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
Query completion facts, run bindings, and selected witnesses
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

M12C later makes this same provider-neutral handoff capable of citing Joern and
future provider observations. GraphSON/GraphML objects, Joern ordinals, and
provider-native types never enter `EvidenceBuildInput`; selected provider
projections, capabilities, assumptions, and observations are represented by
stable SummaryDB and provenance references.

See the
[M10C Evidence IR semantic model and serialization specification](m10c-evidence-ir-semantic-model-serialization-design-spec.md)
for validation, EIR-L0/L1/L2 assembly, content identity, and representation
boundaries.

The cross-cutting
[Evidence IR Agent security use cases](../veritas-evidence-ir-agent-security-use-cases-design-spec.md)
show how this deterministic slice becomes progressive EIR-L0/L1/L2 input to a
future Review Agent and extend the same interaction contract to other common C
and C++ security patterns.
