# M12 Joern CPG SummaryDB Importer Design Spec

**Status:** Draft for written review; architecture approved 2026-08-24
**Milestone:** M12A–M12C; M12D is a separately scoped PhASAR follow-up
**Depends on:** M2 identity, M3 object store, M6 graph storage/query foundations,
M9 fact/provenance publication, and M10B for Evidence integration
**Related:** `m11-m12-summarydb-ingest-adapters-design-spec.md`

---

# 1. Purpose

VERITAS accepts Joern CPG exports as an optional external evidence provider.
Joern does not replace VERITAS's native Clang/LLVM, SVF, member-aware memory,
value-flow, pointer/alias, Summary IR, or WPA pipelines. The importer exists to
make Joern's structural and semantic observations available through the same
SummaryDB-backed symbolic query surface as native VERITAS results.

V1 directly parses the raw whole-graph formats emitted by Joern:

```text
GraphSON
GraphML
```

V1 does not require a Joern-side exporter, a live Joern server, FlatGraph,
OverflowDB, Neo4j, Arrow, Parquet, or a VERITAS-specific interchange bundle.
Those may be added later without changing the provider-neutral SummaryDB
contracts defined here.

The load-bearing distinction is:

```text
unified representation != equal authority
```

Native and imported observations may describe the same program entity or
relation. They retain separate producer, run, capability, epistemic,
assumption, and provenance records. Joern observations may corroborate,
contradict, seed, and explain evidence, but they never silently overwrite or
promote native facts.

Canonical terminology:

```text
SummaryDB Unified Symbolic View
    pinned query view over native + selected provider state

ProviderProgramGraph
    provider-neutral normalized graph product of one importer

M6 ThinCpg
    native VERITAS projection only
```

The brainstorm name “VPG” is not introduced as a fourth durable IR or an
eighth physical database. Its useful provider-neutral graph concept is realized
by `ProviderProgramGraph` plus the SummaryDB unified view.

---

# 2. Architectural Decisions

This specification fixes the following decisions.

1. **Joern is optional.** A repository with no Joern projection behaves exactly
   as it did before M12.
2. **VERITAS parses GraphSON/GraphML directly.** These formats are provider
   inputs, not VERITAS's durable canonical serialization.
3. **SummaryDB is the integration point.** No parallel VPG database or
   Joern-specific query service is introduced.
4. **M6 stays native.** Imported graphs do not mutate the M6 `ThinCpg`, its
   `ProjectionID`, or its atomic summary publication.
5. **Provider projections are first-class SummaryDB history.** The Graph Index
   stores immutable provider-neutral normalized projections alongside the
   native projection under distinct bindings.
6. **Graph and facts are different products.** Structural topology is stored in
   the Graph Index. Only registered semantic mappings publish M9 facts.
7. **External facts have an epistemic floor.** Joern results enter as
   `INFERRED` or `ASSUMED`, never `MUST`.
8. **Imported absence is open-world.** Missing Joern nodes, edges, flows,
   targets, or checks never establish negative evidence.
9. **Publication is atomic and idempotent.** A failed import advances no
   provider binding and publishes no partial graph, facts, or witnesses.
10. **M10B remains provider-neutral.** Evidence Builder reads a pinned combined
    SummaryDB snapshot and never parses GraphSON/GraphML or exposes Joern-native
    types.

---

# 3. Relationship to Existing Milestones

## 3.1 M11 external LLVM IR adapter

M11 accepts `.bc` and `.ll` at module acquisition:

```text
bitcode -> ProgramIr -> VERITAS analysis -> native summaries + native M6 CPG
```

Although the module is externally supplied, Clang CodeGen is the only skipped
stage. SVF, local extraction, Summary IR, M6, WPA, and provenance remain
VERITAS-owned. M11 output therefore stays on the native authority path and does
not use M12's external-provider epistemic floor.

## 3.2 M6 native thin CPG

M6 owns the compact native projection derived from live `ProgramIr` and
completed native summaries. M12 neither extends M6's closed node/edge schema nor
co-publishes Joern data with native current summary bindings.

SummaryDB may expose a unified query view over one native projection and zero
or more provider projections. That view is above M6; it does not turn provider
data into M6 content.

## 3.3 M9 fact and provenance store

M9's `AnalysisFactBatch` remains the only accepted WPA publication contract.
M12 adds `ExternalFactBatch` as a second typed publication contract for a
validated provider projection. Both use the same canonical facts, run
bindings, rooted witnesses, atomicity, and idempotency rules.

## 3.4 M10B Evidence Builder

M12C extends the input snapshot pinned by M10B with selected provider
projection IDs and a provider-binding fingerprint. It does not change the
claim-oriented M10B/M10C handoff or allow provider-specific records to bypass
`EvidenceBuildInput`.

---

# 4. End-to-End Architecture

```text
Joern GraphSON / GraphML
          |
          v
  bounded format reader
          |
          v
   RawProviderGraph
          |
          v
 schema + snapshot validation
          |
          v
 identity resolution + normalization
          |
          +----------------------+----------------------+
          |                      |                      |
          v                      v                      v
 ProviderProgramGraph      ExternalFactBatch     ProviderExtensionObservation
          |                      |                      |
          +----------------------+----------------------+
                                 |
                                 v
                              SummaryDB
    +----------------+----------------+----------------+---------------+
    |                |                |                |               |
    v                v                v                v               v
 Object Store    Metadata Store   Graph Index     Fact/Provenance   History
 artifact        provider runs    normalized      semantic facts    bindings
 descriptor      capabilities     topology        + witnesses       + deltas
    |                |                |                |               |
    +----------------+----------------+----------------+---------------+
                                 |
                                 v
                UnifiedProgramGraphQuery / EvidenceQueryService
                                 |
                                 v
                     EvidenceBuildInput -> Evidence IR
```

