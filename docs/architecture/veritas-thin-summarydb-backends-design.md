# VERITAS Thin SummaryDB and Pluggable Backends

## Physical Layers, Identity Model, Content-Addressed Storage, and Backend Adapter Contract

**Status:** Draft Architecture Specification
**Version:** 0.2
**Project:** VERITAS
**Peers:**

* `docs/architecture/veritas-platform-architecture-design.md` — platform pipeline, principles P1–P8, ingest adapter tiers.
* `docs/architecture/veritas-whole-program-analysis-design.md` — analyzer engines and SOTA C/C++ alias policy.
* `docs/architecture/veritas-evidence-ir-design.md` — Evidence IR consumed by the Agent.

---

# 1. Purpose

The SummaryDB is the persistent, incrementally maintainable semantic representation of a very large software system. Its purpose is not to cache function metadata; it is to be the substrate on which whole-program analysis, incremental invalidation, provenance, and Evidence generation all rest.

This document specifies:

* the physical decomposition of the SummaryDB into seven layers (§2);
* the identity model — from `RepositoryID` down to `FunctionSummaryID` (§5);
* the content-addressed object model and component-hash matrix (§6, §7);
* the pluggable backend adapter contract with reference bindings for RocksDB, SQLite, and in-memory (§9);
* publication atomicity and reader consistency (§10);
* the read API the rest of the platform consumes (§8).

Platform principles P1–P8 and the ingest tiers live in `veritas-platform-architecture-design.md`. Analyzer engines live in `veritas-whole-program-analysis-design.md`. This document assumes both.

**Delivery status.** Implemented M8 publishes C++ fixed-point state and supports
optional file-based Souffle comparison. The approved, not-yet-delivered M8R
target adds the run/component/fact/witness contracts below. Its gate status is
tracked in the
[M8R bridge specification](../specs/milestones/m8r-souffle-wpa-remediation-design-spec.md).

---

# 2. "SummaryDB Is Not One Database"

The name "SummaryDB" refers to a **logical subsystem**, not a single physical database technology. Forcing every program-analysis workload into a single property-graph or single relational store is one of the classic ways to reach a scalability wall on 10 M+ LOC codebases.

VERITAS separates concerns:

```text
                     VERITAS SummaryDB
                             │
     ┌─────────────┬─────────┼─────────┬──────────────┐
     │             │         │         │              │
     ▼             ▼         ▼         ▼              ▼
 Object Store  Metadata  Fact Store  Graph Index  Dependency Index
  (immutable    (versions   (normalized   (CPG        (reverse
   summaries)   builds      relations)    projection)  invalidation)
                configs)
     │             │         │         │              │
     └─────────────┴─────────┼─────────┴──────────────┘
                             │
             ┌───────────────┴───────────────┐
             ▼                               ▼
        WPA Executors                 Evidence Cache
   (compiled Souffle / C++)         (materialized slices)
                                            │
                                            ▼
                                     History Store
                                   (semantic diffs)
```

The seven physical layers:

| Layer | Responsibility | Reference backend (V1) |
| --- | --- | --- |
| Object Store | canonical immutable summaries (CAS) | RocksDB |
| Metadata Store | versions, builds, configurations, ownership | SQLite |
| Fact Store | normalized semantic relations, current + historical | SQLite (later PostgreSQL) |
| Graph Index | CPG adjacency indexes for query | SQLite + in-memory graph |
| Dependency Index | reverse invalidation edges | SQLite |
| Evidence Cache | materialized slices per claim | RocksDB |
| History Store | semantic diffs across revisions | SQLite |
| WPA Executors | run-local execution only | compiled Souffle production adapter; C++ conformance or explicit emergency adapter |

The V1 stack is deliberately conservative. §9 defines the adapter interfaces so each layer can move to a different backend without touching the callers.

---

# 3. Design Goals of the SummaryDB

Five properties matter more than throughput:

