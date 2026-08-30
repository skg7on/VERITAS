# Extending SummaryDB and Evidence IR

This guide is for developers adding native analyses, whole-program relations,
external providers, Evidence producers, or Agent-facing review tools. It maps
the current implementation and the approved target boundaries, then gives a
change workflow that preserves VERITAS identity, provenance, uncertainty,
incrementality, and publication rules.

## 1. Start from the non-negotiable model

VERITAS deliberately separates three representations:

```text
Function Summary IR
  immutable externally visible function semantics

SummaryDB
  logical storage/query/invalidation subsystem over multiple physical layers

Evidence IR
  claim-oriented, typed, provenance-preserving input to Agents and verifiers
```

The feedback loop is equally important:

```text
deterministic analysis -> Evidence IR -> Agent hypothesis
                                       -> proof obligation
                                       -> deterministic verifier
                                       -> authoritative state transition
```

An Agent may propose `INFERRED` content. It may not write `MUST`,
`MUST_NOT`, `VERIFIED_DEFECT`, or `VERIFIED_SAFE` without an authority-bearing
deterministic verifier result.

All extensions inherit these practical rules:

1. Never mutate semantic objects; publish new content and swap bindings.
2. Build IDs only through the canonical, domain-separated identity APIs.
3. Keep semantic and evidence-only hashes separate.
4. Make absence, incompleteness, ambiguity, and truncation explicit.
5. Give every derived fact a finite, rooted provenance witness.
6. Bound every graph/provenance query and report budget exhaustion.
7. Keep native and external provider authority distinct.
8. Make publication atomic at the visibility boundary.

### 1.1 Treat SummaryDB as seven logical layers

`SummaryDB` is the name of the subsystem, not a promise that every record
lives in one database engine:

| Logical layer | Role | Current-tree delivery |
| --- | --- | --- |
| Object Store | Content-addressed immutable summaries and, in the target, provider artifacts and Evidence slices | RocksDB summary CAS is implemented |
| Metadata Store | Repositories, revisions, builds, identities, configurations, bindings, and publication state | SQLite implementation is present |
| Fact Store | Normalized current and historical native/provider relations plus run bindings | Run-local fact types exist; durable M9 publication is a target |
| Graph Index | Native CPG and provider-projection adjacency/query indexes | Native thin CPG is implemented; provider projections are M12 targets |
| Dependency Index | Reverse component dependencies and bounded impact traversal | Native summary dependencies are implemented |
| Evidence Cache | Materialized, snapshot-pinned claim slices | M10B/M10C target |
| History Store | Prior bindings and semantic/component deltas | Summary history/delta schema exists; provider and Evidence history expand later |

WPA executors sit beside these layers: they consume immutable summaries and
run-local projections, then publish validated results through the storage
boundary. Souffle or an engine-native graph is therefore never itself the
SummaryDB.

## 2. Current code map

The paths below exist now unless marked as a target.

| Concern | Primary paths | Responsibility |
| --- | --- | --- |
| Project ingest | `src/build/`, `include/veritas/build/` | Resolve project root, normalize compilation database, derive manifest/context IDs |
| Program IR | `src/analysis/llvm/`, `src/analysis/pipeline/` | Clang CodeGen, module linking, origin mapping, local extraction |
| Required pointer/value-flow analysis | `src/analysis/svf/` | In-process pinned SVF session, fact mapping, summary-v2 merge |
| Durable semantic values | `src/analysis/semantic/`, `include/veritas/analysis/semantic/` | Memory, alias, epistemic, and model value types |
| Function Summary IR | `proto/veritas/summary/`, `src/summary/`, `include/veritas/summary/` | v1/v2 schema, builders, canonicalization, component hashes, version-neutral artifacts |
| Summary persistence | `src/summarydb/`, `include/veritas/summarydb/` | RocksDB CAS, SQLite metadata/schema, current bindings, deltas, reverse dependencies |
| Native CPG | `src/cpg/`, `src/analysis/cpg/`, `include/veritas/cpg/` | Thin projection, canonical identity, persistence, bounded queries |
| Run-local facts | `src/facts/`, `include/veritas/facts/` | `relations.v2`, base facts, dense IDs, Souffle export/runner, rooted proof reconstruction |
| Whole-program analysis | `src/wpa/`, `include/veritas/wpa/` | Call/SCC graphs, C++ fixpoint, state persistence, propagation |
| CLI tools | `src/tools/` | Analyze and query, v1 diff/impact, and a version-only explanation skeleton |
| Evidence Builder | `include/veritas/evidence/`, `src/evidence/` | **M10B target; not present** |
| Evidence IR implementation | `proto/veritas/evidence/`, `include/veritas/evidence/`, `src/evidence/` | **M10C target; not present** |
| Provider substrate/importers | planned `include/veritas/provider/`, `src/provider/` families | **M12 target; not present** |

