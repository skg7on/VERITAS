# Soufflé WPA Architecture Refinement

**Status:** Approved design
**Date:** 2026-08-22
**Scope:** Refine the VERITAS local-analysis, relational-execution, recursive-WPA, and provenance boundaries without rewriting the completed M0–M8 milestone history.

## 1. Purpose

VERITAS already has the correct durable architectural center: immutable Function Summary IR, SummaryDB, semantic-delta scheduling, and a summary-driven WPA boundary. M8 added deterministic call and SCC graphs, a C++ fixed-point evaluator, stable fact tuples, persisted convergence hashes, and an optional file-based Soufflé comparison path.

The next milestones need stronger semantic inputs and a single production owner for recursive analysis. In particular, the current implementation does not yet provide collision-free abstract memory identities, complete indirect-call refinement, full uncertainty transport, a production Soufflé execution path, or a generic witness contract for derived facts.

This design introduces five remediation milestones between M8 and M9. They preserve the completed history while establishing a production architecture in which:

- Function Summary IR remains the durable WPA contract;
- LLVM-level relational facts are run-local execution projections, not another durable platform IR;
- pinned SVF remains the authoritative V1 points-to and indirect-call refinement engine;
- Soufflé is required for normal recursive WPA execution;
- the C++ fixed-point engine is retained only as a conformance oracle and explicitly selected emergency compatibility path;
- durable facts, identities, uncertainty, and provenance remain VERITAS-owned.

## 2. Decisions

The following decisions are authoritative for this refactor:

1. M0–M8 remain immutable historical milestones. Their specifications may receive forward links and status notes, but their delivered scope is not rewritten.
2. Function Summary IR remains the durable whole-program-analysis boundary, preserving platform principle P6.
3. A run-local relational projection may expose detailed program semantics to analyzers, but it is reconstructed rather than persisted as the platform source of truth.
4. SVF remains mandatory for V1 Andersen points-to analysis, SVFG construction, aliases, and indirect-call candidates.
5. Soufflé becomes the required production engine for recursive summary-level WPA.
6. The C++ engine consumes the same logical inputs as Soufflé and serves as a differential oracle. Emergency use requires explicit configuration and produces separately identified runs.
7. M9 does not begin until the M8 remediation entry criteria in this document pass.
8. A Soufflé-native points-to implementation is a later benchmark-gated research project, not part of the M9–M12 critical path.

## 3. Non-goals

This refactor does not:

- replace SVF in the production V1 analysis stack;
- make detailed LLVM facts the durable SummaryDB model;
- rewrite or relabel completed M0–M8 work;
- publish Soufflé-native tuple identities or engine-specific records;
- allow external facts to become authoritative WPA inputs;
- implement context-sensitive points-to analysis in Datalog;
- silently fall back from Soufflé to C++;
- publish partial fixed-point results after a failed or interrupted Soufflé evaluation.

## 4. Target Architecture

```text
Project or bitcode ingest
          |
          v
LLVM ProgramIr
private, run-local
          |
          v
Local extraction + SVF L1
          |
          v
Function Summary IR v2
stable, immutable, durable
          |
          v
SummaryDB
          |
          v
WPA input materializer
stable IDs -> typed dense IDs
models + uncertainty + per-SCC support
          |
          +--------------------------+
          |                          |
          v                          v
Compiled Soufflé WPA          C++ conformance oracle
required production          explicit emergency mode
          |                          |
          +-------------+------------+
                        v
Result and witness canonicalizer
                        |
                        v
Analysis Fact Bus
                        |
                        v
M9 fact/provenance store
                        |
                        v
Evidence and query APIs
```

The run-local relational layer is an execution projection. It does not become a fourth durable VERITAS IR. Ordinary WPA reads summaries; demand-driven refinement may reload `ProgramIr` and invoke a more precise local analysis tier.

## 5. Component Ownership

### 5.1 SVF

SVF owns V1 whole-program points-to calculation, SVFG construction, alias classification, and indirect-call candidates. SVF-native nodes, identifiers, and headers remain confined to `src/analysis/svf`.

SVF results must be normalized before leaving that boundary. The normalized result contains VERITAS stable references, dispatch kinds, alias kinds, epistemic states, abstract memory locations, dependencies, and explicit unknowns.

### 5.2 VERITAS semantic normalization

VERITAS owns:

- stable program identity;
- abstract objects, locations, subobjects, byte ranges, and access paths;
- call-site and dispatch semantics;
- external function models;
- unsupported-feature and soundness policy;
- Function Summary IR construction;
- uncertainty preservation.

Raw LLVM display names and placeholders such as `<local>`, `<unnamed>`, `<param>`, and `<call-effect>` may appear in diagnostics but cannot be semantic identity.

### 5.3 Soufflé

Soufflé owns recursive closure for summary-level WPA domains. The first production rule bundles remain `ReachableCall` and `MayWrite`; later bundles add `MayRead`, `GlobalFlow`, `UnknownEffect`, and `SoundnessCoverage`.

The supported build generates compiled Soufflé programs and links them behind a private VERITAS adapter. Soufflé public or generated types do not appear under `include/veritas/**`.

### 5.4 C++ conformance engine

The C++ implementation is not a second normal production owner. It:

- runs in CI against the same logical component inputs;
- detects semantic drift between transfer functions and Datalog rules;
- supports an explicitly requested emergency compatibility mode;
- records an engine identity distinct from Soufflé;
- cannot overwrite or impersonate a Soufflé run.

### 5.5 VERITAS result canonicalizer

VERITAS validates output schemas, reconstructs stable identities from run-local mappings, validates witness closure, selects deterministic finite witnesses, and computes durable fact IDs. Evaluation order and engine-native tuple identity never influence durable identity.

## 6. Versioned Data Contracts

The refactor introduces three contracts:

- `summary.v2` for normalized durable function semantics;
- `relations.v2` for typed WPA execution relations;
- `wpa-run.v1` for reproducible execution identity and status.

### 6.1 Analysis run manifest

Every execution records:

```text
AnalysisRun(
    RunId,
    RevisionId,
    BuildVariantId,
    SummarySchemaVersion,
    RelationSchemaVersion,
    RuleBundleVersion,
    ModelBundleVersion,
    SvfConfigurationHash,
    WpaConfigurationHash,
    EngineIdentity,
    EngineToolchainIdentity
)
```

All fields participate in `RunId`. `EngineToolchainIdentity` includes the qualified Soufflé source revision, executable digest, and generated bundle/toolchain provenance (or the C++ build identity). Results from different manifests cannot be merged into one run.

### 6.2 Dual identity

Stable identities remain durable. A materializer assigns typed unsigned IDs for hot joins:

```text
FunctionMap(FunctionId, FunctionStableId)
ValueMap(ValueId, ValueStableId)
MemoryMap(MemoryId, MemoryStableId)
CallSiteMap(CallSiteId, CallSiteStableId)
FactMap(FactId, FactStableId)
```

Dense IDs are meaningful only within one run. Missing mappings, duplicate dense IDs, stable-ID conflicts, and cross-domain ID use fail validation.

### 6.3 Semantic values and epistemic state

Semantic content is not encoded indirectly through epistemic state. For example:

```text
AliasKind = MUST_ALIAS | MAY_ALIAS | NO_ALIAS | UNKNOWN_ALIAS
Epistemic = MUST | MAY | MUST_NOT | INFERRED | ASSUMED | UNKNOWN
```

`AliasKind=NO_ALIAS, Epistemic=MUST` means the analysis deterministically established non-aliasing. It is distinct from an absent tuple or an unknown alias result.

The fact boundary preserves every platform epistemic state. Individual rule bundles declare which states they consume and produce; unsupported states are rejected or converted to explicit unknown relations according to the bundle contract, never silently dropped.

## 7. Initial Relation Schema

The initial EDB is grouped by semantic responsibility.

```text
Calls:
  DirectCall(CallSiteId, CallerId, CalleeId, DispatchKind, Epistemic)
  UnknownCall(CallSiteId, CallerId, ReasonId, Epistemic)

Memory:
  DirectRead(FunctionId, MemoryId, RangeKind, Offset, Size, Epistemic)
  DirectWrite(FunctionId, MemoryId, RangeKind, Offset, Size, Epistemic)
  Alias(MemoryId, MemoryId, AliasKind, Epistemic)

Flow:
  LocalFlow(FunctionId, SourceId, DestinationId, FlowKind, Epistemic)
  ParameterFlow(CallSiteId, ActualId, FormalId, Epistemic)
  ReturnFlow(CallSiteId, ReturnId, ResultId, Epistemic)

Models and coverage:
  ModeledEffect(ModelId, FunctionId, EffectKind, SubjectId, Epistemic)
  UnsupportedFeature(NodeId, FeatureKind, SoundnessPolicy)
```