1. **Semantic identity.** Every stored object has a stable identity independent of file layout, source-line drift, and macros. §5.
2. **Immutability.** Summaries never mutate; new content produces new addresses. §6.
3. **Precise invalidation.** Change detection operates on component-level semantic deltas, not object equality or file mtime. §7.
4. **Provenance.** Every derived fact is explainable through a finite provenance subgraph. See `veritas-evidence-ir-design.md` §30–31.
5. **Reader consistency.** Readers never observe a half-written revision. Publication is atomic at metadata bindings. §10.

Throughput is a downstream concern. If these five hold, backends can be swapped or scaled without rewrites.

---

# 4. Storage-Level Invariants

Storage-specific invariants layered on the platform's P1–P8:

| ID | Invariant | Reason |
| --- | --- | --- |
| S1 | Summary bytes are content-addressed and put-if-absent. | Deduplication, historical retention, parallel writers. |
| S2 | Metadata bindings are the only mutable state; object bytes are never overwritten. | Immutability of semantic content. |
| S3 | Publication is atomic at the metadata binding, not at the object write. | Reader consistency. |
| S4 | Each summary component has an independent semantic hash and an independent evidence hash. | Evidence-only churn does not invalidate analysis; semantic churn does. |
| S5 | The dependency index is keyed by `(producer_id, component)`, not by object. | Component-precise invalidation. |
| S6 | Historical rows are retained; the "current" binding is a swap, not a delete. | Time-travel and provenance replay. |
| S7 | Cross-run identities depend only on stable inputs: source revision, build variant, analyzer config, canonical body hash. | Reproducibility across hosts. |
| S8 | Backends implement the contracts in §9; no caller depends on a specific backend. | Backend swappability. |
| S9 | Writes are transactional at the boundary of one milestone-scoped operation (publish-summary, put-fact, index-rebuild). | Crash safety. |
| S10 | Function Summary IR is the durable WPA boundary; `relations.v2`, dense IDs, and engine-native tuples are run-local projections. | Portable, replayable derivations. |
| S11 | A WPA component becomes visible only after result, schema, stable-ID, and rooted-witness validation succeeds. | Failure atomicity; no partial replacement. |
| S12 | Fact Bus publication is complete and idempotent by canonical `(RunId, BatchId)` identity. | Safe retry and multi-sink delivery. |

---

# 5. Identity Model

"The function" has multiple meanings — declaration, build-variant instantiation, source body, analyzed content. VERITAS gives each meaning its own stable ID and derives higher IDs from lower ones.

## 5.1 IDs, layered

| ID | Meaning | Stable across | Changes when |
| --- | --- | --- | --- |
| `RepositoryID` | Logical repository identity. | Branches and revisions of one repo. | Root project identity changes. |
| `RevisionID` | Source snapshot identity. | Builds of the same commit / tree. | Git tree or source manifest changes. |
| `BuildVariantID` | Target, ABI, macros, compile flags, dependency closure. | Rebuilds with identical semantic build config. | Target, ABI, macro set, relevant headers, compile options change. |
| `FunctionSymbolID` | Logical function declaration/definition identity. | Source movement and line changes. | Name / signature / linkage / template specialization changes. |
| `FunctionVariantID` | Build-instantiated function. | Rebuilds with identical build variant. | `FunctionSymbolID` or `BuildVariantID` changes. |
| `FunctionBodyID` | Canonical body content hash. | Semantically-equivalent reformatting. | The canonical body content changes. |
| `AnalyzerRunID` | Analyzer version + configuration. | Reruns with identical config. | Analyzer version, configuration, or config hash changes. |
| `AnalysisRunID` | One reproducible WPA execution envelope. | Never merged across distinct manifests. | Revision, build, schemas, bundles, configurations, engine, or exact toolchain identity changes. |
| `FunctionSummaryID` | Semantic content of a summary object. | Semantically-equivalent recomputations. | Any input to the semantic content changes. |
| `SourceAnchorID` | Canonical `(file, line, column, canonical-decl-path)`. | Reformatting that preserves the canonical anchor. | Underlying canonical location changes. |
| `ProjectionID` | CPG projection identity. | Recomputations that produce the same projection. | Any input node/edge set changes. |

