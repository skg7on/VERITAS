# VERITAS Engineering Backbone Design Specification

## SummaryDB, Incremental WPA, and Provenance-Aware Fact Infrastructure

**Status:** Draft Architecture Specification
**Version:** 0.1
**Project:** VERITAS
**Depends on:**

- `docs/architecture/veritas-platform-architecture-design.md` — platform pipeline, principles P1–P8, ingest adapter tiers.
- `docs/architecture/veritas-whole-program-analysis-design.md` — analyzer engines and SOTA C/C++ pointer-alias policy.
- `docs/architecture/veritas-thin-summarydb-backends-design.md` — SummaryDB physical layers and pluggable storage backends.

---

# 1. Purpose

This document turns the high-level SummaryDB architecture into an engineering design for the first real VERITAS backbone.

The backbone is the subsystem that makes VERITAS scalable before any LLM reviewer is introduced:

```text
Tier 1: project directory containing compile_commands.json (current/pre-M11)
    -> VERITAS Build Intelligence and owned Clang AST/LLVM IR construction
M11 Tier 2: .bc/.ll file or directory
    -> VERITAS-owned bitcode module acquisition
both -> required in-process SVF analysis
    -> stable function identities
    -> immutable function summaries
    -> semantic component hashes
    -> reverse dependency index
    -> SCC-aware incremental WPA
    -> provenance-aware derived facts
    -> Evidence Builder inputs
```

The central engineering goal is:

> Given a large C/C++ codebase and a source revision change, recompute only the semantic facts whose inputs actually changed, explain every derived fact, and expose enough stable IDs for Evidence IR to cite the result without reloading the whole program.

This document specifies the V1 data model, hash model, storage layout, update algorithms, consistency rules, and implementation milestones.

**Approved remediation overlay.** The implemented M8 baseline uses C++
fixpoint execution and optional Souffle comparison. The approved M8R target is
not yet delivered and supersedes live WPA/Datalog ownership statements in this
draft: Function Summary IR is durable, detailed relations are run-local, pinned
SVF owns V1 points-to/alias/SVFG and indirect calls, compiled Souffle owns normal
production recursion, and C++ is conformance or explicit emergency only. The
[M8R bridge spec](milestones/m8r-souffle-wpa-remediation-design-spec.md) is the
delivery authority.

---

# 2. Scope

This specification covers:

* function identity tables,
* build and revision identity,
* immutable content-addressed summary storage,
* summary component hashes,
* component-level semantic deltas,
* reverse dependency indexing,
* SCC/fixpoint propagation,
* provenance-aware derived facts,
* SummaryDB publication semantics,
* APIs needed by WPA, query tools, and Evidence Builder.

This specification does not cover:

* full Evidence IR syntax,
* LLM Review Agent behavior,
* symbolic execution,
* SMT proof backends,
* distributed worker scheduling beyond required storage invariants,
* domain-specific RAN/telecom semantic graphs.

Those systems consume this backbone but are not required for the first implementation.

---

# 3. Design Invariants

The backbone MUST preserve these invariants.

| ID | Invariant | Reason |
| --- | --- | --- |
| B1 | Function identity is semantic, not file-line based. | Source locations drift and macros/templates create multiple meanings. |
| B2 | Summary objects are immutable and content-addressed. | Enables deduplication, parallel generation, historical queries, and reproducibility. |
| B3 | Summary components have independent semantic hashes. | Allows analysis-specific invalidation instead of invalidating all callers. |
| B4 | Dependency edges are reverse-queryable by component delta. | Incremental WPA must find consumers of exactly the changed semantic dimension. |
| B5 | Recursive call regions are propagated as SCCs. | Recursion requires monotone fixpoint convergence, not naive caller chains. |
| B6 | Every derived fact has provenance. | VERITAS must answer why a fact is true and feed proof evidence into EIR. |
| B7 | Epistemic state and confidence are separate. | `MAY` is not low confidence, and `INFERRED` is not verified. |
| B8 | Publication is atomic at metadata bindings, not object mutation. | Readers must never observe a half-written summary revision. |
| B9 | Deterministic analysis is reproducible for the same inputs. | Enables stable hashes, diffing, and regression analysis. |
| B10 | `--project <directory>` is the only public source input and the current pre-M11 contract. M11 adds mutually exclusive `--bitcode <.bc\|.ll\|directory>` as a Tier-2 module-acquisition input; both routes continue through VERITAS-owned local extraction, SVF, Summary IR, WPA, and provenance. No route accepts an SVF artifact or bypasses analysis. M12 external facts are non-authoritative terminal observations. | Preserves owned analysis and reproducibility while admitting controlled module input without confusing it with source or third-party analysis output. |
| B11 | Function Summary IR is durable; `relations.v2` and typed dense IDs are run-local. | Preserve one stable WPA contract. |
| B12 | Compiled Souffle is the normal recursive owner; C++ is conformance or explicitly selected emergency only. | Prevent semantic split and silent fallback. |
| B13 | Every published derived fact has a generic deterministic finite rooted witness. | Engine-neutral explanation. |
| B14 | Failed components publish no replacement; prior success remains stale history. | Failure atomicity. |
| B15 | M9 receives only a complete, rooted, idempotently delivered `AnalysisFactBatch`. | Safe durable publication. |

---

# 4. Logical Architecture

```text
        Tier 1 Project + Build Config | M11 Tier 2 Bitcode Modules
                              |
                              v
                      Build Intelligence
                              |
                              v
                Typed In-Memory Project Context
                              |
                              v
              +---------------+----------------+
              |                                |
              v                                v
      Clang Semantic Frontend           LLVM Analysis Frontend
              |                                |
              +---------------+----------------+
                              |
                              v
                   Required In-Process SVF
                              |
                              v
                     Local Summary Builder
                              |
                              v
               Immutable Function Summary IR v2
                              |
                              v
      +------------------- SummaryDB --------------------+
      |                                                  |
      |  CAS Object Store                                |
      |  Metadata Tables                                 |
      |  Component Hash Index                            |
      |  Reverse Dependency Index                        |
      |  Fact Store                                      |
      |  Provenance Store                                |
      |  SCC/WPA State                                   |
      |                                                  |
      +---------------------+----------------------------+
                            |
                            v
                WPA Input Materializer
                 run-local relations.v2
                            |
              +-------------+-------------+
              |                           |
              v                           v
     Compiled Souffle WPA          C++ Conformance Oracle
       required production        or explicit cpp-emergency
              +-------------+-------------+
                            |
                            v
              Result/Witness Canonicalizer
                            |
                            v
                 Analysis Fact Bus
                            |
                            v
           Global Derived Facts + Witnesses
                            |
                            v
                  Query API / Evidence Builder
```

V1 can use:

```text
RocksDB        immutable summary objects and local key-value indexes
SQLite         metadata, identities, dependency edges, publication bindings
Run-local      typed relation projections and diagnostic artifacts
In-memory      temporary graph projection and SCC worklists
```

The schema should not assume SQLite forever. Table definitions below are logical contracts. They can move to PostgreSQL or another metadata store later.

---

# 5. Identity Model

VERITAS uses multiple IDs because "the function" has multiple meanings.