The current source analysis entry point is
`ProjectAnalyzer::AnalyzeProject`. It resolves the project, builds the private
module, extracts and merges summary.v2 facts, projects the native CPG, and
publishes both through `ProjectPublicationCoordinator`.

## 3. Work with version-neutral summaries

The current native pipeline publishes `summary.v2`, but historical databases
may contain v1 objects. Storage and new tools should therefore use:

```cpp
using SummaryArtifact =
    std::variant<summary::v1::FunctionSummary,
                 summary::v2::FunctionSummary>;
```

The supported helpers are:

```cpp
summary::SchemaVersion(artifact);
summary::Identity(artifact);
summary::SerializeSummaryArtifact(artifact);
summary::ParseSummaryArtifact(schema_version, bytes);
summary::ComputeFunctionSummaryId(artifact);
summary::ComputeComponentDigests(artifact);
```

Do not inspect the variant index to infer a schema version, parse v2 bytes as
v1, or use `SummaryRepository::ListCurrentSummaries` in a consumer that must
handle current v2 output. Use `ListCurrentSummaryArtifacts`.

## 4. Add or refine a native local analysis

A native local analysis contributes externally visible function semantics.
Use this path when VERITAS itself computes the result from the private LLVM
module.

### 4.1 Define the semantic value first

Put durable C++ semantics in `veritas::analysis::semantic`, not in an LLVM,
SVF, Protobuf, SQLite, or CLI type. Define:

- a closed enum or tagged value with an invalid/unknown state;
- stable validation rules;
- conversion to/from the wire schema;
- equality and deterministic ordering; and
- the exact epistemic state and provenance reference carried by each fact.

Never use a diagnostic string as a semantic value when the domain can be
typed. Memory effects in summary.v2 are the model: `MemoryLocation` separates
an abstract object, access path, byte range, and stable `MemoryRef` ID.

### 4.2 Extract into function-local facts

Extend `FunctionLocalFactsV2` or a successor typed local-fact structure. The
extractor must:

- derive every `ValueRef`, `MemoryRef`, `CallSite`, and function reference via
  the canonical ID boundary;
- preserve unknown/partial results instead of dropping them;
- attach stable provenance references;
- remain independent of native pointer addresses and iteration order; and
- return `Status`/`StatusOr<T>` for all failures because VERITAS builds without
  exceptions or RTTI.

If an SVF fact refines an LLVM-local draft, merge by the stable owning
`FunctionVariantID`. Never correlate stages through `llvm::Value*` addresses
after the stage boundary.

### 4.3 Extend the summary schema and builder

For a new durable component:

1. Add a typed Protobuf field using a new field number; never reuse or
   reinterpret an existing number.
2. Register its component kind if it needs independent invalidation.
3. Validate ID kinds and value invariants before serialization.
4. Sort semantically unordered repeated records by canonical bytes.
5. Preserve semantically ordered sequences in their defined order.
6. Pin nondeterministic lifecycle values outside semantic bytes.
7. Compute independent semantic and evidence hashes.
8. Add version-neutral `SummaryArtifact` handling before publishing the new
   schema.

Semantic hashes include data that changes analysis meaning. Evidence hashes
include source anchors, diagnostic names, and provenance details that can
refresh explanations without recomputing WPA.

### 4.4 Publish through the coordinator

Native analysis must hand a complete summary set and matching native CPG to
`ProjectPublicationCoordinator`. Publication is:

```text
validate summary/CPG correspondence
  -> PutImmutableSummaryArtifacts in RocksDB
  -> begin shared SQLite transaction
  -> stage current summary bindings
  -> stage native CPG projection and current binding
  -> commit
```

Do not call `MetadataStore` piecemeal from an analyzer. The coordinator ensures
readers cannot observe a CPG referring to summaries that are not current.

### 4.5 Register dependencies

Add reverse edges at component granularity:

```text
consumer summary/component
  -> producer summary/component
  -> dependency kind
  -> sensitivity
```

Choose sensitivity deliberately:

| Sensitivity | Schedule |
| --- | --- |
| `kSemantic` | Recompute semantic consumer when producer semantic hash changes |
| `kEvidenceOnly` | Refresh evidence when evidence hash changes |
| `kIdentity` | Rebind identity without semantic recomputation |
| `kConfiguration` | Follow the configuration-change path |

A consumer of `Range` must not be invalidated by an unrelated `Calls` change.

### 4.6 Native-analysis test matrix

At minimum, test:

- canonical ID kind validation and malformed-ID rejection;
- input-order independence;
- semantic versus evidence-only digest isolation;
- unknown and epistemic preservation;
- v1 historical read plus new-schema read;
- atomic batch publication and rollback;
- current summary/CPG correspondence;
- component-scoped dependency impact; and
- an end-to-end project fixture through `ProjectAnalyzer`.