The unified symbolic view is a SummaryDB read contract. It is not a single
materialized graph that discards producer boundaries.

---

# 5. Import Context and Input Contract

An import binds to one existing VERITAS program context:

```text
RepositoryID
RevisionID
BuildVariantID
```

The CLI reconstructs the expected context from `--project` and opens the
existing database at `--output`. Publication requires an existing compatible
native binding. Joern metadata such as root path, language, frontend, optional
source hash, and overlays is evidence for correspondence, but it is not allowed
to create or select a VERITAS revision/build identity by itself.

Snapshot correspondence has an explicit basis:

```text
ProviderContextBinding {
    repository_id
    revision_id
    build_variant_id
    basis = VERIFIED |
            SOURCE_VERIFIED_BUILD_ASSERTED |
            USER_ASSERTED
    provider_source_fingerprint?
    provider_build_fingerprint?
    veritas_source_fingerprint
    veritas_build_fingerprint
    assumption_ids[]        // required for every unverified dimension
}
```

`VERIFIED` requires recognized provider source and build-configuration
fingerprints that can be compared with the VERITAS source/build snapshot. A
provider root pathname, an unrecognized opaque hash, or source equality without
compatible compilation/frontend configuration is not sufficient. When the raw
export lacks either verifiable dimension, the default is
`FailedPrecondition`. The operator may opt in with
`--accept-unverified-context`; the importer then records whether only the build
or the entire source/build correspondence is asserted and creates stable
assumptions inherited by every imported observation and fact. An actual
fingerprint mismatch is always rejected and cannot be overridden.

Detached provider imports are rejected in V1. This prevents accidental fusion
of a Joern graph generated from one checkout with native facts generated from
another.

The supported input contract is specifically:

```text
joern-export --repr=all --format=graphson
joern-export --repr=all --format=graphml
```

The importer does not accept arbitrary Gremlin GraphSON, generic application
JSON, per-method DOT output, a live Joern workspace, or `cpg.bin`.

Format detection examines content. A filename extension is only a hint. An
explicit `--format` that disagrees with content is rejected.

---

# 6. Provider Metadata and Content Identity

## 6.1 Provider artifact

```text
ProviderArtifact {
    artifact_id
    provider = JOERN
    format = GRAPHSON | GRAPHML
    content_digest
    byte_count
    retained_blob_ref?
    diagnostic_origin_path?
}
```

`diagnostic_origin_path` is never an identity input. The Object Store always
retains the immutable descriptor and digest. Retaining the full raw artifact is
configurable because whole-program graph exports can be large.

## 6.2 Provider run

```text
ProviderRun {
    provider_run_id
    artifact_id
    raw_graph_id
    repository_id
    revision_id
    build_variant_id
    joern_version?            // UNKNOWN when the raw export omits it
    cpg_schema_version
    frontend
    language
    ordered_overlays
    importer_version
    mapping_version
    configuration_digest
    capability_set_id
    context_binding
    status
    diagnostics
}
```

Lifecycle status and diagnostics are excluded from identity.
Raw Joern exports are not required to carry the Joern distribution version.
When absent, the importer records an explicit unknown producer-version field;
it does not guess from the CPG schema version. Compatibility is gated by the
observed export dialect and CPG schema adapter.

## 6.3 Provider projection

```text
ProviderProjection {
    provider_projection_id
    provider = JOERN
    repository_id
    revision_id
    build_variant_id
    graph_schema_version
    mapping_version
    capability_set_id
    assumption_set_digest
    canonical_graph_digest
    component_digests
}
```

The three primary identities are deliberately distinct:

```text
ProviderArtifactID
    H("provider.artifact.v1", provider, format, exact input bytes)

ProviderRunID
    H("provider.run.v1", provider, artifact_id, context binding,
      schema/frontend/overlays, importer/mapping/configuration versions,
      capabilities, assumptions)

ProviderProjectionID
    H("provider.projection.v1", provider, repository/revision/build,
      graph schema, mapping version, normalized graph,
      capabilities, assumptions)
```

Equivalent GraphSON and GraphML exports may have different artifact/run IDs and
the same normalized projection ID.

---

# 7. Three-Level Program Identity

Joern numeric IDs are never VERITAS semantic identities.

```text
ProviderRecordID
    exact provider node/edge in one artifact

ProgramOccurrenceID
    exact normalized source occurrence in one revision/build

ProgramEntityID
    semantic VERITAS entity, when resolvable
```

## 7.1 Provider record identity

```text
ProviderRawGraphID = H(
    "provider.raw_graph.v1",
    provider,
    canonical typed metadata/nodes/edges
)

ProviderRecordID = H(
    "joern.record.v1",
    provider_artifact_id,
    record_kind,
    typed_provider_record_id
)
```

Raw-graph canonicalization excludes serialization format, record/property
order, source-record locators, and artifact/run identity. Equivalent
GraphSON/GraphML exports therefore have the same `ProviderRawGraphID`.
Node records use their typed Joern node ID, label, and canonical typed
properties. Edge records use their typed endpoints, label, and canonical typed
properties; export-format-only edge IDs are excluded. Exact duplicate edge
records are canonicalized as a multiset before semantic normalization, while
the run-specific raw records remain individually available through provenance.
The schema adapter also excludes properties classified as diagnostic,
provenance-only, or unknown extension payloads from this ID. Absolute roots,
timestamps, raw `CODE`, and host/exporter details therefore cannot make the
same provider graph appear to be a different graph instance.