## 5.2 ID formation

Every ID has the shape `<kind>:<algo>:<hexdigest>`:

```text
function_symbol:sha256:e12f…
function_variant:sha256:8b70…
function_summary:sha256:4a30…
source_anchor:sha256:911d…
projection:sha256:6c05…
```

`FunctionSymbolID` derives from mangled name + canonical signature + linkage context. `FunctionVariantID` layers `FunctionSymbolID` + target + ABI + macro configuration + compile options + relevant headers. `FunctionSummaryID` layers `FunctionVariantID` + canonical body hash + analyzer version + analysis configuration.

Durable stable IDs and run-local dense IDs are separate identity domains.
Function, value, memory, call-site, and fact maps assign typed unsigned dense
IDs only within one `AnalysisRun`. Missing mappings, duplicate dense values,
stable-ID conflicts, or cross-domain use are validation failures.

## 5.3 What semantic identity buys

* **Deduplication.** Two builds producing the same body under the same variant share the same `FunctionSummaryID` — one CAS entry, many bindings.
* **Historical queries.** `get_summary_at_revision(F, R)` is a metadata lookup against the binding table; the object bytes are unchanged.
* **Parallel workers.** Content-addressed writes are inherently idempotent; workers never conflict on summary bytes.
* **Cross-branch reuse.** Bodies unchanged across branches never re-analyze.

## 5.4 What semantic identity does NOT do

* It does **not** collapse source-derived and bitcode-derived summaries. Those live in different `BuildVariantID`s by construction — different source-tree / module inputs — even when they name the same function. Cross-tier identity convergence is explicitly out of scope for V1.
* It does **not** ignore analyzer configuration. `SvfConfig`, alias-tier selection, and any analyzer knob participate in `AnalyzerRunID` and therefore in `FunctionSummaryID`.

---

# 6. Content-Addressed Object Store

Summary objects are immutable. Storing a summary is a `PutIfAbsent` on the object store:

```text
SummaryID
    ↓
serialized FunctionSummary  (Protobuf)
```

Then mutable metadata provides:

```text
(RepositoryID, RevisionID, BuildVariantID, FunctionVariantID)
    ↓
FunctionSummaryID
```

## 6.1 What lives in the object store

* `FunctionSummary` Protobufs, keyed by `FunctionSummaryID`.
* Successful immutable WPA component results, keyed by logical input and exact
  executor/toolchain identity, with per-run references.
* Evidence-case bodies (materialized slices), keyed by their canonical hash.
* Large run-local execution artifacts and projection snapshots retained for
  diagnostics or replay; they never replace summaries or canonical facts.

Everything mutable — bindings, dependency edges, index rows — lives in the metadata layer.

## 6.2 FunctionSummary shape

```text
FunctionSummary {
    identity
    build_variant
    source_hash
    body_hash
    signature
    calls[]
    memory_effects[]
    value_transfers[]
    range_facts[]
    alias_facts[]
    taint_transfers[]
    ownership_effects[]
    locks[]
    state_transitions[]
    assumptions[]
    unknowns[]
    dependencies[]
    provenance[]
}
```

A worked example:

```yaml
function:
  symbol: Decoder::decodeIE
  variant: ARM64_RELEASE_A

inputs:
  - packet
  - context

reads:
  - packet.len
  - context.state

writes:
  - context.state

calls:
  - validateIE
  - copyPayload

value_flow:
  - packet.len -> copyPayload.arg2

range:
  packet.len: { min: 0, max: 65535 }

state:
  - VALIDATING -> COMPLETE

unknowns:
  - semantics: vendorValidate()

dependencies:
  - function: validateIE
  - type: Packet
  - global: gConfig
```

---

# 7. Component Hash Matrix

Component-scoped hashes are the mechanism by which "semantic delta" is defined. Every `FunctionSummary` carries an independent hash per component, and separately a semantic hash vs an evidence hash.

## 7.1 Semantic vs evidence hashes

Every component records two digests:

```text
ComponentDigest {
    semantic_hash    (drives WPA / analysis invalidation)
    evidence_hash    (drives Evidence-case refresh only)
}
```