## 5. Add a whole-program relation or domain

Whole-program execution consumes summaries, not raw source or an engine-native
graph. The implemented fact builder and fixpoint path is currently v1-only:

```text
summary::v1::FunctionSummary set
  -> SummaryFactBuilder
  -> typed relations.v2 facts
  -> stable-to-dense ID maps for one AnalysisRun
  -> C++ fixpoint and/or Souffle executor
  -> canonical facts plus immediate proof inputs
  -> convergence/component state
```

The native analyzer currently publishes v2 summaries, so no released
orchestration connects its output to this v1-only WPA path. Before extending
WPA for current analyzer output, add a version-neutral `SummaryArtifact`
boundary to fact construction and domain execution; do not down-cast v2 or
reinterpret its bytes as v1.

### 5.1 Relation requirements

A new relation needs:

- a stable registered name and fixed typed column schema;
- an arity and cell-domain validator;
- explicit epistemic representation;
- a canonical `FactID` independent of run, dense IDs, tuple order, engine, and
  witness;
- conversion from summaries/base facts;
- monotone join/weakening semantics where recursion uses it;
- finite immediate proof inputs for every derived row; and
- deterministic duplicate and alternate-proof selection.

Engine-native tuple IDs and dense integers are run-local conveniences. They
must never leak into durable facts, Evidence, or provider identities.

### 5.2 Recursion and external visibility

Recursive domains execute over deterministic SCCs. The component's
`FixpointHash` may change when selected proof structure changes, while its
`ExternalHash` changes only when consumer-visible semantics change. Schedule
predecessors only for external semantic changes.

Budget exhaustion, timeout, or engine failure must produce explicit incomplete
state. Do not publish a partial replacement or silently fall back from one
engine identity to another.

### 5.3 M9 boundary

The current repository has run-local fact types and proof reconstruction, but
the durable M9 Fact/Provenance Store and `AnalysisFactBatch` publication
boundary are not implemented. When adding durable publication, follow the M9
specification rather than writing raw tuple rows directly into SQLite.

## 6. Add an external provider adapter

Use the Tier 3 path when another analyzer supplies observations rather than an
LLVM module for VERITAS to analyze.

### 6.1 Keep four layers separate

```text
ProviderReader
  provider syntax -> bounded private raw records

ProviderContextValidator
  bind artifact to an existing repository/revision/build snapshot

ProviderNormalizer
  raw records -> provider-neutral entities/relations/facts/unknowns

ProviderPublisher
  validate and atomically publish graph + facts + provenance + binding
```

Only private reader code may mention GraphSON, GraphML, XML, TinkerPop, Joern,
PhASAR, or another provider's native model. Neither `EvidenceBuildInput` nor a
public SummaryDB semantic query may expose those types.

### 6.2 Preserve three identity levels

Keep these distinct:

```text
ProviderRecordID     exact provider record in an artifact
ProgramOccurrenceID exact normalized source occurrence in a context
ProgramEntityID     semantic VERITAS identity when unambiguously resolved
```

Provider ordinals and raw pointers are provenance, not semantic identity. If
bridging is ambiguous, publish an external stable ID plus a bounded
`UnresolvedIdentity` observation with sorted candidates. Never guess.

### 6.3 Declare capabilities, assumptions, and unknowns

Provider availability is not completeness. A provider run must declare which
overlays/analyses were present, their configuration and budgets, and any
external-call or type-recovery assumptions. Missing or partial capabilities
become explicit unknowns.

External semantic facts enter as `INFERRED`; explicit provider premises enter
as `ASSUMED`. Validation rejects `MUST`. Agreement between providers can create
`Corroborates`, but cannot upgrade authority.

### 6.4 Publish atomically

The M12 target visibility transaction includes:

```text
provider artifact/run/capability/projection metadata
provider-neutral graph and adjacency indexes
canonical facts and run bindings
assumptions and rooted witnesses
provider component history/deltas
Evidence/query dependency invalidation
current provider binding
```

Native summaries, the M6 projection, native reverse dependencies, SCCs, and
WPA are outside this transaction. Provider changes invalidate only consumers
that explicitly selected those provider components.

### 6.5 Treat provider files as untrusted

Readers need limits for total bytes, records, properties, strings/lists,
nesting, and diagnostics. Disable GraphML DTDs and external entities; perform
no network retrieval, embedded path dereference, plugin execution, or provider
runtime invocation. Detect input mutation during read and publish nothing on
any failure.

The [generation manual](summarydb-generation-manual.md) gives separate Joern
and PhASAR workflows. The
[M12 design](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md)
is normative for the common provider substrate and Joern adapter.

## 7. Build Evidence Builder and Evidence IR producers