The original typed Joern ID may be retained inside provenance, but it never
appears in a native semantic ID, graph endpoint, or query key.

## 7.2 Occurrence identity

When sufficient information exists:

```text
ProgramOccurrenceID = H(
    "program.occurrence.v1",
    revision_id,
    build_variant_id,
    normalized_project_relative_path,
    enclosing_function_id,
    normalized_source_span,
    canonical_node_kind,
    structural_discriminator
)
```

The structural discriminator resolves repeated expressions at the same source
location without using provider iteration order. It is derived from normalized
AST ancestry and sibling semantics. If a stable discriminator cannot be
formed, the occurrence remains unresolved.

## 7.3 Entity resolution

Existing VERITAS identities are reused when stable bridge inputs agree:

```text
mangled name + normalized signature + build context -> FunctionVariantID
project-relative path + normalized source span      -> SourceAnchorID
function + parameter position/type                  -> parameter ValueRef
function + canonical call occurrence                -> CallSiteID
base object + field path                             -> MemoryRef
```

Ambiguous matches do not pick a winner. They produce a provider-scoped external
entity plus an `UnresolvedIdentity` observation containing sorted candidates.

Unresolved IDs still use the canonical VERITAS stable-ID form, but remain
explicitly provider-local:

```text
ExternalEntityID = H(
    "external.entity.v1",
    provider,
    provider_raw_graph_id,
    typed_provider_record_id
)
```

This ID can be formed before projection hashing and therefore creates no hash
cycle. Equivalent GraphSON/GraphML exports of the same Joern graph retain the
same provider-local external IDs. A regenerated Joern graph with different raw
content or ordinals produces different unresolved external IDs; only a
successful semantic/occurrence bridge provides cross-generation identity.

The provider ordinal contributes only to this explicitly external namespace.
It never becomes a native semantic identity. The legacy literal string form
`external:<producer>:<ordinal>` is not a valid durable identity and is
superseded by this rule.

---

# 8. Provider-Neutral Program Graph Model

## 8.1 Entity

```text
ProgramEntity {
    entity_id
    occurrence_id?
    entity_kind
    canonical_name?
    type_ref?
    source_anchor_ref?
    normalized_properties
}
```

Initial entity kinds:

```text
SourceFile
Namespace
TypeDecl
Type
Field
Function
Parameter
ParameterEffect
ReturnValue
LocalVariable
VariableUse
FieldUse
Constant
CallSite
Operation
FunctionReference
ControlNode
ReturnOperation
Block
MemoryObject
UnknownEntity
```

## 8.2 Relation

```text
ProgramRelation {
    relation_id
    relation_kind
    source_ref
    target_ref
    semantic_qualifiers
}
```

Initial relation kinds:

```text
SyntaxChild
ControlFlow
MayCall
CallArgument
CallReceiver
ReferencesSymbol
DefUse
ControlDependency
Dominates
PostDominates
HasType
Contains
ParameterInOut
Reads
Writes
Aliases
UnknownRelation
```

`relation_id` hashes context, kind, endpoints, and canonical semantic
qualifiers. It excludes provider identity. This permits multiple providers to
observe one canonical relation.

## 8.3 Provider observation

```text
ProviderObservation {
    observation_id
    provider_run_id
    provider_projection_id
    provider_record_id
    canonical_ref  // entity or relation
    epistemic
    confidence?
    capability_ref
    assumption_refs
    raw_record_digest
    provenance_locator
}
```

Identity and observation are separate. Provider observations never overwrite
canonical entities/relations or other observations.

`observation_id` is run-specific:

```text
ObservationID = H(
    "provider.observation.v1",
    provider_run_id,
    provider_record_id,
    canonical_ref,
    epistemic,
    canonical_observation_qualifiers
)
```

Provider observations and raw-record locators are excluded from
`ProviderProjectionID`. The projection hashes provider-neutral entities,
relations, canonical capabilities, and assumptions. This avoids a hash cycle
and permits distinct GraphSON/GraphML runs to bind the same normalized
projection while preserving distinct record-level provenance.

## 8.4 Provider extensions

Unknown labels, properties, or custom overlays are preserved as inert typed
data:

```text
ProviderExtensionObservation {
    extension_observation_id
    provider_run_id
    provider_record_id
    label
    typed_properties
    connected_provider_record_ids
    raw_record_digest
}
```

Extensions are queryable and citable, but they cannot participate in semantic
rules until a versioned mapping registers their meaning. They are run-specific,
inert observations and are excluded from `ProviderProjectionID` and semantic
fact identity. This prevents unknown machine paths, formatting, or
provider-specific payloads from contaminating canonical graph identity while
still preserving them in SummaryDB.

---

# 9. Reader and Raw Graph Boundary

GraphSON and GraphML readers emit the same private representation:

```text
RawProviderGraph {
    metadata
    nodes
    edges
    parse_diagnostics
}

RawNode {
    typed_provider_id
    label
    typed_properties
    source_record_digest
    source_record_locator
}

RawEdge {
    typed_provider_id
    label
    source_provider_id
    target_provider_id
    typed_properties
    source_record_digest
    source_record_locator
}
```

No GraphSON, GraphML, XML, Joern, TinkerPop, or JVM type escapes the reader
module.

Raw validation requires:

```text
complete parse through EOF
one usable META_DATA node
supported input dialect
supported CPG schema adapter
unique provider record identity or byte-equivalent duplicates
every edge endpoint exists
property values conform to the supported typed subset
counts and resource budgets are respected
metadata/context correspondence succeeds
```