The first IDB contains:

```text
ReachableCall
MayWrite
```

M10A adds:

```text
MayRead
GlobalFlow
UnknownEffect
SoundnessCoverage
```

Each relation has a registry entry specifying column names, types, key columns, ownership, allowed epistemic values, materialization policy, and schema version. `std::vector<std::string>` is not the authoritative schema representation for V2.

`RangeKind` is `KNOWN` or `UNKNOWN`. A known range carries its signed byte offset and unsigned size. An unknown range uses canonical zero payload cells that have no range meaning; validators reject non-zero payloads for `UNKNOWN` and reject half-known ranges before projection. The tag therefore distinguishes a known zero-length or zero-offset access from an unknown access without nullable Soufflé columns or sentinel inference.

## 8. Abstract Memory Model

The minimum V2 memory location is:

```text
MemoryLocation = AbstractObject + AccessPath + ByteRange
```

`AbstractObject` distinguishes global, stack, heap, argument, function, external, and unknown objects. `AccessPath` represents declared fields, array elements or ranges, and an explicit unknown suffix. `ByteRange` records known offset and size when available and an explicit unknown range otherwise.

Memory *identity* is narrower than the memory location value:

```text
MemoryLocationId = hash(AbstractObject + AccessPath)
```

`ByteRange` is an attribute of an access, not of the object being accessed, and is excluded from identity. No distinction is lost: a constant byte offset is already determined by the access path, so two accesses at different offsets keep different identities through their paths. Only the access size leaves identity, and the size describes the accessing instruction rather than the object.

This exclusion is required for the relation schema to be coherent. `DirectRead`/`DirectWrite` carry `range_kind`, `offset`, and `size` as columns beside `memory_id`, which would be redundant if identity already encoded them, and `MayWrite(function_id, memory_id, epistemic)` carries no range at all — with the range folded into identity, two writes at different offsets in one object would derive two distinct `MayWrite` facts and the question "does `f` write object `o`?" would have no single answer. Excluding it also makes a producer that knows the access size (local extraction) and one that does not (SVF, which passes an unknown size) agree on identity for the same object instead of splitting it in two.

The byte range remains recorded on the location and is hashed independently by the summary component digest, so per-access precision is preserved.

Allocation sites and stack objects receive stable identities derived from their owning function, stable source or semantic anchor, allocation kind, and a deterministic local fingerprint. Multiple unnamed allocations in the same function must never collapse into one object.

Opaque-pointer recovery may use GEP source types, load/store types, `DataLayout`, debug information, TBAA, allocation sizes, and SVF results. Missing evidence produces an explicit unknown or overlapping location rather than a fabricated field identity.

## 9. SCC-Scoped Execution

VERITAS continues to own call-graph construction and SCC scheduling. One execution unit evaluates one component for one SCC.

```text
WpaLogicalComponentInput:
  LogicalInputHash
  SccId
  ComponentKind
  Member identities
  Stable/dense mappings
  Member base facts
  Outgoing call edges
  Successor externally visible facts
  Applicable function models

WpaExecutionEnvelope:
  AnalysisRun manifest
  Engine/toolchain provenance
  WpaLogicalComponentInput
```

The logical component input is engine-neutral and has one canonical serialization. `LogicalInputHash` excludes revision, `RunId`, and engine identity; it covers the exact schema, rule, model, and semantic configuration versions plus the canonical stable EDB, mappings, member set, and successor external facts. Production and conformance executions derive separate envelopes and `RunId` values from that same immutable logical input. Each executor rejects an envelope whose engine identity does not match the executor. Differential qualification asserts byte-identical logical-input serialization and mappings before comparing engine-independent canonical facts.

SCCs execute in reverse topological order. Results from successor SCCs enter a component as explicit support relations carrying stable support fact identities. Rules may recursively derive facts for members of the current SCC and may cite successor support, but they cannot claim ownership of successor results.

The component output includes semantic result rows, witness rows, and diagnostics. Only current-SCC results cross the output boundary.

## 10. Incremental State

Each SCC component retains:

```text
LogicalInputHash
FixpointHash
ExternalHash
```

`LogicalInputHash` covers relation, rule, model, and semantic configuration versions; local semantic facts; member identity; mappings; and successor external hashes. It excludes revision, `RunId`, engine identity, and tuple order. `FixpointHash` covers the complete canonical result and selected witnesses. `ExternalHash` covers only predecessor-visible semantic results and is also engine-neutral.