Rationale:

* Reformatting a comment changes neither.
* Renaming a local variable changes only the evidence hash (Evidence text may mention the name; analysis does not depend on it).
* Changing a range bound changes the semantic hash (WPA consumers of the range fact must re-run).
* Changing the source line of a body without changing canonical content changes only the evidence hash.

## 7.2 Components with independent digests

Every canonical component is hashed independently:

```text
Calls
MemoryEffects
ValueFlow
ControlFlow
Range
Alias
Taint
Ownership
Locks
State
Unknowns
Assumptions
Dependencies
Provenance
```

A downstream consumer of *only* `Range` is invalidated by a `Range.semantic_hash` change; a `Calls.semantic_hash` change alone leaves it untouched. This is the practical form of P5 (semantic-delta incrementality).

## 7.3 Reverse dependency index

The reverse index is keyed by `(producer_id, component)`:

```text
DependencyEdge {
    consumer
    producer
    kind:      CALL | TYPE_LAYOUT | GLOBAL_VALUE | ALIAS | SUMMARY | BUILD_CONFIG | MACRO | CONTRACT
    component: Calls | MemoryEffects | Range | …
}
```

Sensitivity tags on the edge control whether a change actually propagates:

```text
SEMANTIC        — trigger analysis rerun on the consumer
EVIDENCE_ONLY   — trigger evidence refresh only
IDENTITY        — trigger identity re-binding (no analysis rerun)
CONFIGURATION   — trigger reconfiguration path
```

`UsersOf(producer, component_delta)` returns the exact set of consumers to enqueue. `GetImpactSet(change)` gives the transitive closure for a broader diff visualization.

---

# 8. SummaryDB Read API

The read surface exposed to WPA, `veritas-query`, and Evidence Builder is deliberately semantic — never SQL, never a raw graph handle.

```text
getFunctionSummary(F)
getSummaryAtRevision(F, R)
getSummaryDelta(F, R1, R2)

getCallers(F)
getCallees(F)
getTransitiveCallers(F)

getValueFlow(src, dst)
getMayWrites(F)
getReads(F)
getAliases(V)              → { MustAlias | MayAlias | NoAlias | UnknownAlias }
getRanges(V)
getStateTransitions(F)

getDependencySet(F)
getImpactSet(Change)

explainFact(FactID)
getEvidenceSlice(Claim)
```

These become Review Agent tools in the post-backbone milestones. Query budgets (`QueryBudget`, `EvidenceQueryBudget`) are first-class parameters — any truncation is reported as an explicit `TruncationReason`, never silently applied.

---

# 9. Pluggable Backend Adapter Contract

The seven physical layers are exposed as small adapter interfaces. Implementations bind them to specific storage engines. Callers depend only on the interface (invariant S8).

## 9.1 Adapter surfaces