| ID | Meaning | Stable Across | Changes When |
| --- | --- | --- | --- |
| `RepositoryID` | Logical repository identity. | Branches and revisions of one repo. | The root project identity changes. |
| `RevisionID` | Source snapshot identity. | Builds of the same commit/tree. | Git tree or source manifest changes. |
| `BuildVariantID` | Target, ABI, macros, compile flags, dependency closure. | Rebuilds with identical semantic build config. | Target, ABI, macro set, relevant headers, or compile options change. |
| `FunctionSymbolID` | Logical function declaration/definition identity. | Source movement and line changes. | Name/signature/linkage/template specialization semantic identity changes. |
| `FunctionVariantID` | Build-specific function meaning. | Rebuilds with equivalent semantic config. | Function symbol or build variant changes. |
| `FunctionBodyID` | Normalized function body for one revision/build. | Formatting and nonsemantic source edits. | AST/IR semantic body changes. |
| `FunctionSummaryID` | Exact immutable summary content. | All consumers and revisions with identical analysis output. | Any summary component payload or summary metadata changes. |
| `AnalyzerRunID` | Analyzer binary/config/schema identity. | All facts emitted by one configured analyzer run. | Analyzer version, schema version, or analysis config changes. |
| `AnalysisRunID` | Exact WPA execution manifest and envelope identity. | Never merged across incompatible or engine-distinct runs. | Revision, build variant, summary/relation schema, rule/model bundle, SVF/WPA configuration, engine, or exact toolchain identity changes. |
| `FactID` | Witness-independent canonical `relations.v2` semantic row. | Revisions, builds, runs, engines, and derivations with identical typed stable semantic cells and epistemic value. | Relation schema/name, typed stable semantic cell, or epistemic value changes. |

## 5.1 Hash Format

All IDs SHOULD be encoded as:

```text
<kind>:<algorithm>:<hex-digest>
```

Examples:

```text
funcsym:sha256:...
funcvar:sha256:...
summary:sha256:...
analysis_run:sha256:...
fact:sha256:...
```

V1 SHOULD use SHA-256. The hash algorithm is part of the ID string so the system can migrate later.

## 5.2 Canonical Hashing Rules

Every content-derived ID MUST be computed from a canonical byte representation:

* UTF-8 strings normalized to one selected form.
* Maps sorted by key.
* Repeated unordered fields sorted by canonical child ID.
* Repeated ordered fields retain order only when order has semantics.
* Default values either omitted everywhere or emitted everywhere.
* Source paths normalized relative to repository root.
* Absolute local machine paths excluded from semantic hashes.
* Timestamps excluded from semantic hashes.
* Analyzer debug text excluded from semantic hashes unless the field is semantically consumed.

Source locations MAY be stored as anchors, but they MUST NOT be the primary input to function identity.

---

# 6. Function Identity Tables

This section defines the minimum metadata schema for stable function identities.

## 6.1 Repository and Revision Tables

```sql
CREATE TABLE repositories (
    repository_id      TEXT PRIMARY KEY,
    logical_name       TEXT NOT NULL,
    root_fingerprint   TEXT NOT NULL,
    created_at         INTEGER NOT NULL
);

CREATE TABLE revisions (
    revision_id        TEXT PRIMARY KEY,
    repository_id      TEXT NOT NULL REFERENCES repositories(repository_id),
    vcs_kind           TEXT NOT NULL,
    vcs_revision       TEXT,
    source_tree_hash   TEXT NOT NULL,
    created_at         INTEGER NOT NULL,
    UNIQUE(repository_id, source_tree_hash)
);
```

`source_tree_hash` is a normalized manifest hash, not necessarily the Git commit hash. Generated source and vendor snapshots can participate if the build uses them.

## 6.2 Build Variant Tables

```sql
CREATE TABLE build_variants (
    build_variant_id       TEXT PRIMARY KEY,
    repository_id          TEXT NOT NULL REFERENCES repositories(repository_id),
    target_triple          TEXT NOT NULL,
    abi                    TEXT NOT NULL,
    compiler_id            TEXT NOT NULL,
    compiler_version       TEXT NOT NULL,
    compile_options_hash   TEXT NOT NULL,
    macro_set_hash         TEXT NOT NULL,
    include_closure_hash   TEXT NOT NULL,
    type_layout_hash       TEXT NOT NULL,
    config_hash            TEXT NOT NULL
);

CREATE TABLE translation_units (
    translation_unit_id    TEXT PRIMARY KEY,
    revision_id            TEXT NOT NULL REFERENCES revisions(revision_id),
    build_variant_id       TEXT NOT NULL REFERENCES build_variants(build_variant_id),
    source_path            TEXT NOT NULL,
    compile_command_hash   TEXT NOT NULL,
    preprocessor_hash      TEXT NOT NULL,
    ast_hash               TEXT,
    llvm_module_hash       TEXT,
    UNIQUE(revision_id, build_variant_id, source_path, compile_command_hash)
);
```

`BuildVariantID` is computed from build inputs that can alter semantic meaning, not from incidental build metadata.

## 6.3 Function Symbol Table

```sql
CREATE TABLE function_symbols (
    function_symbol_id     TEXT PRIMARY KEY,
    repository_id          TEXT NOT NULL REFERENCES repositories(repository_id),
    language               TEXT NOT NULL,
    mangled_name           TEXT,
    qualified_name         TEXT NOT NULL,
    canonical_signature    TEXT NOT NULL,
    linkage_kind           TEXT NOT NULL,
    namespace_scope        TEXT,
    class_scope            TEXT,
    template_identity      TEXT,
    overload_key           TEXT,
    symbol_usr             TEXT,
    first_seen_revision_id TEXT REFERENCES revisions(revision_id)
);

CREATE INDEX idx_function_symbols_name
    ON function_symbols(repository_id, qualified_name);
```

`FunctionSymbolID` SHOULD be computed from:

```text
repository_id
language
mangled_name or canonical declaration USR
canonical_signature
linkage_kind
namespace/class scope
template specialization identity
```

For static/internal-linkage functions, the identity MUST include a stable enclosing translation unit semantic identity to avoid merging unrelated local symbols with the same name.

## 6.4 Function Variant Table

```sql
CREATE TABLE function_variants (
    function_variant_id    TEXT PRIMARY KEY,
    function_symbol_id     TEXT NOT NULL REFERENCES function_symbols(function_symbol_id),
    build_variant_id       TEXT NOT NULL REFERENCES build_variants(build_variant_id),
    calling_convention     TEXT,
    target_features_hash   TEXT,
    type_layout_hash       TEXT NOT NULL,
    macro_context_hash     TEXT NOT NULL,
    UNIQUE(function_symbol_id, build_variant_id, target_features_hash, macro_context_hash)
);

CREATE INDEX idx_function_variants_symbol
    ON function_variants(function_symbol_id);
```

`FunctionVariantID` identifies the meaning of a function under a build configuration. The same source-level function can have multiple variants because of macros, target features, ABI, template instantiation, or type layout differences.

## 6.5 Function Body Table

```sql
CREATE TABLE function_bodies (
    function_body_id       TEXT PRIMARY KEY,
    function_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    revision_id            TEXT NOT NULL REFERENCES revisions(revision_id),
    translation_unit_id    TEXT NOT NULL REFERENCES translation_units(translation_unit_id),
    source_hash            TEXT NOT NULL,
    ast_body_hash          TEXT NOT NULL,
    ir_body_hash           TEXT,
    semantic_body_hash     TEXT NOT NULL,
    source_anchor_id       TEXT,
    is_definition          INTEGER NOT NULL,
    UNIQUE(function_variant_id, revision_id, translation_unit_id, semantic_body_hash)
);

CREATE INDEX idx_function_bodies_variant_revision
    ON function_bodies(function_variant_id, revision_id);
```

`semantic_body_hash` is the key input for avoiding unnecessary local summary recomputation. It should ignore comments and formatting and should normalize equivalent AST/IR forms where practical.

## 6.6 Source Anchor Table

```sql
CREATE TABLE source_anchors (
    source_anchor_id       TEXT PRIMARY KEY,
    revision_id            TEXT NOT NULL REFERENCES revisions(revision_id),
    path                   TEXT NOT NULL,
    start_line             INTEGER,
    start_column           INTEGER,
    end_line               INTEGER,
    end_column             INTEGER,
    macro_expansion_stack  TEXT,
    spelling_location      TEXT,
    expansion_location     TEXT
);
```

Source anchors are for diagnostics and Evidence IR display. They are not identity.

---

# 7. Analyzer Runs, Analysis Runs, and Schema Versioning

`AnalyzerRunID` identifies local extraction and Summary IR production.
`AnalysisRunID` identifies a whole WPA execution envelope. They are distinct:
content-addressed result reuse may reference the same immutable result from
multiple analysis runs, but it never merges their manifests or history.