Unknown vocabulary is not a schema failure. Invalid envelope structure,
conflicting duplicate IDs, dangling endpoints, or unsupported typed values are
failures.

---

# 10. Joern-to-VERITAS Normalization

## 10.1 Entity mapping

| Joern label | Provider-neutral entity |
| --- | --- |
| `FILE` | `SourceFile` |
| `NAMESPACE_BLOCK` | `Namespace` |
| `TYPE_DECL` | `TypeDecl` |
| `TYPE` | `Type` |
| `MEMBER` | `Field` |
| `METHOD` | `Function` |
| `METHOD_PARAMETER_IN` | `Parameter` |
| `METHOD_PARAMETER_OUT` | `ParameterEffect` |
| `METHOD_RETURN` | `ReturnValue` |
| `LOCAL` | `LocalVariable` |
| `IDENTIFIER` | `VariableUse` |
| `FIELD_IDENTIFIER` | `FieldUse` |
| `LITERAL` | `Constant` |
| `CALL` | `CallSite` or normalized `Operation` |
| `METHOD_REF` | `FunctionReference` |
| `CONTROL_STRUCTURE` | `ControlNode` |
| `RETURN` | `ReturnOperation` |
| `BLOCK` | `Block` |

## 10.2 Relation mapping

| Joern relation | Provider-neutral relation | Fact eligibility |
| --- | --- | --- |
| `AST` | `SyntaxChild` | graph only |
| `CFG` | `ControlFlow` | graph; registered control-flow facts only |
| `CALL` | `MayCall` | call fact |
| `ARGUMENT` | `CallArgument` | graph; call fact support |
| `RECEIVER` | `CallReceiver` | graph; call fact support |
| `REF` | `ReferencesSymbol` | graph; selected reference facts |
| `REACHING_DEF` | `DefUse` | value-flow fact |
| `CDG` | `ControlDependency` | control-dependency fact |
| `DOMINATE` | `Dominates` | dominating-check support only |
| `POST_DOMINATE` | `PostDominates` | graph; registered facts only |
| `EVAL_TYPE` | `HasType` | graph/type fact |
| `CONTAINS` | `Contains` | graph only |
| `PARAMETER_LINK` | `ParameterInOut` | graph; summary support only |

Joern call resolution is external and therefore maps to `MayCall` even when a
single target is reported. It may be corroborated by native call resolution,
but it is not imported as `MUST`.

## 10.3 Operator normalization

Joern encodes many language operations as calls. The versioned operator mapper
normalizes recognized operators before relation construction:

```text
<operator>.assignment    -> Assign
<operator>.addition      -> Add
<operator>.subtraction   -> Subtract
<operator>.fieldAccess   -> FieldAccess
<operator>.indirectFieldAccess -> IndirectFieldAccess
<operator>.indirection   -> Dereference
<operator>.addressOf     -> AddressOf
<operator>.indexAccess   -> IndexAccess
<operator>.conditional   -> Conditional
```

Unrecognized operators remain `Operation` entities with a provider extension.
No normalization rule guesses a load, store, field, or alias relation from
source spelling alone.

## 10.4 Property disposition

Every supported Joern property has a versioned disposition:

```text
SEMANTIC        canonical graph/fact input
OCCURRENCE      source-occurrence identity input after normalization
PROVENANCE_ONLY retained in the run/witness, excluded from semantic identity
DIAGNOSTIC      bounded display only
REJECT          unsafe or invalid for the declared schema
```

Absolute roots are normalized to verified project-relative paths or retained as
provenance only. Raw `CODE` text, formatting, exporter paths, timestamps, and
provider iteration details never enter semantic identity. Unknown properties
default to an inert `ProviderExtensionObservation`; a mapping version must opt
them into semantic or occurrence identity explicitly.

---

# 11. Member-Aware Memory Normalization

VERITAS preserves memory structure instead of flattening Joern code strings.

```text
ProviderMemoryAccess {
    access_kind = READ | WRITE | ADDRESS | UNKNOWN
    base_entity_ref
    canonical_field_path[]
    dereference_depth
    index_components[]
    declared_type_ref?
    recovered_type_ref?
    source_anchor_ref?
    uncertainty_reasons[]
}
```

Mapping to an existing native `MemoryRef` requires a stable base-object bridge
and compatible canonical field path. Otherwise the access remains a provider
observation over an `ExternalEntityID`.

Joern observations never narrow native SVF alias or points-to results by
absence. A positive Joern alias, flow, read, or write may be stored as an
inferred fact and compared with native analysis.

---

# 12. Capabilities, Assumptions, and Unknowns

## 12.1 Capability model

```text
ProviderCapability {
    domain = AST | CFG | SYMBOLS | TYPES | CALLS | DEF_USE |
             CONTROL_DEPENDENCE | DOMINATORS | POINTS_TO |
             EXTERNAL_SEMANTICS
    availability = ABSENT | PRESENT | PARTIAL | UNKNOWN
    basis = DECLARED | OBSERVED | VALIDATED
    unresolved_count?
    assumption_refs[]
}
```

Metadata and overlay names can establish declared availability. Observed
labels establish presence. Only importer-owned validation can establish the
limited properties that `VALIDATED` means. None of these states claims global
sound completeness.

## 12.2 Assumptions

Every imported fact retains the assumptions that enabled it, including:

```text
external-call semantics/model bundle
unresolved or heuristic call target
type-recovery mode
frontend configuration
data-flow overlay and semantics configuration
language/frontend limitations
truncation or provider-side slice budget
```

An assumption is a stable, queryable provenance input. It is not free-form
confidence text attached after publication.

## 12.3 Unknowns

The importer emits explicit unknown observations for:

```text
unresolved identity
ambiguous identity
missing required property for a mapping
unresolved type or call target
unsupported semantic mapping
partial capability
provider extension with no registered meaning
```

Unknowns are scoped and bounded. The importer does not compensate by linking
an unresolved call to every function or by inventing all-pairs alias edges.

---

# 13. External Fact Admission

## 13.1 External fact

The legacy stringly `ExternalFact` is replaced by canonical M9 facts plus
provider-specific occurrence data:

```text
ExternalFactObservation {
    canonical_fact
    provider_run_id
    provider_projection_id
    provider_observation_ids[]
    scope
    confidence?
    selected_witness
}
```

`canonical_fact` uses the registered `relations.v2` schema and M9 `FactID`
formation. Provider, run, witness, and raw record do not enter `FactID`; they
belong to the run binding and witness.

## 13.2 External fact batch

```text
ExternalFactBatch {
    provider_run_id
    batch_id
    provider_projection_id
    expected_components[]
    completed_components[]
    artifact_root_ids[]
    canonical_facts[]
    run_bindings[]
    witnesses[]
    assumptions[]
    diagnostics[]
}
```

The expected component set is derived from the validated normalized provider
projection and the versioned mapping registry. The producer must prove:

```text
expected components == completed components
every fact conforms to its registered relation schema
every witness is finite, acyclic, and closed
every witness leaf is an artifact, provider record, assumption, or prior fact
every run binding names a fact in the batch or existing canonical store
no fact has epistemic MUST
batch identity is canonical
```

## 13.3 Common publication boundary

```text
AnalysisFactBatch  (WPA only) --------+
                                      +--> FactPublicationValidator
ExternalFactBatch  (M12 providers) ---+             |
                                                    v
                                      Fact Store + Provenance Store
```

`AnalysisFactBatch` remains unchanged as the only WPA input. M12 does not add a
raw `FactStore::PublishFacts(vector<ExternalFact>)` bypass.

## 13.4 Epistemic and modality rules

Relation modality and epistemic origin are separate:

```text
relation = MayCall
epistemic = INFERRED
```

External semantic analysis produces `INFERRED`. Explicit provider premises and
model declarations produce `ASSUMED`. Neither may be published as `MUST`.

A registered deterministic verification rule may later derive a native fact
from an inferred candidate, but its witness must include the verifier inputs
and result. Agreement between two external providers alone is corroboration,
not verification.

---

# 14. SummaryDB Placement

The imported result is integrated into existing logical layers.

| SummaryDB layer | M12 content |
| --- | --- |
| Object Store | artifact descriptor, optional retained raw blob, canonical extension payloads |
| Metadata Store | provider artifacts, runs, capabilities, projections, current/history bindings |
| Graph Index | provider-neutral nodes, relations, observations, adjacency indexes |
| Fact Store | registered semantic facts and provider run bindings |
| Provenance Store | rooted provider witnesses, assumptions, raw-record digests/locators |
| Dependency Index | provider-component to Evidence/query-cache dependencies only |
| Evidence Cache | cases keyed by selected provider run/projection binding fingerprint |
| History Store | provider projection/component deltas and prior bindings |

There is no new physical SymbolicDB. The unified symbolic view is implemented
over these SummaryDB layers.

## 14.1 Required metadata bindings

```text
(repository, revision, build_variant, provider, provider_configuration)
    -> current { ProviderRunID, ProviderProjectionID }

ProviderRunID
    -> ProviderArtifactID + ProviderProjectionID
```

Different provider configurations coexist. A query selects a deterministic
ordered provider set; it never guesses between multiple current configurations.

## 14.2 Graph indexes

Required provider indexes include:

```text
provider_projection_id + entity_id -> canonical entity
provider_projection_id + relation_id -> canonical relation
provider_projection_id + source_ref + relation_kind -> outgoing
provider_projection_id + target_ref + relation_kind -> incoming
provider_run_id + canonical entity/relation -> provider observations
provider_run_id + provider_record_id -> normalized reference
revision + build_variant + provider/config -> current run/projection binding
```

## 14.3 Atomic publication

Publication order:

```text
1. Put immutable artifact descriptor/blob and extension payloads.
2. Build and validate the complete normalized graph and ExternalFactBatch.
3. Begin one SummaryDB metadata/fact/graph transaction.
4. Insert immutable provider run/projection/history rows.
5. Insert graph nodes, relations, observations, and adjacency rows.
6. Insert canonical facts, run bindings, witnesses, and assumptions.
7. Record provider-component Evidence/query dependencies and stale markers.
8. Swap the current provider binding.
9. Commit.
```

Steps 3–8 are one visibility operation. On any failure the prior provider
binding remains current. Native summary and M6 projection bindings are never
part of this transaction and never advance because of a Joern import.

---

# 15. Unified Query and Provider Fusion

## 15.1 Snapshot

```text
ProgramGraphSnapshot {
    repository_id
    revision_id
    build_variant_id
    native_projection_id
    fact_snapshot_id
    ordered_provider_bindings[]  // { ProviderRunID, ProviderProjectionID }
    provider_binding_fingerprint
}
```

The fingerprint hashes the exact ordered provider selection, run IDs,
projection IDs, capability digests, mapping versions, and assumption-set
digests. A semantically equal re-import with different raw provenance is
therefore a distinct Evidence snapshot even when it reuses the normalized
projection.

## 15.2 Query API