```cpp
namespace veritas::storage {

// Immutable content-addressed object store.
class ObjectStore {
 public:
  virtual ~ObjectStore() = default;
  virtual Status         PutIfAbsent(StableId id, absl::Span<const std::byte>) = 0;
  virtual StatusOr<Bytes>  Get(StableId id) const = 0;
  virtual bool             Contains(StableId id) const = 0;
};

// Versioned relational metadata: repositories, revisions, build variants,
// analyzer runs, translation units, function symbols/variants/bodies,
// summary publication bindings.
class MetadataStore {
 public:
  virtual ~MetadataStore() = default;
  virtual StatusOr<Txn>    BeginTxn() = 0;
  virtual Status           PublishSummary(const PublishSummaryReq&) = 0;
  virtual StatusOr<Row>    GetCurrent(BindingKey) const = 0;
  virtual StatusOr<Rows>   GetHistory(BindingKey) const = 0;
  // …
};

// Normalized derived-fact tuples with dual FactID (analyzer-scoped) and
// semantic_fact_hash (cross-revision equivalence).
class FactStore {
 public:
  virtual ~FactStore() = default;
  virtual Status           PutFacts(absl::Span<const Fact>, ProvenanceRef) = 0;
  virtual StatusOr<Rows>   Query(FactQuery) const = 0;
  virtual StatusOr<Explain> Explain(FactID, ExplainBudget) const = 0;
};

// CPG adjacency indexes for graph queries.
class GraphIndex {
 public:
  virtual ~GraphIndex() = default;
  virtual Status           Publish(ProjectionID, const ThinCpg&) = 0;
  virtual StatusOr<Trav>   Traverse(TraversalQuery, QueryBudget) const = 0;
};

// Reverse invalidation index, keyed by (producer_id, component).
class DependencyIndex {
 public:
  virtual ~DependencyIndex() = default;
  virtual Status           ReplaceCurrentDependencies(FunctionSummaryID,
                                                     absl::Span<const DependencyEdge>) = 0;
  virtual StatusOr<Users>  UsersOf(StableId producer, ComponentDelta) const = 0;
  virtual StatusOr<Impact> GetImpactSet(ChangeSpec) const = 0;
};

// Materialized Evidence slices, keyed by canonical case hash.
class EvidenceCache {
 public:
  virtual ~EvidenceCache() = default;
  virtual Status                   Put(EvidenceCaseID, const EvidenceCase&) = 0;
  virtual StatusOr<EvidenceCase>   Get(EvidenceCaseID) const = 0;
  virtual Status                   MarkStale(EvidenceCaseID, StaleReason) = 0;
};

// Cross-revision semantic diffs.
class HistoryStore {
 public:
  virtual ~HistoryStore() = default;
  virtual Status                   Record(RevisionPair, const SemanticDelta&) = 0;
  virtual StatusOr<SemanticDelta>  Get(RevisionPair) const = 0;
};

}  // namespace veritas::storage
```

The public headers under `include/veritas/storage/**` expose only these interfaces — no RocksDB, SQLite, or LMDB types leak across the boundary.

## 9.2 Reference bindings and their fit

The V1 stack maps each layer to the backend that fits best. Every layer has at least one additional candidate for future migration.

| Layer | V1 backend | Rationale | Later candidates |
| --- | --- | --- | --- |
| Object Store | **RocksDB** | high-throughput ordered KV, compaction-friendly for millions of small immutable blobs, put-if-absent trivially maps to `Merge`. | LMDB (mmap-based, smaller footprint); filesystem CAS (`objects/ab/cdef…` layout) for read-only distribution; S3-compatible for distributed CAS. |
| Metadata Store | **SQLite** | zero-ops embedded relational store, ACID transactions, indexes and joins for the binding tables. | PostgreSQL for team-scale / multi-writer deployments; DuckDB when read-analytics dominates. |
| Fact Store | **SQLite** | shares transactions with metadata; joins on `FactID` / `semantic_fact_hash` are inexpensive at the tuple scale of a single project. | PostgreSQL for team-scale; Parquet + DuckDB for offline analytics passes; ClickHouse for very large fact volumes. |
| Graph Index | **SQLite adjacency indexes + in-memory graph** | in-memory for hot traversals within a run; SQLite persistence keyed by `ProjectionID` for cross-run reload. | Kùzu / other embedded property-graph engines; Neo4j for large multi-user deployments (see §9.5). |
| Dependency Index | **SQLite** | small, transactional, joinable with metadata; sensitivity tags fit clean rows. | PostgreSQL for team-scale; Redis for hot lookups if latency becomes critical. |
| Evidence Cache | **RocksDB** | large blobs, high write throughput on Evidence rebuilds, canonical hash key. | S3-compatible object store for shared team caches. |
| History Store | **SQLite** | small tables per revision pair; joinable with metadata. | PostgreSQL for team-scale. |
| WPA execution | **compiled Souffle adapter** | Required normal production recursion over run-local `relations.v2`; C++ consumes byte-identical logical input only for conformance or explicit emergency use. | Another engine only after qualification preserves the logical-input, run-identity, witness, and failure contracts. |

## 9.3 In-memory backend

An in-memory implementation of every adapter is required. It exists for two reasons:

1. **Testing.** Unit and integration tests must be able to construct a full SummaryDB in memory without touching disk.
2. **Ephemeral / distributed workers.** A worker producing summaries for a shard can hold its intermediate metadata in memory and flush results to the shared object store; only the metadata authority runs a durable metadata backend.

The in-memory backend enforces the same invariants (S1–S12) as the durable backends.

## 9.4 Serialization

* Summary bodies and fact tuples use **Protobuf** as the canonical serialization.
* Canonical Protobuf encoding rules (`CanonicalEncode`) produce byte-identical output for semantically-identical inputs — sorted maps, sorted repeated fields where semantics allow, UTF-8, deterministic sub-message ordering.
* Textual formats (JSON) are for diagnostics only, not durable state.

## 9.5 What is deliberately NOT the SummaryDB

* **Neo4j.** VERITAS does not adopt Neo4j as the primary CPG store. The thin CPG projection is small enough to keep in-memory + SQLite adjacency indexes, and forcing all workloads into a property graph is one of the classic scaling pitfalls this document is explicit about avoiding (§2). Neo4j (or Kùzu) can layer on later for visualization or query experiments; it is not the source of truth.
* **A single relational store.** V1 uses SQLite for several layers, but the adapter contracts (§9.1) are what makes the split real — moving fact-store or graph-index onto a different engine is a swap, not a rewrite.
* **User-visible SQL.** No caller in the platform writes SQL. Storage-specific query languages are confined to the backend adapters (§9.1); the public read surface is the semantic API in §8.

---

# 10. Publication and Reader Consistency

## 10.1 Order of writes on `PublishSummary`

```text
1. ObjectStore.PutIfAbsent(FunctionSummaryID, serialized_summary)
2. MetadataStore.PutHistoricalRow(binding_key → FunctionSummaryID)
3. MetadataStore.SwapCurrentBinding(binding_key → FunctionSummaryID)
```

Only step 3 changes what a reader observes. Steps 1 and 2 are safe to retry, safe to interleave across workers, and never invalidate a prior read.

## 10.2 What atomicity means

A reader calling `GetCurrent(binding_key)` at any instant sees:

* either the previous `FunctionSummaryID` (a valid, complete summary), or
* the new `FunctionSummaryID` (a valid, complete summary).

The reader never sees an in-flight write. This holds even if step 3 races with a concurrent reader, because the swap is a single indexed row update in the metadata transaction.

## 10.3 Cross-layer atomicity

`ProjectionID` publication (M6) requires atomicity across the graph index and the summary bindings — the CPG must reference summaries that are still "current" at the moment the projection becomes visible. This is achieved by a single metadata transaction that co-swaps:

```text
Txn:
    GraphIndex.Publish(new_projection_id, thin_cpg)
    MetadataStore.SwapCurrentProjection(module_key → new_projection_id)
    MetadataStore.RebindSummaries(module_key → new_summary_set)
Commit
```

## 10.4 Failure and retry

* Any write may be retried; `PutIfAbsent` and `SwapCurrentBinding` are both idempotent for the same inputs.
* A crash between step 2 and step 3 leaves the CAS entry orphaned but semantically harmless (readers still see the prior current); a background GC pass reclaims orphans older than a retention threshold.
* A crash mid-transaction rolls back per the backend's ACID guarantees.

## 10.5 WPA component publication

WPA publication is stricter than file completion. A component must finish its
fixed point and pass output-schema, stable/dense mapping, duplicate-row, and
rooted-witness validation before it can replace the current component binding.
Timeout, crash, resource exhaustion, incompatible bundle, or any validation
failure records a durable diagnostic and an incomplete run but publishes no
replacement. The last successful result stays queryable as stale history for
the new revision/configuration and is never mixed with partial output.

Successful component reuse is content-addressed by
`(LogicalInputHash, EngineToolchainIdentity)`. The logical hash is
engine-neutral; exact executor provenance prevents C++ or a different Souffle
build from satisfying a production run. Every new `AnalysisRun` retains its own
manifest and result reference, so cache reuse never merges run history.