Summary identities and run-fact bindings MUST retain enough analyzer and WPA
metadata to avoid silently mixing incompatible outputs. Canonical `FactID`
itself remains run- and witness-independent under §15.3. `AnalyzerRunID` hashes
analyzer binary/config/schema identity; its `started_at` lifecycle field is not
an identity input.

```sql
CREATE TABLE analyzer_runs (
    analyzer_run_id        TEXT PRIMARY KEY,
    analyzer_name          TEXT NOT NULL,
    analyzer_version       TEXT NOT NULL,
    schema_version         TEXT NOT NULL,
    config_hash            TEXT NOT NULL,
    trust_level            INTEGER NOT NULL,
    started_at             INTEGER NOT NULL
);

CREATE TABLE analysis_configurations (
    config_hash            TEXT PRIMARY KEY,
    serialized_config      BLOB NOT NULL
);

CREATE TABLE engine_toolchains (
    engine_toolchain_identity      TEXT PRIMARY KEY,
    engine_identity                TEXT NOT NULL,
    canonical_provenance_payload   BLOB NOT NULL,
    canonical_provenance_hash      TEXT NOT NULL
);

CREATE TABLE analysis_run_manifests (
    analysis_run_id          TEXT PRIMARY KEY,
    revision_id              TEXT NOT NULL REFERENCES revisions(revision_id),
    build_variant_id         TEXT NOT NULL REFERENCES build_variants(build_variant_id),
    summary_schema_version   TEXT NOT NULL,
    relation_schema_version  TEXT NOT NULL,
    rule_bundle_version      TEXT NOT NULL,
    model_bundle_version     TEXT NOT NULL,
    svf_configuration_hash   TEXT NOT NULL,
    wpa_configuration_hash   TEXT NOT NULL,
    engine_identity          TEXT NOT NULL,
    engine_toolchain_identity TEXT NOT NULL REFERENCES engine_toolchains(engine_toolchain_identity)
);

CREATE TABLE analysis_run_state (
    analysis_run_id          TEXT PRIMARY KEY REFERENCES analysis_run_manifests(analysis_run_id),
    status                   TEXT NOT NULL,
    started_at               INTEGER NOT NULL,
    completed_at             INTEGER,
    diagnostics_object_ref   TEXT
);
```

`AnalysisRunID` hashes only the immutable `AnalysisRunManifest` fields:
revision, build variant, summary/relation schemas, rule/model bundles, SVF/WPA
configurations, engine identity, and exact engine/toolchain identity. Mutable
lifecycle status, timestamps, diagnostics, retry count, and progress never
participate in `AnalysisRunID`; they live in `analysis_run_state`.
`EngineToolchainIdentity` is the hash of `engine_identity` plus the canonical
engine-specific provenance payload. An
`analysis_run_manifests.engine_identity` must match the referenced toolchain
record's engine identity. The validator applies a tagged union rule:

* `souffle` requires the configured install-provenance manifest and its digest,
  version 2.5 source revision
  `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, the configured executable digest
  verified against that manifest, the generated-bundle digest, and
  generator/compiler/link toolchain provenance;
* C++ conformance and `cpp-emergency` require the exact C++ build identity and
  reject a payload that claims or reuses Souffle provenance;
* a future engine must define and validate its own canonical payload before it
  can create a run.

Thus every engine has exact provenance without fabricating another engine's
fields. `canonical_provenance_hash` must match the stored payload before an
`AnalysisRunManifest` or component record may reference it.

Examples of config fields:

```text
alias_precision
range_precision
template_instantiation_policy
unknown_external_policy
timeout_budget
source_language_standard
target_triple
```

If any configuration value can alter emitted summary facts, it belongs in the relevant hash.

---

# 8. Immutable Content-Addressed Summaries

## 8.1 Summary Object Model

The production summary object SHOULD be Protobuf. Its logical shape is:

```text
FunctionSummary {
    header
    identity
    component_hashes
    calls
    memory_effects
    value_flows
    range_facts
    alias_facts
    taint_transfers
    ownership_effects
    lock_effects
    state_transitions
    unknowns
    assumptions
    dependencies
    provenance_refs
}
```

The object is immutable. A new summary is published by writing a new object and updating metadata bindings. Existing summary bytes are never overwritten.

## 8.2 Summary Object Tables

```sql
CREATE TABLE summary_objects (
    function_summary_id    TEXT PRIMARY KEY,
    function_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    function_body_id       TEXT NOT NULL REFERENCES function_bodies(function_body_id),
    analyzer_run_id        TEXT NOT NULL REFERENCES analyzer_runs(analyzer_run_id),
    object_hash            TEXT NOT NULL,
    object_size_bytes      INTEGER NOT NULL,
    object_store_key       TEXT NOT NULL,
    semantic_hash          TEXT NOT NULL,
    evidence_hash          TEXT NOT NULL,
    created_at             INTEGER NOT NULL
);

CREATE INDEX idx_summary_objects_variant
    ON summary_objects(function_variant_id);

CREATE INDEX idx_summary_objects_body_analyzer
    ON summary_objects(function_body_id, analyzer_run_id);
```

`semantic_hash` covers payload that affects program-analysis meaning.

`evidence_hash` covers semantic payload plus provenance references that affect explanation. This lets a semantic consumer skip invalidation when only explanatory detail changes, while Evidence Builder can still refresh explanations.

## 8.3 Revision Bindings

```sql
CREATE TABLE summary_bindings (
    revision_id            TEXT NOT NULL REFERENCES revisions(revision_id),
    build_variant_id       TEXT NOT NULL REFERENCES build_variants(build_variant_id),
    function_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    function_summary_id    TEXT NOT NULL REFERENCES summary_objects(function_summary_id),
    publication_epoch      INTEGER NOT NULL,
    is_current             INTEGER NOT NULL,
    published_at           INTEGER NOT NULL,
    PRIMARY KEY(revision_id, build_variant_id, function_variant_id, publication_epoch)
);

CREATE UNIQUE INDEX idx_summary_bindings_current
    ON summary_bindings(revision_id, build_variant_id, function_variant_id)
    WHERE is_current = 1;
```

The current binding for a function variant is the mutable pointer. The summary object itself is immutable.

Publication rule:

```text
1. Write CAS object.
2. Insert summary_objects row.
3. Insert component rows.
4. Insert dependency rows.
5. In one metadata transaction:
   - mark old current binding not current
   - insert new current binding
   - enqueue dependency delta
```

Readers use a stable snapshot transaction or a publication epoch watermark.

---

# 9. Semantic Component Hashes

## 9.1 Component Set

Each summary has independently hashed components.

| Component | Hash Name | Typical Consumers |
| --- | --- | --- |
| Calls | `call_hash` | call graph, SCC builder, reachability, impact analysis |
| Memory effects | `effect_hash` | may-read/may-write, UAF, ownership, Evidence Builder |
| Value flow | `value_flow_hash` | taint, buffer overflow, data dependency slicing |
| Range facts | `range_hash` | bounds, nullability, constraint extraction |
| Alias facts | `alias_hash` | memory effects, value flow, concurrency |
| Taint transfer | `taint_hash` | injection, untrusted input flows |
| Ownership effects | `ownership_hash` | leak, UAF, double free |
| Lock effects | `lock_hash` | deadlock, race, lock ordering |
| State transitions | `state_hash` | protocol/state-machine analysis |
| Unknowns | `unknown_hash` | Evidence IR unknown tracking, model gap reports |
| Assumptions | `assumption_hash` | proof context, trust policy |
| Dependencies | `dependency_hash` | invalidation diagnostics and audits |
| Provenance | `provenance_hash` | `explainFact`, Evidence IR derivation |

## 9.2 Component Hash Table

```sql
CREATE TABLE summary_components (
    function_summary_id    TEXT NOT NULL REFERENCES summary_objects(function_summary_id),
    component_kind         TEXT NOT NULL,
    semantic_component_hash TEXT NOT NULL,
    evidence_component_hash TEXT NOT NULL,
    item_count             INTEGER NOT NULL,
    payload_offset         INTEGER,
    payload_length         INTEGER,
    PRIMARY KEY(function_summary_id, component_kind)
);