```text
UnifiedProgramGraphQuery {
    OpenSnapshot(snapshot_spec)
    GetEntity(entity_id)
    GetObservations(entity_or_relation_id)
    Traverse(relation_filter, budget)
    GetCalls(function_or_callsite, budget)
    GetValueFlow(source, sink, budget)
    GetMemoryAccesses(memory_ref, budget)
    GetCapabilities(provider_projection_id)
    GetAssumptions(observation_or_fact_id)
    CompareProviders(entity_or_relation_id)
}
```

Every result identifies its snapshot, completeness, truncation, canonical
members, provider observations, facts, and provenance refs.

## 15.3 Fusion relations

Fusion never mutates source facts. It derives explicit comparison records:

```text
SameProgramEntity
Corroborates
Contradicts
Refines
UnresolvedIdentity
```

Query-level status:

```text
NATIVE_ONLY
EXTERNAL_ONLY
CORROBORATED
CONTRADICTED
UNRESOLVED
```

Corroboration can improve explanation and ranking policy, but not epistemic
state. Contradictions remain visible and prevent a consumer from presenting a
single unqualified answer.

## 15.4 Authority defaults

Default provider selection is:

```text
native projection
+ all explicitly enabled current provider configurations
```

Importing Joern does not implicitly enable it for every deployment. The query
or Evidence configuration selects providers and records that selection in the
snapshot.

---

# 16. Completeness and Negative Evidence

Provider capabilities establish availability, not a closed-world proof.

```text
no Joern REACHING_DEF edge != no value flow
no Joern call target       != unreachable call
no Joern dominating check  != absence of a dominating check
```

Imported absence therefore cannot feed a negative relation or M10B's
`evidence.closed_world.dominating_check_absence.v1` rule.

A positive Joern observation is still relevant. If a provider reports a
candidate dominating check that conflicts with a native complete-empty result,
the unified query is not empty; it returns the candidate with inferred
epistemic state and the conflict/identity status. The Evidence Builder must not
derive unqualified negative evidence while a selected provider supplies a
contradicting positive observation or unresolved candidate in scope.

Provider-side truncation, slices, or partial overlays are always explicit and
propagate to query metadata as incompleteness or unknowns.

---

# 17. M10B and M10C Integration

M10B conceptually pins:

```text
EvidenceInputSnapshot {
    native_projection_id
    fact_snapshot_id
    selected_provider_bindings  // run + projection
    provider_binding_fingerprint
}
```

The selected provider run/projection bindings, capabilities, mapping versions,
and assumption-set digests participate in each query-completion fact's
`input_snapshot_fingerprint` and therefore in the typed `EvidenceBuildInput`
provenance closure.

M10B APIs remain semantic. They return canonical facts/relations,
`ProviderObservation` references, assumptions, unknowns, and provenance. They
never expose:

```text
Joern node IDs
GraphSON/GraphML objects
Joern labels as required consumer logic
FlatGraph/OverflowDB/TinkerPop/JVM types
```

M10C applies its existing validation rules. A Joern fact becomes Evidence only
through the same `EvidenceBuildInput -> EvidenceCase` path as native facts.

---

# 18. Incremental Update and Cache Freshness

Each provider projection computes component digests at least for:

```text
syntax
control_flow
calls
references
def_use
control_dependence
types
memory_access
capabilities
assumptions
```

Run-specific provider observations and unknown extensions have separate
evidence/provenance digests. They are not semantic projection components.

Where stable function ownership exists, each component also has a per-function
digest. Re-import compares canonical component digests:

```text
unchanged provider component -> reuse graph/fact objects
changed provider component   -> advance provider history binding
                             -> invalidate dependent queries/Evidence
```

Provider deltas do not schedule native Summary IR recomputation, M7 reverse
summary invalidation, or WPA. They affect only provider history, unified-query
caches, and Evidence dependencies.

Identical artifact, context, importer configuration, and mapping version is an
idempotent no-op. Different raw serialization with identical normalized
content reuses the provider projection while retaining a distinct artifact/run
binding. This is an evidence/provenance change even when every provider
semantic component is unchanged; semantic query results may be reused, while
provenance-bearing Evidence results are rebound or marked stale.

---

# 19. CLI Contract

```bash
veritas-build import \
  --joern <graph.graphson|graph.graphml> \
  --project <project-root> \
  --output <summarydb-root> \
  [--format auto|graphson|graphml] \
  [--accept-unverified-context]
```

Import and native analysis are separate commands. `import` does not run Joern,
regenerate a CPG, or rerun VERITAS analysis.

Provider selection for queries is explicit:

```bash
veritas-query ... --providers native
veritas-query ... --providers native,joern:<configuration-id>
```

Successful import diagnostics include:

```text
artifact/run/projection IDs
repository/revision/build binding
format, schema, frontend, language, overlays
raw and normalized node/edge counts
semantic fact count
known extension count
resolved/ambiguous/unresolved identity counts
capabilities and assumptions
component delta summary
whether the current provider binding advanced
```

Diagnostics are not durable canonical serialization.

---

# 20. Failure and Security Policy

## 20.1 Failure taxonomy

| Failure | Status and policy |
| --- | --- |
| Malformed GraphSON/GraphML | `InvalidArgument`; publish nothing |
| Explicit format/content mismatch | `InvalidArgument` |
| Unsupported dialect or CPG schema | `FailedPrecondition` |
| Missing/invalid metadata | `InvalidArgument` |
| Repository/revision/build mismatch | `FailedPrecondition` |
| No verifiable provider snapshot fingerprint | `FailedPrecondition` unless explicit context assumption is enabled |
| Conflicting duplicate provider ID | `InvalidArgument`; reject import |
| Missing edge endpoint | `InvalidArgument`; reject import |
| Unsupported typed property encoding | `InvalidArgument` |
| Unknown label/property/overlay extension | preserve; not an error |
| Identity ambiguity or no match | unresolved observation; not an error |
| Missing capability | absent/unknown capability; not fabricated |
| Resource budget exceeded | `ResourceExhausted`; publish nothing |
| Input changes during read | `Aborted`; publish nothing |
| Normalized graph/fact invariant failure | `DataLoss`; publish nothing |
| Transaction/storage failure | rollback; retain prior binding |
| Identical normalized re-import | success; idempotent no-op |