This section describes the approved M10B/M10C target. No corresponding public
implementation exists in the current tree.

### 7.1 Evidence query services are semantic

Expose operations such as:

```text
GetValueFlow
GetRanges
GetCapacities
GetAliases
GetUnknowns
GetDominatingChecks
Explain(run_id, fact_id)
BuildEvidenceInput
```

Every query must pin one immutable program snapshot before reading:

```text
repository + revision + build variant + analysis run
+ native projection + fact snapshot
+ ordered selected provider run/projection bindings
```

Every result carries members, supporting and contradicting facts, unknowns,
provenance references, examined counts, completeness, and stable truncation
reasons. A selected provider's positive contradiction or unresolved in-scope
candidate prevents an unqualified negative conclusion.

### 7.2 Keep `EvidenceBuildInput` typed and immutable

M10C consumes a typed handoff, not diagnostic JSON and not a second set of
database queries. It contains the claim seed, semantic slices, query-completion
facts and run bindings, selected witnesses, provenance graph, and snapshot
identity.

The builder may assemble; it may not re-run reachability, alias, range,
dominance, or provenance analysis. Mixed runs or revisions are errors.

### 7.3 Construct and validate `EvidenceCase`

The initial case holds:

```text
program context, primary claim, entities, relations, paths, facts,
assumptions, hypotheses, unknowns, constraints, provenance,
proof obligations, summary references, dependencies, omissions,
verification state
```

Required validation includes unique and resolved references, typed predicates,
one explicit epistemic state per fact, provenance for derived facts, connected
paths, visible truncation, immutable summary references, hypothesis isolation,
and one coherent program context.

Canonical identity is computed from semantic bytes, not EIR-T whitespace,
Protobuf wire ordering, comments, or diagnostic labels. EIR-T, Protobuf, and
diagnostic EIR JSON must round-trip to the same `EvidenceID`.

All three Evidence levels are projections of that one case, not independent
schemas:

| Level | Intended contents |
| --- | --- |
| L0 | Program context, claim, severity/state, primary support/contradiction/path, and the primary unknown or omission |
| L1 | L0 plus the causal slice: flow/call paths, control/dominance, memory, summary edges, constraints, assumptions, provenance roots, unknowns, and truncation |
| L2 | All already-available detailed proof material, such as expanded provenance DAGs, alias sets, constraints, and analyzer expressions |

An L0/L1 reference to omitted detail remains expandable. Requesting L2 means
“include all available detail”; it does not authorize the builder to run a
proof engine or claim completeness.

## 8. Design an Agent-facing analysis tool

An Agent tool should expose one bounded semantic operation, not a database or
source-repository escape hatch. A good tool response has:

```text
snapshot identity
typed result members
supporting evidence
contradicting evidence
unknowns and omissions
completeness and truncation
provenance roots
expandable references
```

Avoid these interfaces:

- arbitrary SQL over `metadata.db`;
- raw RocksDB reads;
- unbounded “give me the whole CPG” calls;
- booleans that conflate false, unknown, and truncated;
- source text copied without a stable source-anchor reference;
- provider-native IDs or JSON exposed as canonical evidence; and
- tools that allow the Agent to set authoritative verification state.

The Agent may request refinement, rank candidates, infer a contract, or create
a typed hypothesis and proof obligation. A verifier adapter owns the later
state transition and must return its exact tool/config/input identity plus a
checkable result.

Continue with the
[Agent review tutorial](tutorial-agent-code-review.md) for a worked design.

## 9. Definition of done for an extension

An analysis or provider extension is not complete until it demonstrates:

1. **Identity:** semantic equivalents reproduce IDs; meaningful changes alter
   the correct ID/component only.
2. **Validation:** malformed IDs, values, schemas, and cross-context references
   fail with typed status.
3. **Epistemics:** no path silently strengthens uncertainty or external
   authority.
4. **Provenance:** every derived fact has a finite rooted witness.
5. **Completeness:** empty, complete-empty, truncated-empty, and unknown are
   distinguishable.
6. **Incrementality:** only consumers of the changed semantic component are
   invalidated.
7. **Atomicity:** injected failure at each write stage leaves the prior binding
   current.
8. **Determinism:** record/input order, paths, and native addresses do not
   affect canonical output.
9. **Compatibility:** historical schema versions remain readable or fail with
   an explicit unsupported-version status.
10. **Boundaries:** public headers and Evidence inputs contain no provider,
    storage-backend, LLVM, or SVF implementation types unless that boundary
    explicitly owns them.
11. **Budgets:** scale limits and truncation are tested, including exact-budget
    completion.
12. **End to end:** one isolated project/provider fixture reaches the intended
    query or Evidence boundary with required and forbidden outputs asserted.

Use the architecture and milestone specs as the source of truth for any field,
identity formula, or authority decision not fixed by this guide.