CREATE INDEX idx_summary_components_hash
    ON summary_components(component_kind, semantic_component_hash);
```

Component hashes allow deltas like:

```text
RangeHash changed
ValueFlowHash unchanged
CallHash unchanged
```

This is the core of semantic incremental recomputation.

## 9.3 Component Delta

```text
SummaryDelta {
    function_variant_id
    old_summary_id
    new_summary_id
    changed_components[]
    semantic_changed: bool
    evidence_changed: bool
}

ComponentDelta {
    component_kind
    old_semantic_hash
    new_semantic_hash
    old_evidence_hash
    new_evidence_hash
    added_items[]
    removed_items[]
    changed_items[]
}
```

V1 may compute item-level lists only for calls, memory effects, value flows, and range facts. Other components can initially report hash-level change only.

## 9.4 Semantic vs Evidence Change

The system SHOULD distinguish:

| Change Type | Example | Invalidate Analysis Consumers | Refresh Evidence |
| --- | --- | --- | --- |
| No change | Rebuild emits identical summary. | No | No |
| Evidence-only | Better source span or derivation tree. | No | Yes, if case cites affected explanation |
| Semantic component | Range bound changes. | Yes, for consumers sensitive to `range` | Yes |
| Identity/build | ABI or type layout changes. | Yes, broadly | Yes |

---

# 10. Summary Dependencies

Every summary records the semantic inputs it depends on.

## 10.1 Dependency Kinds

```text
CALL
MAY_CALL
TYPE_LAYOUT
GLOBAL_VALUE
ALIAS
SUMMARY_COMPONENT
BUILD_CONFIG
MACRO
CONTRACT
EXTERNAL_MODEL
ANALYZER_CONFIG
SOURCE_BODY
```

## 10.2 Dependency Table

```sql
CREATE TABLE summary_dependencies (
    consumer_summary_id    TEXT NOT NULL REFERENCES summary_objects(function_summary_id),
    consumer_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    consumer_component     TEXT NOT NULL,
    producer_kind          TEXT NOT NULL,
    producer_id            TEXT NOT NULL,
    producer_component     TEXT,
    dependency_kind        TEXT NOT NULL,
    sensitivity            TEXT NOT NULL,
    epistemic              TEXT NOT NULL,
    provenance_id          TEXT,
    PRIMARY KEY(
        consumer_summary_id,
        consumer_component,
        producer_kind,
        producer_id,
        producer_component,
        dependency_kind
    )
);

CREATE INDEX idx_summary_dependencies_reverse
    ON summary_dependencies(producer_kind, producer_id, producer_component);

CREATE INDEX idx_summary_dependencies_consumer
    ON summary_dependencies(consumer_variant_id, consumer_component);
```

`sensitivity` describes how the consumer reacts to producer changes:

```text
SEMANTIC
EVIDENCE_ONLY
IDENTITY
CONFIGURATION
```

Example:

```text
consumer: decodeIE.value_flow
producer: validateIE.range
kind: SUMMARY_COMPONENT
sensitivity: SEMANTIC
```

## 10.3 Dependency Producer IDs

Producer IDs can be:

```text
function_variant_id
function_summary_id
type_layout_id
global_symbol_id
external_model_id
build_variant_id
analyzer_run_id
fact_id
```

For long-lived reverse indexing, dependency edges SHOULD target stable producer entities plus component names, not only the current `function_summary_id`.

---

# 11. Reverse Dependency Indexing

The reverse dependency index answers:

> Given a changed producer and a changed component set, which consumers must be reconsidered?

It MUST be more precise than "all callers."

## 11.1 Reverse Index Table

```sql
CREATE TABLE reverse_dependency_index (
    producer_kind          TEXT NOT NULL,
    producer_id            TEXT NOT NULL,
    producer_component     TEXT NOT NULL,
    consumer_kind          TEXT NOT NULL,
    consumer_id            TEXT NOT NULL,
    consumer_component     TEXT NOT NULL,
    dependency_kind        TEXT NOT NULL,
    sensitivity            TEXT NOT NULL,
    last_observed_summary_id TEXT,
    PRIMARY KEY(
        producer_kind,
        producer_id,
        producer_component,
        consumer_kind,
        consumer_id,
        consumer_component,
        dependency_kind
    )
);

CREATE INDEX idx_reverse_dependency_lookup
    ON reverse_dependency_index(producer_kind, producer_id, producer_component);
```

This table can be derived from `summary_dependencies`, but V1 SHOULD materialize it because it is the hot path for incremental updates.

## 11.2 Consumer Subscription Model

Each analysis component declares subscriptions.

| Consumer Component | Producer Components It Cares About |
| --- | --- |
| `call_graph` | `calls`, `unknowns`, `assumptions` |
| `scc` | `calls` |
| `may_write` | `calls`, `memory_effects`, `alias` |
| `global_value_flow` | `calls`, `value_flow`, `alias`, `memory_effects` |
| `range_wpa` | `calls`, `range`, `value_flow`, `assumptions` |
| `taint_wpa` | `calls`, `taint`, `value_flow`, `unknowns` |
| `ownership_wpa` | `calls`, `ownership`, `memory_effects`, `alias` |
| `evidence_slice` | all cited components, plus `provenance` |

When a summary delta arrives:

```text
changed_components = {range}

reverse_dependency_index.users(
    producer = changed_function_variant,
    producer_component in {range}
)
```

This returns only consumers that actually use range information.

## 11.3 Stale Edge Reconciliation

After a consumer recomputes, its dependency set may change.

Publication MUST:

```text
1. Remove reverse index rows associated with old current summary.
2. Insert reverse index rows for new current summary.
3. Keep historical dependency rows for old summary objects.
```

Historical rows are useful for explaining old evidence and diffs, but the hot reverse index should point at current dependencies for the selected revision/build.

---

# 12. Local Summary Build Pipeline

## 12.1 Inputs

```text
translation_unit_id
function_variant_id
function_body_id
analyzer_run_id
current imported summaries
external semantic models
analysis configuration
```

## 12.2 Steps

```text
1. Load Clang semantic AST and source map.
2. Load or generate LLVM IR for the translation unit.
3. Extract function declarations, definitions, and source anchors.
4. Compute body hashes.
5. Run local call, CFG, memory, value-flow, range, alias, and unknown analysis.
6. Normalize output into FunctionSummary components.
7. Compute component semantic and evidence hashes.
8. Compute FunctionSummaryID from canonical summary bytes.
9. Write immutable summary object.
10. Publish metadata binding and dependency indexes.
```

## 12.3 Local Summary Boundary

Local summaries SHOULD record direct effects and local transfer functions. They SHOULD NOT eagerly expand all callees.

Example direct summary:

```yaml
function: Decoder::decodeIE
calls:
  - validateIE
  - copyPayload
memory:
  reads:
    - arg0.length
    - arg1.state
  writes:
    - arg1.state
value_flow:
  - arg0.length -> call.copyPayload.arg2
range:
  arg0.length:
    min: 0
    max: 65535
unknowns:
  - external_model_missing: vendorValidate
dependencies:
  - type_layout: Packet
  - type_layout: Context
```

WPA is responsible for recursive transitive effects.

---

# 13. Incremental Update Pipeline

The incremental update pipeline is worklist-driven.

```text
Source change
    -> changed files
    -> affected translation units
    -> changed functions
    -> local summary rebuild
    -> component delta
    -> reverse dependency lookup
    -> SCC scheduling
    -> fixpoint propagation
    -> fact delta publication
    -> affected evidence case invalidation
```

## 13.1 Update Event Tables

```sql
CREATE TABLE update_events (
    update_event_id        TEXT PRIMARY KEY,
    revision_old_id        TEXT REFERENCES revisions(revision_id),
    revision_new_id        TEXT NOT NULL REFERENCES revisions(revision_id),
    build_variant_id       TEXT NOT NULL REFERENCES build_variants(build_variant_id),
    started_at             INTEGER NOT NULL,
    completed_at           INTEGER,
    status                 TEXT NOT NULL
);