---

# 11. History and Semantic Diff

VERITAS's diff surface is the History Store. `veritas-diff <rev-a> <rev-b>` produces a semantic delta:

```text
Source changed functions:          214
Summary changed functions:          37
Call behavior changed:               4
Memory effects changed:             12
Range contracts changed:             9
State transitions changed:           3
WPA affected functions:            186
```

Under the hood, the History Store records per-revision-pair:

```text
SemanticDelta {
    added_summaries:      set<FunctionSummaryID>
    removed_summaries:    set<FunctionSummaryID>
    changed_summaries:    map<FunctionVariantID, ComponentDelta>
    added_facts:          set<FactID>
    removed_facts:        set<FactID>
    changed_facts:        set<FactID>            (same semantic_fact_hash, new FactID)
    unknown_resolved:     set<UnknownID>
    unknowns_introduced:  set<UnknownID>
}
```

Consumers include `veritas-diff`, PR-review tooling, and the Evidence Builder (which uses semantic deltas to flag stale Evidence cases).

---

# 12. Facts, Witnesses, and the Analysis Fact Bus

Every derived fact has a generic finite witness rooted in stable input fact
identities. Engines emit immediate edges over injectively encoded semantic keys;
the VERITAS canonicalizer maps them to stable facts, rejects orphaned/cyclic or
unclosed proofs, and selects one deterministic proof independently of engine
tuple order. Relation-specific reconstruction and Souffle-native tuple identity
are not durable provenance contracts.

The Provenance Store persists the selected witness as a DAG:

```text
ProvenanceNode {
    node_id:   FactID | SourceAnchorID | AnalyzerRunID | RuleID | AssumptionID
    kind:      base | derived | assumption | inferred | verified
}

ProvenanceEdge {
    consumer:   ProvenanceNode
    producer:   ProvenanceNode
    rule:       string       (e.g. "parameter-return propagation")
}
```

`Explain(FactID, ExplainBudget)` returns a finite provenance subgraph. Truncation, if any, is explicit — the returned subgraph carries a `truncated: bool` and a `TruncationReason`.

Cross-revision equivalence uses `semantic_fact_hash`; per-revision identity uses `FactID`. The same predicate under different provenance intentionally produces different `FactID`s, because the "reason a thing is true" is part of what makes it a distinct fact.

Full syntax and semantics are in `veritas-evidence-ir-design.md` §30–33.

## 12.1 Run and component records

An `AnalysisRun` manifest includes revision and build variant; summary and
relation schemas; rule and model bundles; SVF and WPA configurations; engine
identity; and exact engine/toolchain identity. Before production execution,
VERITAS parses the configured Souffle install-provenance manifest, requires
Souffle 2.5 source revision
`5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, hashes the configured executable,
and verifies that digest against the manifest. `EngineToolchainIdentity`
includes the verified manifest identity/content digest, source revision,
executable digest, generated-bundle digest, and generator/compiler/link
toolchain provenance. All run-manifest fields participate in `RunId`.

Each component record retains `LogicalInputHash`, `FixpointHash`,
`ExternalHash`, status, diagnostics, rooted input IDs, and the immutable result
reference. Witness-only change may change `FixpointHash` but not `ExternalHash`;
only an external semantic change schedules predecessor consumers.

## 12.2 M9 handoff and idempotency

Only a successful `WpaRunResult` can construct an immutable
`AnalysisFactBatch`. The batch carries expected component keys from the run
manifest, completed component records and hashes, rooted input fact IDs,
canonical derived facts, witnesses, and diagnostics. The Fact Bus rejects an
expected/completed mismatch or any witness leaf absent from the rooted-input
set.

Multi-sink delivery is idempotent at least once under canonical
`(RunId, BatchId)` identity. Per-sink progress is recorded, so retry after
partial fan-out cannot duplicate logical publication. `AnalysisFactBatch` is
the only M9 WPA input contract; M9 never accepts raw `FactTuple` output or
recomputes recursive facts to recover provenance.

---

# 13. Evidence Cache and Its Freshness

The Evidence Cache stores materialized Evidence cases keyed by canonical hash. Its role is not durability — Evidence can always be re-derived — but latency: rebuilding a large Evidence case from raw summaries costs seconds; retrieving it is milliseconds.

Freshness states:

```text
UNCHANGED
STALE
PARTIALLY_STALE
INVALID
REBUILT
```

The Dependency Index drives freshness: an Evidence case records the summaries and facts it cited (`depends_on { summary(...), fact(...), type_layout(...) }`), and any component-delta on those inputs marks the case `STALE` via the scheduler's `EVIDENCE_INVALIDATION` work items.

`STALE` Evidence is served with a warning; `INVALID` Evidence is refused and re-derived on next request.

---

# 14. Distributed Workers and the CAS

Because summaries are immutable and content-addressed, workers can generate them in parallel without coordinating on write. The CAS store is the natural coordination surface.

```text
                        Coordinator
                             │
             ┌───────────────┼───────────────┐
             ▼               ▼               ▼
         Worker 1        Worker 2         Worker N
          TU-A            TU-B             TU-Z
             │               │               │
             │  PutIfAbsent  │  PutIfAbsent  │  PutIfAbsent
             ▼               ▼               ▼
                     Summary Object Store  (CAS)
                             │
                             ▼
                Metadata authority  (single writer)
                             │
                             ▼
              Global WPA Coordinator  (SCC + fixpoint)