A matching successful `LogicalInputHash` may reuse an immutable prior component result only when its exact executor/toolchain provenance and rule bundle also match. The cache key is `(LogicalInputHash, EngineToolchainIdentity)`; each new run manifest records its own reference to the content-addressed result object, so run histories are never merged. This permits validated cross-revision reuse when semantic inputs are unchanged while preventing C++ results or a different Soufflé build from satisfying a production Soufflé run. A witness-only change can update `FixpointHash` while leaving `ExternalHash` unchanged; in that case no predecessor is scheduled. An `ExternalHash` change enqueues the predecessor SCCs that consume that component.

Cache lookup always revalidates the object hash, logical input hash, executor/toolchain provenance, schema and bundle identities, and rooted inputs. Revision-only changes may reuse the object; any semantic input or qualified-toolchain change misses the cache.

## 11. Witness and Provenance Contract

Each rule bundle emits explicit immediate witness edges:

```text
Witness(
    ResultSemanticKey,
    RuleId,
    InputSemanticKey,
    InputOrdinal
)
```

Base and successor-support inputs are rooted in stable input fact identities. Locally derived inputs may initially reference semantic keys. The canonicalizer validates that every published result has a finite path to rooted inputs, rejects orphaned or malformed witnesses, and selects one proof using this ordering:

1. fewer derived edges;
2. lower versioned rule priority;
3. lexicographically ordered stable input identities.

The selected proof must be acyclic and closed over the published fact set and rooted input set. Alternative witnesses may be retained as optional diagnostic evidence but do not participate in the canonical fact ID.

Semantic keys use a versioned, injective length-prefixed UTF-8 codec shared by the C++ canonicalizer and a private Soufflé 2.5 stateful-functor library. Symbol, signed-number, and unsigned-number functors expose C-linkage entry points, encode one type-tagged self-delimiting field at a time, and are linked into the generated worker; only encoded fields may then be concatenated. Concatenation of raw values with an ambiguous delimiter is forbidden. Qualification invokes the generated program with adversarial cells containing delimiters, empty strings, Unicode, digit prefixes, and numeric bounds and requires byte-identical decoded keys from both engines.

This contract replaces relation-specific C++ reconstruction of Datalog joins. Soufflé's interactive provenance remains available for rule debugging but is not the durable VERITAS evidence protocol.

## 12. Failure and Publication Policy

A production Soufflé component either completes its fixed point and passes validation or produces no publishable component result.

The following conditions fail the component:

- missing or incompatible compiled rule bundle;
- input or output schema mismatch;
- invalid dense or stable identity mapping;
- Soufflé process or generated-program failure;
- resource exhaustion or orchestration timeout;
- malformed or unrooted witness graph;
- unsupported output epistemic state;
- inconsistent duplicate semantic rows.

Failure records durable diagnostics and marks the analysis run incomplete. The last successful result remains queryable but becomes stale for the new revision or configuration. Partial output never replaces a successful result and is not mixed with facts from a previous run.

Emergency C++ evaluation requires an explicit configuration value. Its `EngineIdentity` and `RunId` differ from Soufflé, and the result carries a degraded-operation diagnostic. Automatic fallback is forbidden.

## 13. Verification Strategy

Every remediation milestone adds tests at the narrowest boundary and retains end-to-end qualification.

Required suites include:

- summary-v1 compatibility and summary-v2 canonicalization;
- dense/stable-ID round trips and cross-domain type rejection;
- abstract object, subobject, offset, overlap, and unknown-location cases;
- direct, indirect, virtual, callback, and unknown-external calls;
- all four alias kinds and all platform epistemic states;
- golden EDB/IDB cases for every rule bundle;
- recursive SCC and successor-support behavior;
- deterministic results under input, member, and evaluation-order permutations;
- C++/Soufflé semantic differential tests;
- witness closure, canonical selection, and malformed-witness rejection;
- incremental cache reuse and predecessor invalidation;
- missing engine, incompatible bundle, timeout, crash, and stale-result behavior;
- repeated project analysis in one process;
- a recorded performance and memory baseline for representative fixtures.

Release qualification first requires byte-identical engine-neutral logical-input bytes, rooted-input identities, and dense/stable mappings. It then derives distinct Soufflé and C++ conformance execution envelopes and requires semantic equality for the overlapping domain corpus. Emergency-only behavior is tested separately and is never used to qualify the Soufflé engine.