CREATE TABLE summary_deltas (
    delta_id               TEXT PRIMARY KEY,
    update_event_id        TEXT NOT NULL REFERENCES update_events(update_event_id),
    function_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    old_summary_id         TEXT,
    new_summary_id         TEXT NOT NULL,
    semantic_changed       INTEGER NOT NULL,
    evidence_changed       INTEGER NOT NULL
);

CREATE TABLE component_deltas (
    delta_id               TEXT NOT NULL REFERENCES summary_deltas(delta_id),
    component_kind         TEXT NOT NULL,
    old_semantic_hash      TEXT,
    new_semantic_hash      TEXT NOT NULL,
    old_evidence_hash      TEXT,
    new_evidence_hash      TEXT NOT NULL,
    change_kind            TEXT NOT NULL,
    PRIMARY KEY(delta_id, component_kind)
);
```

## 13.2 Worklist Item

```text
WorkItem {
    kind:
        LOCAL_SUMMARY
        SCC_RECOMPUTE
        WPA_COMPONENT
        FACT_DERIVATION
        EVIDENCE_INVALIDATION

    target_id
    revision_id
    build_variant_id
    triggering_delta_ids[]
    priority
    attempt_count
}
```

## 13.3 Local Rebuild Pseudocode

```cpp
SummaryDelta rebuildLocal(FunctionVariantID f) {
    SummaryID old_id = currentSummary(f);
    FunctionSummary old_summary = loadSummary(old_id);

    FunctionSummary fresh = analyzeLocal(f);
    SummaryID new_id = putSummaryIfAbsent(fresh);

    if (old_id == new_id)
        return SummaryDelta::none(f, old_id);

    ComponentDeltaSet delta = diffComponents(old_summary, fresh);
    publishSummaryBinding(f, new_id, delta);

    return SummaryDelta(f, old_id, new_id, delta);
}
```

## 13.4 Dependency Scheduling Pseudocode

```cpp
void scheduleConsumers(SummaryDelta delta) {
    for (ComponentDelta c : delta.changed_components) {
        auto users = reverseIndex.users(
            ProducerKind::FunctionVariant,
            delta.function_variant_id,
            c.component_kind);

        for (Consumer u : users) {
            if (u.sensitivity == EVIDENCE_ONLY && !c.evidence_changed)
                continue;

            enqueue(workItemFor(u, delta));
        }
    }
}
```

The scheduler MUST deduplicate pending work items by:

```text
kind
target_id
revision_id
build_variant_id
consumer_component
```

---

# 14. SCC-Aware WPA

Recursive and mutually recursive functions require SCC-level propagation.
VERITAS owns call-graph construction and reverse-topological scheduling, while
compiled Souffle owns the recursive production evaluation within each logical
component. Pinned SVF supplies normalized indirect-call candidates; unknown
calls remain explicit and never fan out to the entire program.

## 14.1 SCC Tables

```sql
CREATE TABLE wpa_sccs (
    analysis_run_id        TEXT NOT NULL REFERENCES analysis_run_manifests(analysis_run_id),
    scc_id                 TEXT NOT NULL,
    call_graph_hash        TEXT NOT NULL,
    member_hash            TEXT NOT NULL,
    created_at             INTEGER NOT NULL,
    PRIMARY KEY(analysis_run_id, scc_id)
);

CREATE TABLE wpa_scc_members (
    analysis_run_id        TEXT NOT NULL,
    scc_id                 TEXT NOT NULL,
    function_variant_id    TEXT NOT NULL REFERENCES function_variants(function_variant_id),
    PRIMARY KEY(analysis_run_id, scc_id, function_variant_id),
    FOREIGN KEY(analysis_run_id, scc_id)
        REFERENCES wpa_sccs(analysis_run_id, scc_id)
);

CREATE TABLE wpa_scc_edges (
    analysis_run_id        TEXT NOT NULL,
    from_scc_id            TEXT NOT NULL,
    to_scc_id              TEXT NOT NULL,
    edge_kind              TEXT NOT NULL,
    epistemic              TEXT NOT NULL,
    PRIMARY KEY(analysis_run_id, from_scc_id, to_scc_id, edge_kind),
    FOREIGN KEY(analysis_run_id, from_scc_id)
        REFERENCES wpa_sccs(analysis_run_id, scc_id),
    FOREIGN KEY(analysis_run_id, to_scc_id)
        REFERENCES wpa_sccs(analysis_run_id, scc_id)
);

CREATE TABLE wpa_scc_component_state (
    analysis_run_id        TEXT NOT NULL,
    scc_id                 TEXT NOT NULL,
    component_kind         TEXT NOT NULL,
    logical_input_hash     TEXT NOT NULL,
    fixpoint_hash          TEXT,
    external_hash          TEXT,
    engine_toolchain_identity TEXT NOT NULL REFERENCES engine_toolchains(engine_toolchain_identity),
    result_object_ref      TEXT,
    iteration_count        INTEGER,
    status                 TEXT NOT NULL,
    diagnostics_object_ref TEXT,
    completed_at           INTEGER,
    PRIMARY KEY(analysis_run_id, scc_id, component_kind),
    FOREIGN KEY(analysis_run_id, scc_id)
        REFERENCES wpa_sccs(analysis_run_id, scc_id)
);
```

The component row's toolchain identity must equal its `AnalysisRun` manifest.
A successful row requires `fixpoint_hash`, `external_hash`, and an immutable
`result_object_ref`. Pending or failed rows leave those successful-result
fields null and retain status/diagnostics, so they cannot replace a prior
success. Cache reuse creates a new per-run component row that references the
validated immutable result object; it never reuses or merges another run's row.

## 14.2 SCC Construction

The SCC builder consumes call edges from current summaries.

Call edges have epistemic labels:

```text
MUST_CALL
MAY_CALL
UNKNOWN_CALL
```

V1 SCC construction SHOULD include `MUST_CALL` and `MAY_CALL`. `UNKNOWN_CALL` edges should not create arbitrary full-program SCCs; they should produce unknown facts and conservative external-call summaries.

The call graph hash is computed from canonical call edges:

```text
caller_function_variant_id
callee_function_variant_id or external_target_id
callsite_identity
dispatch_kind
epistemic
```

## 14.3 Condensation DAG

After SCCs are built:

```text
function call graph -> SCC condensation DAG
```

WPA propagation processes SCCs in reverse topological order for bottom-up summaries, then schedules affected predecessors when externally visible SCC summaries change.

## 14.4 Fixpoint Requirements

Each WPA component defines a versioned relation/rule/model contract and one
canonical `WpaLogicalComponentInput` containing member facts, stable/typed-dense
mappings, outgoing calls, successor support facts, and applicable models. The
same byte-identical logical input is placed into distinct production and C++
conformance/emergency execution envelopes with distinct `RunId` values.

The first production domains are:

```text
ReachableCall
MayWrite
```

M10A later adds `MayRead`, `GlobalFlow`, `UnknownEffect`, and
`SoundnessCoverage` through independent bundles and conformance suites.

## 14.5 SCC Execution Pseudocode

```cpp
StatusOr<WpaComponentResult> executeComponent(SccID scc,
                                               ComponentKind component) {
    WpaLogicalComponentInput logical = materializeCanonicalInput(scc, component);
    WpaExecutionEnvelope envelope = makeSouffleEnvelope(currentRun(), logical);
    auto output = souffleExecutor().execute(envelope);
    return validateAndCanonicalize(logical, output);
}
```

The normal path never catches a Souffle failure and invokes C++. C++ execution
requires a separate conformance envelope or explicit `cpp-emergency`
configuration and cannot impersonate or overwrite a Souffle run.

## 14.6 SCC Delta Rule

After an SCC fixpoint converges:

```text
old_external_hash == new_external_hash
    -> stop propagation