## 20.2 Untrusted-input boundary

Graph files are untrusted. Required controls:

```text
bounded or streaming parse
maximum input bytes
maximum nodes and edges
maximum properties per record
maximum property/list/string bytes
maximum JSON/XML nesting
bounded diagnostics and unknown-extension samples
GraphML DTD and external entities disabled
no network retrieval during parse
no embedded path dereference
no Joern script/plugin/JVM/native database execution
staging scoped to one import and cleaned after commit/rollback
```

Embedded source roots and file paths are data. They may be normalized and
compared with the project context; they never cause arbitrary filesystem reads.

## 20.3 Raw artifact retention

When full raw-artifact retention is disabled, every observation still retains:

```text
ProviderArtifactID
ProviderRecordID
canonical raw-record digest
bounded normalized property payload
source-record locator when meaningful
mapping version
```

This is sufficient to explain which imported record produced a normalized
observation. Re-import under a new mapping version requires the original file
or a retained raw blob.

---

# 21. Milestone Decomposition

## M12A — SummaryDB external-provider substrate

Depends on M2, M3, M6 storage foundations, and M9.

Delivers:

```text
provider artifact/run/capability/projection types
provider-neutral graph and observation model
provider projection storage/index/history bindings
ExternalFactBatch and shared validation boundary
atomic provider publication coordinator
provider-aware immutable query snapshot
in-memory and SQLite-backed conformance tests
```

M12A contains no Joern parser.

## M12B — Joern GraphSON/GraphML importer

Depends on M12A.

Delivers:

```text
bounded GraphSON and secure GraphML readers
RawProviderGraph and schema compatibility registry
snapshot validator and capability extractor
identity resolver
entity/relation/operator/type/call/data-flow/memory normalizers
extension preservation
Joern assumptions and rooted provenance
veritas-build import --joern
C and C++ golden/schema/adversarial fixtures
```

## M12C — Provider fusion and Evidence integration

Depends on M10B and M12B.

Delivers:

```text
UnifiedProgramGraphQuery
provider comparison/fusion relations
provider-aware M10B snapshot fingerprints
capability/assumption/unknown query results
provider-component Evidence/query invalidation
veritas-query provider selection
Joern -> SummaryDB -> M10B -> M10C integration tests
```

## M12D — PhASAR result adapter

Depends on M12A and is independent of the Joern graph reader/normalizers.
PhASAR primarily supplies analysis-result facts rather than a full program graph
projection. It requires a separate detailed design before implementation and
must not be forced through the Joern importer API.

---

# 22. Acceptance Test Contract

The implementation plan must assign stable test IDs to at least the following
cases.

## 22.1 Format and canonicalization

```text
FMT-001 supported whole-graph GraphSON imports
FMT-002 supported whole-graph GraphML imports
FMT-003 equivalent GraphSON/GraphML -> same ProviderProjectionID
FMT-004 record order does not affect canonical projection
FMT-005 property order does not affect canonical projection
FMT-006 explicit format mismatch rejects atomically
FMT-007 arbitrary application JSON is rejected
FMT-008 per-method/non-whole-graph export is rejected
FMT-009 equivalent GraphSON/GraphML -> same ProviderRawGraphID
FMT-010 export-format-only edge IDs do not affect normalized identity
```

## 22.2 Schema, context, and capabilities

```text
SCH-001 supported CPG schema/frontend/overlay imports
SCH-002 unsupported CPG schema rejects atomically
SCH-003 missing META_DATA rejects
SCH-004 multiple conflicting META_DATA records reject
SCH-005 repository/revision/build mismatch rejects
SCH-006 absent/unrecognized source fingerprint requires explicit context assumption
SCH-007 source match without build-config match records build assertion
SCH-008 verified source or build fingerprint mismatch cannot be overridden
SCH-009 absent data-flow overlay cannot claim def-use capability
SCH-010 observed capability cannot claim sound completeness
SCH-011 unknown overlay is preserved as extension metadata
SCH-012 provider configuration selects a distinct current binding
SCH-013 missing Joern distribution version is recorded as unknown, not guessed
```

## 22.3 Identity

```text
ID-001 provider ordinal never becomes semantic ID
ID-002 exact function bridge resolves FunctionVariantID
ID-003 exact source bridge resolves SourceAnchorID
ID-004 stable call occurrence resolves CallSiteID
ID-005 member path resolves compatible MemoryRef
ID-006 ambiguous match remains unresolved with sorted candidates
ID-007 missing match receives canonical ExternalEntityID
ID-008 host-absolute path does not enter semantic identity
ID-009 equivalent imports on different hosts produce equal normalized IDs
ID-010 raw CODE formatting does not enter semantic identity
```

## 22.4 Normalization and memory structure