The M9 handoff is constructed only from a successful `WpaRunResult`. Its immutable batch carries the expected component keys from the run manifest, completed component records and hashes, and rooted input fact IDs in addition to derived facts and witnesses. The Fact Bus rejects any expected/completed set mismatch or witness leaf absent from the rooted-input set. Multi-sink delivery is idempotent at least once under a canonical `(RunId, BatchId)` key; partial fan-out is recorded per sink and safe retry cannot duplicate logical publication.

## 14. Repository Boundaries

The intended structure is:

```text
include/veritas/analysis/semantic/
  abstract objects, memory locations, dispatch, uncertainty

include/veritas/facts/
  RelationSchema, AnalysisRun, AnalysisFact, Witness, FactBus

include/veritas/wpa/
  WpaExecutor, WpaLogicalComponentInput,
  WpaExecutionEnvelope, WpaComponentResult

src/analysis/svf/
  SVF-native mapping only

src/analysis/semantic/
  stable semantic normalization

src/facts/
  materialization, validation, canonicalization

src/wpa/
  SCC orchestration, hashes, scheduling,
  SouffleWpaExecutor, CppConformanceExecutor

logic/
  schema, common, reachability, memory_effects,
  global_flow, models
```

Generated Soufflé sources and libraries are build artifacts, not checked-in public interfaces. Human-authored Datalog and its versioned manifest are source artifacts.

## 15. Schema Migration

Existing `summary.v1` objects remain immutable and readable. New analysis after M8R.2 publishes `summary.v2`.

A V1 compatibility adapter may create a V2 execution projection with these restrictions:

- raw string memory locations become `LEGACY_OPAQUE` memory objects;
- absent dispatch information becomes explicit unknown dispatch;
- missing alias semantics remain unknown rather than inferred;
- missing stable call targets become `UnknownCall`;
- the run manifest records the V1 compatibility source.

Compatibility projection never fabricates precision. Reanalysis is the canonical path to native V2 summaries. V1 and V2 inputs cannot coexist in one component run unless every V1 input first passes through the tagged compatibility projection.

Historical V1 facts remain queryable. New V2 publication marks superseded historical results stale; it does not rewrite their bytes or identities.

## 16. Milestone Plan

### M8R.1 — Semantic Fact Contract

Deliver:

- `AnalysisRun` and versioned relation-schema registry;
- typed semantic enums and V2 fact representations;
- dense/stable-ID mapping and validation;
- complete uncertainty transport;
- compatibility adapters for existing M8 facts.

Complete when every supported semantic and epistemic value round-trips, distinct entities cannot collide, and supported M8 fixtures retain equivalent results.

### M8R.2 — SVF and Memory Refinement

Deliver:

- normalized indirect, virtual, and callback targets from SVF;
- refined memory effects and dependency facts;
- abstract objects, locations, subobjects, ranges, and unknown locations;
- complete alias-kind preservation;
- initial external-function model infrastructure;
- `summary.v2` publication.

Complete when stable `MAY` call targets enter the call graph, all alias kinds survive, distinct allocations remain distinct, unknown externals carry explicit policy, and no placeholder string is durable semantic identity.

### M8R.3 — Relational WPA Projection

Deliver:

- summary-to-EDB materializer;
- per-SCC component-input and successor-support contracts;
- relation and rule-bundle manifests;
- V2 reachability and may-write rules;
- witness relations and generic canonicalizer;
- adapters that give Soufflé and C++ the same logical input.

Complete when existing supported M8 semantics match across engines and every output has a closed finite witness.

### M8R.4 — Production Soufflé WPA

Deliver:

- required Soufflé build integration and compiled rule programs;
- exact supported Soufflé source/binary provenance verification recorded in the engine toolchain identity;
- private production executor;
- SCC orchestration and incremental state integration;
- content-addressed cross-revision component reuse keyed by logical input and qualified engine toolchain;
- standard project-pipeline integration;
- stale and incomplete run handling;
- explicit C++ emergency configuration.

Complete when standard analysis executes recursive WPA, missing or incompatible Soufflé fails normally, emergency mode requires explicit selection, and only external semantic changes schedule predecessors.

### M8R.5 — Qualification and M9 Handoff

Deliver:

- differential conformance corpus;
- determinism, failure-injection, migration, and performance qualification;
- generic Analysis Fact Bus handoff;
- complete component/rooted-input metadata and idempotent multi-sink delivery;
- removal of relation-specific semantic-row proof reconstruction from the production path;
- synchronized platform, WPA, SummaryDB, README, and milestone documentation.

Complete when every M9 entry criterion in section 17 passes.

### M9 — Durable Fact and Provenance Store

Persist analysis runs, canonical facts, witness edges, diagnostics, stale state, and semantic hashes. `explainFact` reads persisted witnesses and does not recompute M8R facts.

### M10A — Recursive Domain Expansion

Add `MayRead`, `GlobalFlow`, `UnknownEffect`, and `SoundnessCoverage`, with independent rule bundles, models, and conformance suites.

### M10B — Evidence Builder and Security Demo

Build Evidence APIs over M9 facts and M10A relations. The buffer-safety demonstration must use actual value flow, range, memory, uncertainty, and provenance facts.

### M11 — External IR Adapter

Accept bitcode and textual IR while preserving the same local analysis, summary-v2, relational projection, and WPA contracts.

### M12A–M12D — External Provider Ingestion

M12A introduces the SummaryDB provider graph/fact substrate, M12B directly
imports Joern GraphSON/GraphML, M12C adds provider fusion and Evidence
integration, and M12D reserves a separately designed PhASAR result adapter.
Imported observations remain non-authoritative and cannot silently participate
in native summary or WPA invalidation. The canonical design is
`m12-joern-cpg-summarydb-importer-design-spec.md`.

### M13 — Benchmark-gated PTA Research

Evaluate a Soufflé-native points-to and call-graph kernel against pinned SVF. Replacement requires an independently approved design and explicit correctness, precision, performance, and model-coverage thresholds. M13 is not a dependency of M9–M12.

## 17. M9 Entry Criteria

M9 may begin only when all of these conditions hold:

1. Function Summary IR v2 preserves dispatch, abstract memory, alias kind, uncertainty, and provenance inputs.
2. Indirect-call candidates from SVF participate in the summary call graph.
3. Distinct unnamed values and allocation sites retain distinct semantic identity.
4. V2 WPA relations use a typed schema and run-local dense IDs for hot joins.
5. Soufflé is the normal production recursive executor and is integrated into standard analysis.
6. C++ comparison uses byte-identical engine-neutral logical component input under its own run envelope and passes the overlapping conformance corpus.
7. Every derived result has a validated finite witness, and the M9 batch proves expected-component completion and rooted-input closure.
8. Failed or incomplete runs cannot replace successful results.
9. Run, schema, rule, model, configuration, and engine versions participate in identity.
10. Architecture, milestone, README, and operational documentation agree on engine ownership and current behavior.

## 18. Documentation Migration

When implementation begins, documentation changes follow these rules:

- the M8 specification remains an implemented historical record and links forward to this remediation program;
- the platform architecture retains P6 and adds the run-local relational projection;
- the WPA architecture names SVF, Soufflé, C++, and VERITAS ownership precisely;
- the SummaryDB design adds run, fact, witness, diagnostic, and stale-result contracts;
- the milestone map adds M8R.1–M8R.5 and divides M10 into M10A and M10B;
- the brainstorm document is marked as design input superseded by this approved architecture;
- README describes Soufflé as the required recursive WPA engine only after M8R.4 is delivered and continues to describe current behavior accurately before then.

## 19. Final Architecture Invariants

1. Function Summary IR is the durable WPA contract.
2. Detailed relational program facts are run-local projections.
3. SVF is the authoritative V1 points-to owner.
4. Soufflé is the authoritative production recursive-WPA owner.
5. C++ is an oracle or explicit emergency engine, never a silent peer.
6. VERITAS owns stable identity, semantic normalization, uncertainty, fact identity, witnesses, and publication.
7. Durable semantic identity never depends on raw LLVM names, ordinals, pointers, or engine tuple order.
8. Unknown and negative information survives every boundary explicitly.
9. No derived fact is published without a finite rooted witness.
10. No incomplete run replaces a successful result.
11. Semantic changes, not file timestamps or evidence-only churn, drive recursive invalidation.
12. Engine replacement does not require changing Summary IR, Fact Bus, or Evidence APIs.
13. Conformance compares distinct valid run envelopes over byte-identical engine-neutral logical input.
14. Component-cache reuse never crosses a logical-input or qualified engine/toolchain boundary.
15. M9 receives only complete batches whose rooted witnesses and idempotent delivery identity have been validated.