old_external_hash != new_external_hash
    -> schedule predecessor SCCs that consume changed components
```

`LogicalInputHash` covers canonical semantic inputs but excludes revision,
`RunId`, engine identity, and tuple order. `FixpointHash` covers canonical results
and selected witnesses. `ExternalHash` covers predecessor-visible semantics
only. A witness-only change may alter `FixpointHash` but not canonical
fact/root IDs or `ExternalHash`, and therefore does not schedule predecessors.

Successful immutable components may be reused across revisions only by
`(LogicalInputHash, EngineToolchainIdentity)`, after content, schema, bundle,
rooted-input, and exact executor provenance revalidation. A failure publishes no
replacement; it records diagnostics and leaves the previous success stale.

---

# 15. WPA Fact Engine

The fact engine materializes global semantic facts from local summaries and SCC states.

## 15.1 Fact Classes

| Class | Example Predicate |
| --- | --- |
| Call reachability | `reachable_call(F, G)` |
| Memory effect | `may_write(F, M)` |
| Value flow | `flows_to(V1, V2)` |
| Range | `range(V, Lower, Upper)` |
| Alias | `may_alias(M1, M2)` |
| Taint | `taint_flows(Source, Sink)` |
| Ownership | `may_free(F, M)` |
| Lock | `may_hold(F, L)` |
| Unknown | `unknown_external_call(F, Site)` |

## 15.2 Canonical Facts and Run Bindings

```sql
CREATE TABLE analysis_facts (
    fact_id                TEXT PRIMARY KEY,
    relation_schema_version TEXT NOT NULL,
    relation_name          TEXT NOT NULL,
    typed_semantic_cells   BLOB NOT NULL,
    epistemic              TEXT NOT NULL
);

CREATE TABLE run_fact_bindings (
    analysis_run_id        TEXT NOT NULL REFERENCES analysis_run_manifests(analysis_run_id),
    fact_id                TEXT NOT NULL REFERENCES analysis_facts(fact_id),
    confidence             TEXT NOT NULL,
    producer_kind          TEXT NOT NULL,
    analyzer_run_id        TEXT REFERENCES analyzer_runs(analyzer_run_id),
    scope_kind             TEXT,
    scope_id               TEXT,
    selected_witness_id    TEXT,
    is_current             INTEGER NOT NULL,
    created_at             INTEGER NOT NULL,
    PRIMARY KEY(analysis_run_id, fact_id)
);

CREATE INDEX idx_analysis_facts_relation
    ON analysis_facts(relation_schema_version, relation_name);
```

`typed_semantic_cells` is the canonical typed stable-cell encoding supplied by
the Fact Bus. Subject indexes may project selected schema columns, but they do
not redefine identity. Current/history state, producer, confidence, scope,
timestamps, and witness selection belong only to `run_fact_bindings`.

## 15.3 Fact Identity

`MakeFact` computes `FactID` from exactly:

```text
relations.v2
relation name
typed stable semantic cells
epistemic
```

Revision, build, analyzer/run/engine identity, dense IDs, tuple order, producer,
scope, rule, witness, provenance, and derivation do not participate. The store
revalidates the incoming ID against these bytes and never re-identifies an
`AnalysisFact`. The same semantic row derived by another proof path keeps the
same `FactID`; `(RunId, FactID)` and its selected/alternative witness records
preserve occurrence and explanation history.

---

# 16. Provenance-Aware Derived Facts

## 16.1 Provenance Model

Every non-trivial fact has a derivation DAG:

```text
input fact(s) or summary component(s)
    -> rule / transfer function / analyzer
    -> output fact
```

Provenance answers:

```text
Why is this fact true?
Which source locations contributed?
Which summary objects were used?
Which rule derived it?
What analyzer version and config produced it?
What epistemic assumptions were required?
```

## 16.2 Provenance Tables

```sql
CREATE TABLE fact_witnesses (
    analysis_run_id        TEXT NOT NULL REFERENCES analysis_run_manifests(analysis_run_id),
    fact_id                TEXT NOT NULL REFERENCES analysis_facts(fact_id),
    witness_id             TEXT NOT NULL,
    selected               INTEGER NOT NULL,
    producer_kind          TEXT NOT NULL,
    producer_id            TEXT NOT NULL,
    rule_id                TEXT,
    rule_version           TEXT,
    analyzer_run_id        TEXT REFERENCES analyzer_runs(analyzer_run_id),
    source_anchor_id       TEXT REFERENCES source_anchors(source_anchor_id),
    summary_id             TEXT REFERENCES summary_objects(function_summary_id),
    description            TEXT,
    PRIMARY KEY(analysis_run_id, fact_id, witness_id)
);

CREATE TABLE fact_witness_edges (
    analysis_run_id        TEXT NOT NULL,
    output_fact_id         TEXT NOT NULL,
    witness_id             TEXT NOT NULL,
    input_kind             TEXT NOT NULL,
    input_id               TEXT NOT NULL,
    input_ordinal          INTEGER NOT NULL,
    PRIMARY KEY(analysis_run_id, output_fact_id, witness_id, input_ordinal),
    FOREIGN KEY(analysis_run_id, output_fact_id, witness_id)
        REFERENCES fact_witnesses(analysis_run_id, fact_id, witness_id)
);

CREATE INDEX idx_fact_witness_edges_input
    ON fact_witness_edges(input_kind, input_id);
```

Each `(RunId, FactID)` has one selected witness and may retain alternatives.
Changing that selection may alter `FixpointHash`, but it never changes the
canonical fact/root IDs or `ExternalHash`.

Input kinds include:

```text
FACT
SUMMARY
SUMMARY_COMPONENT
SOURCE_ANCHOR
TYPE_LAYOUT
BUILD_CONFIG
EXTERNAL_MODEL
ASSUMPTION
HYPOTHESIS
```

## 16.3 Epistemic Propagation

The fact engine MUST conservatively propagate epistemic state.

Baseline ordering:

```text
MUST
MAY
MUST_NOT
INFERRED
ASSUMED
UNKNOWN
```

This is not a total confidence order. It is a semantic classification.

Rules:

```text
MUST + sound rule -> MUST
MAY + sound rule -> MAY
ASSUMED input -> output records assumption dependency
INFERRED input -> output remains INFERRED unless verified
UNKNOWN input -> output is UNKNOWN or MAY, according to transfer policy
```

An LLM-created hypothesis can be stored as `INFERRED`, but it cannot produce a `MUST` fact without a verifier result.

## 16.4 Explain API

`explainFact(run_id, fact_id)` returns:

```text
Fact
Producer
Rule
Epistemic state
Immediate inputs
Recursive provenance closure, budgeted
Source anchors
Summary component IDs
Assumptions
Unknowns
```

The API must support budgets:

```text
max_depth
max_nodes
include_source_anchors
include_summary_ids
include_datalog_derivation
```

Evidence IR should cite facts and provenance IDs rather than copying every derivation by default.

---

# 17. Datalog Integration

Compiled Souffle is the required normal production recursive engine after
M8R.4. Before execution, the adapter applies §7's configured-manifest and
executable-digest verification for Souffle 2.5 source revision
`5682a9f12e2668ecdd26348fe63cc508bc0fcf47`. The verified manifest, executable,
generated bundle, and generator/compiler/link provenance form
`EngineToolchainIdentity`. Generated programs use one evaluation thread until a
separately qualified upgrade retires the upstream ARM concurrency issue.

C++ consumes the same byte-identical engine-neutral logical component input
only as a CI conformance oracle or explicitly selected `cpp-emergency` engine.
There is no automatic fallback, and the two executions have distinct valid
envelopes and `RunId` values. A C++ envelope records the exact C++ build
identity in its engine-specific provenance payload and never reuses or
impersonates the verified Souffle payload.

## 17.1 Base Relations

These typed relations are a `relations.v2` run-local projection from durable
Function Summary IR, not a durable platform IR. The live EDB signatures are:

```text
DirectCall(CallSiteId, CallerId, CalleeId, DispatchKind, Epistemic)
UnknownCall(CallSiteId, CallerId, ReasonId, Epistemic)
DirectRead(FunctionId, MemoryId, RangeKind, Offset, Size, Epistemic)
DirectWrite(FunctionId, MemoryId, RangeKind, Offset, Size, Epistemic)
Alias(MemoryId, MemoryId, AliasKind, Epistemic)
LocalFlow(FunctionId, SourceId, DestinationId, FlowKind, Epistemic)
ParameterFlow(CallSiteId, ActualId, FormalId, Epistemic)
ReturnFlow(CallSiteId, ReturnId, ResultId, Epistemic)
ModeledEffect(ModelId, FunctionId, EffectKind, SubjectId, Epistemic)
UnsupportedFeature(NodeId, FeatureKind, SoundnessPolicy)
SupportReachableCall(SourceId, TargetId, Epistemic)
SupportMayWrite(FunctionId, MemoryId, Epistemic)
```

The last two relations are EDB-only successor support. `RangeKind`, signed
`Offset`, and unsigned `Size` are lossless payloads; rules may ignore those
payload columns only when the derived relation intentionally abstracts range.

## 17.2 Derived Relations

```souffle
.decl ReachableCall(source:FunctionId, target:FunctionId, epistemic:Epistemic)
.decl MayWrite(function:FunctionId, memory:MemoryId, epistemic:Epistemic)