```text
NRM-001 entity-label mapping covers the V1 allowlist
NRM-002 AST -> SyntaxChild without fact-store inflation
NRM-003 CFG -> ControlFlow
NRM-004 CALL/ARGUMENT/RECEIVER normalize coherently
NRM-005 REF -> ReferencesSymbol
NRM-006 REACHING_DEF variable qualifier is preserved
NRM-007 CDG/DOMINATE/POST_DOMINATE remain distinct
NRM-008 known operator calls normalize semantically
NRM-009 unknown operator remains inert extension
NRM-010 unknown node/edge/property survives round-trip query
NRM-011 unknown extension does not affect semantic ProviderProjectionID
MEM-001 base object and field path are preserved
MEM-002 dereference depth is preserved
MEM-003 index components are preserved without guessed ranges
MEM-004 unresolved memory access remains external/inferred
MEM-005 Joern absence never narrows native alias/points-to results
```

## 22.5 Facts, witnesses, and epistemic rules

```text
FCT-001 semantic mapping emits registered relations.v2 fact
FCT-002 structural-only mapping emits no semantic fact
FCT-003 every external fact has a closed rooted witness
FCT-004 expected/completed component mismatch rejects batch
FCT-005 Joern MUST publication is rejected
FCT-006 MayCall modality and INFERRED epistemic remain distinct
FCT-007 external semantics appear as assumptions
FCT-008 unresolved resolution appears as unknown
FCT-009 raw-record digest/provider record is reachable in explanation
FCT-010 no raw vector<ExternalFact> publication bypass exists
FCT-011 user-asserted context assumption is inherited by every imported fact
```

## 22.6 Publication, history, and incrementality

```text
PUB-001 malformed input publishes nothing
PUB-002 identity/normalization failure publishes nothing
PUB-003 witness failure publishes nothing
PUB-004 injected graph/fact transaction failure rolls back all provider state
PUB-005 failure retains previous current provider binding
PUB-006 identical re-import is idempotent
PUB-007 equivalent normalized different-format import reuses projection but creates distinct run provenance
PUB-008 historical provider run/projection remains readable
PUB-009 provider import never advances native summary/CPG binding
PUB-010 component delta invalidates only dependent query/Evidence cache entries
PUB-011 provider delta never schedules native summary/WPA work
PUB-012 artifact descriptor/digest is durable with or without retained raw blob
```

## 22.7 Fusion and Evidence

```text
FUS-001 native-only result remains unchanged without Joern
FUS-002 Joern-only inferred observation is queryable
FUS-003 matching native/Joern facts produce corroboration
FUS-004 contradiction retains both observations
FUS-005 corroboration does not promote epistemic state
FUS-006 provider selection changes snapshot fingerprint
FUS-007 provider binding change makes old combined snapshot stale
EVD-001 provider IDs/capabilities/assumptions enter completion provenance
EVD-002 GraphSON/GraphML/Joern types never enter EvidenceBuildInput
EVD-003 imported absence cannot produce negative evidence
EVD-004 positive provider contradiction blocks unqualified negative result
EVD-005 truncated/partial provider data remains incomplete or unknown
EVD-006 Joern evidence reaches EIR only through EvidenceBuildInput
```

## 22.8 CLI and provider selection

```text
CLI-001 import requires --joern, --project, and --output
CLI-002 auto format detection accepts supported content independent of extension
CLI-003 explicit format/content mismatch reports InvalidArgument
CLI-004 unverified context requires the explicit opt-in flag
CLI-005 import diagnostics report stable IDs/counts/capabilities/assumptions/deltas
CLI-006 provider query selection pins the exact current run/projection binding
CLI-007 import executes no Joern CLI, script, plugin, JVM, or network request
```

## 22.9 Adversarial and scale

```text
SEC-001 GraphML DTD/external entity is rejected without retrieval
SEC-002 deeply nested JSON/XML hits a bounded failure
SEC-003 oversized property/string/list hits a bounded failure
SEC-004 node/edge/input-byte budget hits ResourceExhausted
SEC-005 dangling endpoint rejects atomically
SEC-006 conflicting duplicate ID rejects atomically
SEC-007 embedded absolute/path-traversal value triggers no filesystem read
SEC-008 input modified during read aborts publication
PER-001 large fixture stays within declared peak-memory budget
PER-002 large fixture output is deterministic across runs
PER-003 diagnostic samples remain bounded
PER-004 budget failure leaves no staging/current-binding residue
```

Every case must assert required and forbidden typed outcomes before comparing
diagnostic text or golden files.

---

# 23. Upstream Format References

The implementation must qualify against pinned fixtures generated from the
supported upstream contracts, not against undocumented assumptions:

- [Joern graph export formats](https://docs.joern.io/export/)
- [Code Property Graph schema](https://cpg.joern.io/)
- [Joern data-flow semantics](https://docs.joern.io/dataflow-semantics/)
- [Joern slice JSON schema](https://github.com/joernio/joern/blob/master/joern-cli/JOERN_SLICE.md)

`joern-slice` remains candidate-evidence input for a future extension; it is
not a substitute for the M12B whole-graph contract.

---

# 24. Exit Criteria

M12A–M12C are complete when all of the following hold:

```text
VERITAS directly imports supported Joern GraphSON and GraphML whole graphs.
The complete normalized result is stored as immutable provider state in SummaryDB.
Equivalent exports produce deterministic provider-neutral graph identities.
Native and Joern observations share a semantic query surface without sharing authority.
Joern facts are atomic, rooted, explainable, and epistemically lowered.
Unknown extensions, assumptions, capabilities, and identity failures are explicit.
Provider imports never mutate M6/native summaries or schedule native WPA.
M10B pins provider projections in its immutable snapshot and provenance.
Imported absence cannot become negative evidence.
Removing Joern leaves native VERITAS behavior unchanged.
All format, schema, identity, normalization, fact, publication, fusion,
Evidence, adversarial, and scale acceptance cases pass.
```

M12D/PhASAR is not part of the Joern completion gate and requires its own
detailed design and plan.