```

Rules:

* Object writes: many-writer, `PutIfAbsent` on `FunctionSummaryID`.
* Metadata writes: single-writer authority. Bindings are serialized through one process (V1) or one lease-holder (later).
* Dependency-index writes: same authority as metadata.

The single-writer authority is a V1 simplification. It can move to a lease-based multi-writer model later without changing any caller.

---

# 15. V1 Storage Footprint

Rough sizing for the M10B demo target (100K–1M LOC):

| Layer | V1 estimate |
| --- | --- |
| Object Store (RocksDB) | 200 MB – 4 GB (summary bodies + Evidence bodies) |
| Metadata (SQLite) | 20 MB – 400 MB |
| Fact Store (SQLite) | 100 MB – 2 GB |
| Graph Index (in-memory + SQLite) | 40 MB – 800 MB |
| Dependency Index (SQLite) | 20 MB – 400 MB |
| Evidence Cache (RocksDB) | 100 MB – 2 GB |
| History Store (SQLite) | 20 MB – 400 MB |

These numbers exist to bound the V1 scaling target — a laptop-class machine analyzing a million-line C/C++ codebase. Team-scale deployments (10M+ LOC) migrate the fact and metadata stores to PostgreSQL and shard the object store; the adapter contracts (§9) make that a swap rather than a rewrite.

---

# 16. Handoff to Evidence IR

The SummaryDB does not construct Evidence prompts. Its role is to answer the Evidence Builder's semantic queries:

```text
getValueFlow(packet.len, memcpy.size)
getRange(packet.len)
getObjectCapacity(dst)
findDominatingChecks(memcpy)
getCallPath(entry, memcpy)
getRelevantSummaries(path)
explainFact(fact_id)
```

The Evidence Builder assembles the answers into an `EvidenceCase`. See `veritas-evidence-ir-design.md` for the full IR.

---

# 17. Reading Order

Start with `veritas-platform-architecture-design.md` for the platform pipeline and principles.
Then `veritas-whole-program-analysis-design.md` for how the analyzer engines produce the objects this document stores.
Then this document for identity, hashing, storage, and the backend adapter contract.
Then `veritas-evidence-ir-design.md` for the IR the Agent consumes.

Milestone specs and implementation plans:

* M2 (`docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md`) — identity and metadata store.
* M3 (`docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md`) — Summary IR + CAS.
* M7 (`docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md`) — dependency index + scheduler.
* M8 (`docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md`) — Soufflé WPA execution.
* M8R (`docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md`) — production-Souffle remediation, Fact Bus, and M9 gate.
* M9 (`docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md`) — provenance-aware fact store + explain API.