ReachableCall(f, g, e) :- DirectCall(_, f, g, _, e).
ReachableCall(f, h, e) :-
    DirectCall(_, f, g, _, e1), ReachableCall(g, h, e2),
    WeakenEpistemic(e1, e2, e).
ReachableCall(f, h, e) :-
    DirectCall(_, f, g, _, e1), SupportReachableCall(g, h, e2),
    WeakenEpistemic(e1, e2, e).

MayWrite(f, m, e) :- DirectWrite(f, m, _, _, _, e).
MayWrite(f, m, e) :-
    DirectCall(_, f, g, _, e1), MayWrite(g, m, e2),
    WeakenEpistemic(e1, e2, e).
MayWrite(f, m, e) :-
    DirectCall(_, f, g, _, e1), SupportMayWrite(g, m, e2),
    WeakenEpistemic(e1, e2, e).
```

Both IDBs have exactly three columns: `(source, target, epistemic)` for
`ReachableCall` and `(function, memory, epistemic)` for `MayWrite`. The
`MayWrite` base rule consumes `DirectWrite`'s full six-column row while
deliberately abstracting its range payload.

## 17.3 Provenance Capture

Every rule bundle exports generic immediate witness edges:

```text
Witness(ResultSemanticKey, RuleId, InputSemanticKey, InputOrdinal)
```

Semantic keys use a shared versioned, injective, type-tagged, length-prefixed
UTF-8 codec. Raw delimiter concatenation is forbidden. Base and successor
support inputs are rooted in stable input fact IDs; locally derived keys must
resolve within the published component.

The VERITAS canonicalizer rejects malformed, cyclic, orphaned, or unclosed
witnesses and selects one finite proof by derived-edge count, versioned rule
priority, then lexicographic stable input identity. Relation-specific C++ proof
reconstruction is forbidden on the production path. Souffle interactive
provenance remains a debugging aid only.

## 17.4 Semantic and identity preservation

Stable identity and typed dense IDs are separate. `RangeKind` losslessly
distinguishes known signed offsets/unsigned sizes (including zero) from explicit
unknown ranges. All six epistemic states and all alias kinds cross the boundary,
so `NO_ALIAS + MUST`, an unknown alias result, and an absent tuple remain
distinct.

---

# 18. Publication and Consistency

## 18.1 Object Store Contract

```text
put_if_absent(key, bytes) -> object_store_key
get(key) -> bytes
exists(key) -> bool
```

The object store key SHOULD be the content hash. A corrupt or mismatched object is fatal for that analysis run.

## 18.2 Metadata Transaction

Metadata publication MUST be atomic:

```text
BEGIN;
  insert summary_objects;
  insert summary_components;
  insert summary_dependencies;
  update summary_bindings old current -> not current;
  insert summary_bindings new current;
  update reverse_dependency_index;
  insert summary_deltas/component_deltas;
COMMIT;
```

If the process crashes after CAS write but before metadata commit, the object is unreachable garbage and can be collected later.

If it crashes after commit, readers must be able to load the object by key.

## 18.3 Snapshot Reads

Query and WPA readers SHOULD use:

```text
revision_id
build_variant_id
publication_epoch <= snapshot_epoch
```

This prevents long-running queries from observing mixed old/new summary bindings.

## 18.4 WPA batch publication

A production component publishes only after execution, schema/identity
validation, and rooted-witness closure all succeed. Missing or incompatible
bundles, engine failure, timeout, resource exhaustion, invalid mappings,
duplicate conflict, or malformed witnesses record an incomplete run and
durable diagnostics but no replacement facts. The prior success remains
queryable as stale history.

Only a successful `WpaRunResult` creates `AnalysisFactBatch`. The batch includes
the manifest's expected component keys, completed component records and hashes,
rooted input fact IDs, canonical facts, witnesses, and diagnostics. The Fact Bus
rejects expected/completed mismatch and witness leaves absent from the rooted
set. Delivery is idempotent at least once by canonical `(RunId, BatchId)`;
per-sink progress makes retry after partial fan-out safe and non-duplicating.

---

# 19. Query and Service APIs

## 19.1 Summary API

```text
getFunctionSymbol(query) -> FunctionSymbolID[]
getFunctionVariant(symbol_id, build_variant_id) -> FunctionVariantID
getCurrentSummary(function_variant_id, revision_id, build_variant_id) -> FunctionSummaryID
getSummary(summary_id, components[]) -> FunctionSummary
getSummaryComponentHash(summary_id, component_kind) -> ComponentHash
diffSummary(old_summary_id, new_summary_id) -> SummaryDelta
```

## 19.2 Dependency API

```text
getDependencies(summary_id) -> DependencyEdge[]
getReverseDependencies(producer_id, component_kind) -> Consumer[]
getImpactSet(delta_id, budget) -> ImpactGraph
```

## 19.3 WPA API

```text
getScc(function_variant_id, revision_id, build_variant_id) -> SccID
getSccMembers(scc_id) -> FunctionVariantID[]
getSccState(scc_id, component_kind) -> StateID
recomputeScc(scc_id, component_kind) -> SccResult
getDerivedFacts(subject_id, predicate_kind) -> Fact[]
```

## 19.4 Provenance API

```text
explainFact(run_id, fact_id, budget) -> ProvenanceGraph
explainSummaryComponent(summary_id, component_kind, budget) -> ProvenanceGraph
getFactInputs(run_id, fact_id) -> InputRef[]
```

## 19.5 Evidence Builder API

```text
getValueFlow(src, dst, budget) -> FlowSlice
getRange(value_ref) -> Fact[]
getMayWrites(function_ref) -> Fact[]
getAliases(memory_ref) -> Fact[]
getCallPath(src_function, dst_function, budget) -> PathSlice
getDominatingChecks(callsite_ref, predicate_kind) -> Fact[]
getUnknowns(scope_ref) -> Unknown[]
```

These APIs are semantic. They should not expose SQL, RocksDB keys, or graph storage internals.

---

# 20. Initial CLI Workflow

The current pre-M11 command-line workflow proves the backbone without an LLM
or user-managed compiler-analysis preprocessing:

```bash
veritas-build analyze --project <project-directory>
```

`<project-directory>/compile_commands.json` is mandatory. Build Intelligence,
Clang AST extraction, LLVM IR generation/linking, required in-process SVF
analysis, and summary publication execute as internal stages of this command.
A diagnostic manifest or cached IR may be emitted under `.veritas`, but neither
is a public input to a later command.

M11 adds the mutually exclusive module-input form:

```bash
veritas-build analyze --bitcode <file.bc|file.ll|directory>
```

It enters at VERITAS-owned module acquisition (skipping only Clang CodeGen),
then runs the same local extraction, required SVF, Summary IR, WPA, and
provenance pipeline. It never accepts an SVF artifact or a third-party analysis
result. M12's separate external-facts import remains non-authoritative and
terminal: imported observations do not become Summary IR or recursive-WPA
inputs.

Expected output:

```text
Translation Units:        8421
Function Symbols:       692812
Function Variants:      681221
Summaries Written:      681221
Deduped Summaries:       18441
Unknown Calls:            8231
Reverse Dependencies:  5029183
```

```bash
veritas-diff HEAD~1 HEAD
```

Expected output:

```text
Source changed functions:           214
Semantic body changed functions:     51
Summary changed functions:           37

Changed components:
  calls:                              4
  memory_effects:                    12
  value_flow:                        18
  range:                              9
  unknowns:                           3

WPA affected SCCs:                   23
WPA affected functions:             186
Evidence cases stale:                 7
```

```bash
veritas-explain fact <fact_id> --run <run_id>
```

Expected output:

```text
Fact: may_write(Decoder::decodeIE, ctx.state)
Epistemic: MAY
Producer: wpa.memory_effects
Derived by: transitive_may_write_v1

Inputs:
  Decoder::decodeIE calls copyPayload
  copyPayload direct_write ctx.state

Source:
  decoder.cpp:281
  payload.cpp:144
```

---

# 21. Testing Strategy

## 21.1 Unit Tests

* ID canonicalization.
* Stable hashing under field reordering.
* Function symbol identity for overloads, templates, static functions, and methods.
* Build variant identity changes for macros, ABI, type layout, and target triple.
* Component hash diffing.
* Dependency edge construction.
* Epistemic propagation rules.

## 21.2 Golden Tests

Use tiny C/C++ fixtures with expected summaries:

```text
direct call
indirect call
virtual dispatch
template function
static internal-linkage function
macro-dependent behavior
range guard before memcpy
range guard not dominating memcpy
recursive pair
external unknown call
```

Golden outputs should include:

```text
FunctionSymbolID
FunctionVariantID
component hashes
summary dependencies
derived facts
provenance graph
```

## 21.3 Incremental Tests

Each test mutates one fixture and checks expected propagation:

| Mutation | Expected |
| --- | --- |
| Comment only | no semantic body change |
| Reformat expression | maybe source hash only |
| Change range constant | range component delta only |
| Add call | call component delta, SCC update |
| Change callee memory write | effect consumers scheduled |
| Change recursive member internal detail | stop if SCC external hash unchanged |
| Improve provenance only | evidence refresh, no analysis invalidation |

## 21.4 Crash Consistency Tests

Simulate crashes:

* after object store write before metadata insert,
* after summary object row before binding update,
* after binding update before reverse index update,
* during SCC state publication,
* during fact publication.

Recovery must leave either the old current binding or the new complete binding visible, never a partial binding.

---

# 22. V1 Milestones

## Milestone 1: Identity and CAS Summary Store

Deliver:

* repository/revision/build tables,
* function symbol and variant identity,
* function body hashes,
* Protobuf summary object skeleton,
* RocksDB CAS store,
* summary binding publication,
* component hash table.

Exit criteria:

```text
veritas-build analyze --project tests/fixtures/projects/small_fixture
veritas-query summary <function>
veritas-diff same_revision same_revision -> no summary deltas
```

## Milestone 2: Local Analysis Summaries

Deliver:

* direct calls,
* CFG reachability and dominator facts,
* basic memory read/write effects,
* local value-flow edges,
* simple range facts,
* unknown external calls,
* dependency edges.

Exit criteria:

```text
Changing one range check changes only range/value-flow-sensitive summaries.
Changing one callsite changes call component and SCC inputs.
```

## Milestone 3: Reverse Dependency Index and Incremental Scheduler

Deliver:

* materialized reverse dependency index,
* summary delta tables,
* worklist scheduler,
* consumer subscription masks,
* stale edge reconciliation.

Exit criteria:

```text
veritas-diff reports affected consumers by component.
Propagation is narrower than all-callers invalidation on fixture tests.
```

## Milestone 4: SCC/Fixpoint WPA

Deliver:

* call graph SCC detection,
* condensation DAG,
* compiled-Souffle execution for `ReachableCall` and `MayWrite`,
* byte-identical logical input for the C++ conformance/explicit emergency engine,
* SCC component state hashes,
* stop propagation when external SCC hash is unchanged.

Exit criteria:

```text
Recursive fixtures converge.
Internal recursive changes do not propagate upstream if external summary is stable.
Missing or failed Souffle publishes no replacement and never falls back silently.
```

## Milestone 5: Provenance-Aware Fact Store

Deliver:

* fact table,
* provenance nodes and edges,
* `explainFact`,
* epistemic propagation,
* generic finite rooted witness storage from the engine-neutral witness contract,
* complete `AnalysisFactBatch` and idempotent Fact Bus publication.

Exit criteria:

```text
Every derived fact in V1 has an explainable derivation graph.
Facts derived from unknown or inferred premises retain correct epistemic state.
```

## Milestone 6: Evidence Builder Input API

Deliver:

* value-flow slice API,
* call path API,
* range and memory fact queries,
* unknown retrieval,
* provenance budgeted expansion.

Exit criteria:

```text
A buffer-overflow fixture can produce all data needed for an EIR-L1 case without loading full source files.
```

---

# 23. Key Engineering Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Function identity instability | Cache misses, false diffs, broken historical references. | Build dedicated identity golden tests early. |
| Component hashes too coarse | Over-invalidation. | Track semantic hashes per component from V1. |
| Component hashes too fine | Hash churn and difficult debugging. | Define canonical payloads and avoid source-location-only hash input. |
| Reverse index drift | Missed invalidations or extra stale work. | Rebuild index from current dependencies as a validation command. |
| SCC explosion from unknown calls | Whole program becomes one component. | Model unknown calls conservatively without arbitrary graph edges. |
| Provenance storage bloat | Fact store becomes expensive. | Store immediate provenance by default, expand recursively with budgets, dedupe provenance nodes. |
| Analyzer version churn | Everything invalidates every time. | Separate semantic payload hashes from analyzer-run identity where safe, but never mix incompatible schemas. |
| Memory abstraction imprecision | Too many MAY facts. | Start simple, make unknown/may explicit, add demand-driven refinement later. |

---

# 24. Open Design Decisions

These should be resolved before implementation begins.

| Decision | Approved contract/default |
| --- | --- |
| Summary schema compatibility | Existing `summary.v1` artifacts remain immutable/readable; native reanalysis and new publication emit `summary.v2` only |
| WPA contract versions | `relations.v2` execution projection and `wpa-run.v1` manifest |
| M9 fact schema version | Defined by the M9 persistence implementation/specification; this design does not invent an additional fact-version name |
| Metadata store | SQLite |
| CAS store | RocksDB |
| Hash algorithm | SHA-256 |
| First production WPA components | `ReachableCall`, `MayWrite`; M10A later adds `MayRead`, `GlobalFlow`, `UnknownEffect`, `SoundnessCoverage` |
| First language target | C/C++ via Clang + LLVM |
| Recursive engine ownership | Compiled Souffle 2.5 at `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`; C++ conformance or explicit emergency only |
| Unknown call policy | emit unknown external summary, do not connect to every function |
| Evidence invalidation | summary/fact dependency based, not text-location based |

---

# 25. Final Backbone Definition

The VERITAS engineering backbone is:

> A versioned, content-addressed, provenance-preserving semantic summary and fact infrastructure that gives each function a stable build-aware identity, stores immutable summaries with independently hashed semantic components, indexes reverse dependencies by the exact component consumed, propagates whole-program facts through SCC-aware fixpoint computation, and records explainable derivations for every derived fact.

If this backbone is correct, the later Review Agent does not need to understand a whole repository from source text. It can ask VERITAS for a small, stable, provenance-backed semantic world and reason over Evidence IR without silently discarding uncertainty or proof obligations.
