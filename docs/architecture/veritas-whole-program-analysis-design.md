# VERITAS Whole-Program Analysis

## Analyzer Engines, Graph Representations, and SOTA C/C++ Pointer-Alias Policy

**Status:** Draft Architecture Specification
**Version:** 0.2
**Project:** VERITAS
**Peers:**

* `docs/architecture/veritas-platform-architecture-design.md` — platform pipeline, principles P1–P8, ingest adapter tiers.
* `docs/architecture/veritas-thin-summarydb-backends-design.md` — SummaryDB physical layers and pluggable backends.
* `docs/architecture/veritas-evidence-ir-design.md` — Evidence IR consumed by the Agent.

---

# 1. Purpose

This document specifies how VERITAS builds the deterministic semantic world that everything else consumes. It fixes:

* the frontend split between Clang AST and LLVM IR;
* the graph representations produced (AST, CFG, dominator tree, DDG, VFG, thin CPG);
* the six core analysis engines that run in V1 (call graph, CFG/dominators, def-use / value flow, alias / points-to, memory effects, ranges / constraints);
* the SOTA C/C++ pointer-alias landscape and VERITAS's L0–L3 tier policy;
* what SVF at the committed revision delivers today and the upgrade path;
* how uncertainty is preserved end-to-end.

**Delivery status.** M8 is implemented with a C++ recursive engine and optional
file-based Souffle comparison. The approved M8R architecture described below is
the target and is not yet delivered: pinned SVF owns V1 points-to/alias/SVFG and
indirect-call truth, compiled Souffle owns normal production recursive WPA, and
C++ is only a conformance oracle or explicitly selected emergency engine. The
[M8R bridge spec](../specs/milestones/m8r-souffle-wpa-remediation-design-spec.md)
records the gated transition without rewriting M8 history.

Platform-wide invariants (P1–P8) live in `veritas-platform-architecture-design.md`. Storage layout and identity IDs live in `veritas-thin-summarydb-backends-design.md`. This document assumes both.

---

# 2. Frontend — Clang + LLVM

VERITAS's frontend is Clang and LLVM. Clang gives access to source-level semantics; LLVM IR gives value-flow and target-aware semantics. Neither is a substitute for the other.

## 2.1 What Clang provides

```text
Clang AST
   │
   ├── semantic identity (mangled names, USR, canonical decls)
   ├── C/C++ semantic AST
   ├── templates and instantiations
   ├── inheritance and virtual dispatch
   ├── macro provenance
   ├── source locations
   └── compile configuration (target, macros, options)
```

Clang is used for identity, source anchors, and any analysis that requires source-level knowledge (templates, overload resolution, template specialization).

## 2.2 What LLVM IR provides

```text
LLVM IR
   │
   ├── SSA form
   ├── explicit memory operations
   ├── canonical control flow
   ├── def-use chains
   ├── target-aware semantics
   ├── optimization-normalized representation
   └── uniform substrate for SVF/DDG/VFG
```

LLVM IR is used for anything that benefits from a normalized, target-aware substrate — value flow, alias/points-to, range analysis, and the whole SVF pipeline.

## 2.3 Boundary rules

* VERITAS owns the frontend end-to-end (Clang CodeGen and `llvm::Linker` are executed by VERITAS, not by the user; see `veritas-platform-architecture-design.md` §7–8).
* No `Clang::Decl*`, `llvm::Module*`, or `llvm::Value*` pointer ever persists into a VERITAS artifact. All references are re-derived to VERITAS stable IDs.
* Detailed instruction-level graphs are regenerable on demand from the private in-memory `ProgramIr`; the persistent world is function- and object-centric.

---

# 3. Graph Representations

VERITAS builds and persists six graph views over the program. Only two of them are persisted as first-class storage; the rest are query projections or on-demand regenerations.

| Graph | Level | Role | Persisted? |
| --- | --- | --- | --- |
| AST | source | identity, source anchors, templates | via source_anchors; full AST regenerable |
| CFG | function | control flow and dominator relations | as summary facts (dominates, post-dominates, reachable) |
| Dominator tree | function | dominance and post-dominance | as summary facts |
| DDG | function/module | data dependencies (def-use / use-def) | as summary edges, later expandable |
| VFG | interprocedural | sparse value flow across summaries | as summary edges + WPA-derived flow facts |
| Thin CPG | interprocedural | unified query projection | yes (in-memory + SQLite adjacency indexes) |

The persistent world is deliberately function- and object-centric. Instruction-level detail is materialized only when refinement demands it (e.g. path feasibility for Evidence).

## 3.1 AST — source-level semantic identity

The AST is not persisted verbatim. What VERITAS persists is:

* `SourceAnchorID`s (canonical `(file, line, column, canonical-decl-path)` hash).
* Identity handles used by higher IDs: mangled names, canonical signatures, template-specialization identity, linkage class.
* Structural facts derived from the AST that are not recoverable from LLVM IR (macro provenance, template instantiations, virtual dispatch relations, C++ inheritance).

## 3.2 CFG and dominator analysis

The CFG is regenerable from the LLVM `Function`. What VERITAS persists is summary-level facts:

```text
dominates(bb_a, bb_b)
post_dominates(bb_a, bb_b)
reachable(bb_a, bb_b)
DominatorSummaryFact per function
BasicBlockSummaryRef with SourceAnchorID
```

Block IDs are derived from mapped source anchors, never from LLVM ordinal or address. Unmapped anchors produce a scoped `UnknownFact` rather than an invented ID.

## 3.3 Data Dependence Graph (DDG)

The DDG models def-use and use-def dependencies within a function and, via summary edges, across functions. It is the classical representation on top of which VFGs are built.

Two axes matter:

* **Sparse vs dense.** In SSA-form LLVM IR, def-use is already sparse: each value has an explicit list of uses. VERITAS's DDG is therefore built directly from SSA rather than from a dense CFG-annotated data-flow analysis.
* **Local vs interprocedural.** Local DDG lives entirely within a function; interprocedural dependencies are carried by summary edges (parameter-to-return, parameter-to-global, global-to-return, field-to-field, call-arg-to-parameter, return-to-call-result).

The DDG is a strict superset of the VFG's structural information; the VFG (§3.4) is what VERITAS actually persists.

## 3.4 Value Flow Graph (VFG)

The VFG is the value-oriented, SSA-sparse view of the DDG augmented with alias and memory-access edges. It is the primary IR for Evidence generation, because most defects (buffer overflow, tainted sink, null dereference, use-after-free) are naturally described as paths in the VFG.

```text
packet->len
     │
     ▼
parseHeader::len
     │
     ▼
decodeIE::size
     │
     ▼
copyIE::length
     │
     ▼
memcpy.size
```

VERITAS persists:

```text
ValueNode

ValueFlowEdge {
    src
    dst
    kind:      assignment | parameter | return | load | store | phi | alias | summary
    condition: optional predicate
}
```

`kind = summary` edges stand for an entire internal graph and expand on demand.

## 3.5 Thin CPG projection

The CPG is a compact query graph inspired by Joern's Code Property Graph, but scoped to what VERITAS actually needs. It is a projection over the same summaries and facts, not a parallel source of truth (P7).

Nodes:

```text
TranslationUnit  Namespace  Type  Function  Parameter  Local  Global
BasicBlockSummary  Summary  CallSite  MemoryObject  Field
Lock  State  Thread  Message  Unknown
```

Edges:

```text
CONTAINS  DECLARES  CALLS  MAY_CALL  UNKNOWN_CALL
DEF  USE  FLOWS_TO  CONTROLS  DOMINATES_SUMMARY  POST_DOMINATES
READS  WRITES  ALIASES  ACQUIRES  RELEASES  TRANSITIONS
SUMMARIZED_BY  UNKNOWN_AT
```

Two policies keep the CPG "thin":

1. **Function- and object-centric.** Instruction-level nodes are not stored globally; they are regenerated per query.
2. **Unknowns are named, not fanned-out.** An `UNKNOWN_CALL` never expands to "edges to every function"; it is a first-class node with its own reason (`EXTERNAL_FUNCTION`, `INDIRECT_TARGET_UNRESOLVED`, `INLINE_ASM`, …).

The CPG is atomic with the summaries it projects — a `ProjectionID` hashes the schema, revision, build variant, module hash, and the sorted set of summaries/nodes/edges, and publication swaps the projection atomically with the underlying summary bindings.

---

# 4. Core Analysis Engines (V1)

V1 ships six core engines. Later domains (concurrency, ownership, state machines) build on the same substrate; they are out of scope for this document.

## 4.1 Call graph

The call graph is the foundation because almost every WPA algorithm depends on it. VERITAS represents dispatch kind and certainty explicitly:

```text
CallEdge {
    caller
    callsite
    target
    confidence:   MUST_CALL | MAY_CALL | UNKNOWN_CALL
    dispatch_kind: direct | function_pointer | virtual | template | callback | external
}
```

Sources contributing edges:

* Clang AST — direct calls, virtual dispatch candidates, template instantiations, callback pattern detection.
* LLVM IR — inlined call sites, address-taken functions, direct call peephole.
* SVF — indirect-call refinement, virtual-dispatch tightening via points-to.

Rules:

* `MUST_CALL` is emitted only when the analyzer establishes the target holds for all executions in its abstraction.
* `UNKNOWN_CALL` never fans out to "all functions." It is a first-class edge with a reason.
* Later refinement can strengthen `UNKNOWN_CALL → MAY_CALL → MUST_CALL`, never the reverse.

## 4.2 CFG and dominator analysis

Persisted as summary facts (see §3.2). Detailed CFGs are regenerable from the private `ProgramIr`.

## 4.3 Def-use / value-flow analysis

VERITAS follows CodeQL's decomposition — local data flow, global data flow, taint tracking — and persists them as VFG edges plus classified transfer summaries:

```text
LocalDefUse
ParameterToReturn
ParameterToGlobal
GlobalToReturn
FieldToField
CallArgToParameter
ReturnToCallResult
```

## 4.4 Alias and points-to analysis

Layered precision, explicit uncertainty. See §5 for the full SOTA landscape and tier policy.

## 4.5 Memory-effect analysis

Every function summary carries a compact effect record:

```yaml
memory:
  reads:
    - ctx.state
    - packet.header
  writes:
    - ctx.counter
    - ctx.state
  allocates:
    - return
  frees:
    - arg1
  escapes:
    - arg0
```

Effects use **abstract memory locations**, not raw LLVM pointers:

```text
Global:G
Object:ctx
ObjectField:ctx.state
ArgumentObject:arg0
ArgumentField:arg0.header
HeapSite:foo.cpp:128
```

Memory-abstraction quality is one of the most important decisions in the whole platform; see §7.

## 4.6 Range and constraint analysis

VERITAS builds on LLVM's existing value reasoning. Facts:

```text
0 <= packet.len <= 65535
SUCCESS => ctx.state == CONNECTED
arg0 != NULL
ret == SUCCESS => bytes_written <= capacity
```

The representation supports (in later versions) symbolic expressions:

```text
RangeExpression
BooleanConstraint
LinearConstraint
Predicate
```

These facts feed Evidence IR and eventually SMT.

---

# 5. SOTA C/C++ Pointer-Alias Landscape

Pointer-alias precision is the single largest lever on analysis quality for C and C++. This section surveys the state of the art, then states VERITAS's layered policy in §6.

## 5.1 Complexity vs precision — the axes

Pointer analyses vary along four largely independent axes. Combinations of these axes generate the actual algorithmic landscape.

| Axis | Values | What it buys |
| --- | --- | --- |
| Flow sensitivity | flow-insensitive · flow-sensitive | precision along a single execution path |
| Context sensitivity | context-insensitive · k-CFA · object-sensitive · type-sensitive | precision across call sites |
| Field sensitivity | field-insensitive · field-based · field-sensitive | precision on struct/class members |
| Heap abstraction | one-object · allocation-site · k-limited · shape | precision on dynamic allocations |

A production analyzer picks one point per axis and often mixes points (e.g. flow-insensitive main pass + flow-sensitive refinement).

## 5.2 Foundational algorithms

**Steensgaard (1996).** Unification-based, flow- and context-insensitive. Every may-alias becomes a must-merge of equivalence classes. Near-linear time — O(n α(n)). Fastest possible, but loses `NoAlias` in most non-trivial programs; useful as a first-cut / structural filter.

**Andersen (1994).** Subset (inclusion) constraint-based, flow- and context-insensitive. Every may-alias becomes a subset relation over points-to sets. Classical complexity is cubic; modern engineering (wave-propagation, difference propagation, offline optimization) drives it well below cubic in practice for LLVM-scale programs. This is the canonical L1 baseline.

**Hardekopf / Lin (2007) "Ball & Wilson" and follow-ons.** Semi-sparse and staged Andersen variants — pre-analysis to identify strong updates, then a sparse flow-sensitive pass. Bridge between L1 and L2.

## 5.3 Sensitivity extensions

**Flow-sensitive analysis (SFS, semi-sparse FS).** Adds path-ordered updates so `p = &a; p = &b;` distinguishes the two program points. Necessary for strong updates and precise kill/gen sets.

**Context-sensitive analysis.**

* **Call-string / k-CFA.** Distinguish points-to per suffix of the call stack (`k=1`, `k=2`, …). Precise for procedural code but explodes on deep polymorphic call chains.
* **Object-sensitivity (Milanova, Rountev, Ryder).** Distinguish per receiver-object provenance. Especially effective for OO / C++ virtual dispatch and containers.
* **Type-sensitivity.** Distinguish per receiver-type. Cheaper than object-sensitivity, close in precision for well-typed OO code.

**Field sensitivity.**

* Field-insensitive treats `s.f` as `s`.
* Field-based indexes points-to by declared field name (unsafe under C-style aliasing).
* Field-sensitive tracks per-field points-to with proper handling of casts and array indexing — closest to what real programs need.

**Heap abstraction.**

* Allocation-site (a.k.a. "birthplace") is the default in most modern analyzers.
* k-limited allocation-site refines by k prefix of the call stack at allocation.
* Shape analysis (TVLA-style) tracks structural properties (list, tree) — expensive; rarely used in whole-program mode.

## 5.4 Demand-driven refinement

Whole-program flow-and-context-sensitive analysis is too expensive on 10 M+ LOC codebases if run eagerly. Demand-driven approaches invert the flow: the analyzer answers "may `p` alias `q` at this point?" by exploring only the slice of the program that participates in the answer.

* **DDA / demand-driven Andersen** (Sridharan & Bodík, 2006; refined for C/C++ by Yulei Sui et al.).
* **SUPA** (Sui, Xue et al.) — sparse, flow-sensitive, context-sensitive on-demand points-to, built on top of SVF's VFG.
* **Origin-sensitivity** (Tan, Li, Xue) — a modern refinement for OO codes that outperforms plain object-sensitivity at similar cost.

Demand-driven refinement is how VERITAS gets from L1 to L2/L3 without paying whole-program flow-sensitive cost.

## 5.5 Sparse-VFG-based analyses

Sparse value-flow analyses build a VFG from an initial (usually Andersen) points-to solution, then run subsequent analyses (flow-sensitive points-to, taint, use-after-free detectors) directly over the VFG rather than the CFG. This is the SVF architecture and the source of most of the leverage on 10 M-LOC C/C++ codebases.

Key algorithms in this family:

* `AndersenWaveDiff` — the standard fast Andersen implementation in SVF.
* `FlowSensitive` — a full FS pointer analysis built on the sparse VFG.
* `TypeBasedHeapCloning` — targeted heap cloning for OO code.
* `VersionedFlowSensitive` — versioned SSA propagation for improved FS scalability.

## 5.6 C/C++-specific difficulties

C/C++ makes pointer analysis harder than the general theory suggests:

* **Address-taken functions and function pointers.** Indirect calls dominate real code; without them, call-graph precision collapses.
* **Virtual dispatch and multiple inheritance.** vtable layout, thunks, RTTI, and template specialization all interact with points-to.
* **Union types and reinterpret_cast.** Field-sensitivity must be robust to type punning.
* **Placement `new` and custom allocators.** Allocation-site heap abstraction requires seeing all allocation sites, not just `malloc` / `new`.
* **Uninstantiated templates.** Header-only code produces many instantiations; identity must not conflate them.
* **Inline assembly and intrinsics.** No IR-level semantics — must surface as `UnknownFact` with a scoped reason.

These are exactly the classes of imprecision VERITAS's uncertainty labels are designed to represent instead of paper over.

---

# 6. VERITAS Alias Tier Policy

VERITAS commits to a **layered precision** policy. Every fact in the fact store is stamped with the tier that produced it, and higher tiers are permitted to refine lower tiers but never to silently downgrade a `MUST` or invent a `MustAlias` where a lower tier said `MayAlias`.

## 6.1 The four tiers

| Tier | Analysis | Cost | Precision goal |
| --- | --- | --- | --- |
| L0 | Type-based / LLVM BasicAA | very low | conservative filter (reject obvious `NoAlias`) |
| L1 | Andersen inclusion (SVF `AndersenWaveDiff`) | moderate | baseline whole-program points-to |
| L2 | Context-sensitive refinement (object-, origin-, or type-sensitive) | higher | tighten popular hot spots, virtual dispatch |
| L3 | Demand-driven path refinement (SUPA-style) | on-demand | precision-critical Evidence slices |

L0 is a filter, L1 is the whole-program pass, L2 is a targeted refinement, L3 is invoked only when a specific query needs it (e.g., an Evidence case blocked on unresolved `MayAlias`).

## 6.2 What the tiers persist

VERITAS never collapses uncertainty to a boolean. Alias facts persist all four labels:

```text
MustAlias
MayAlias
NoAlias
UnknownAlias
```

For Evidence generation, `UnknownAlias` is a first-class fact — it tells the Agent (or a proof engine) exactly where a lower tier could not resolve, and where an L3 refinement might help.

## 6.3 SVF commitment (today's L1)

VERITAS commits to the SVF library at a pinned revision as the L1 implementation:

```text
third_party/SVF
upstream: https://github.com/SVF-tools/SVF.git
revision: 18fb5650600530a54f0afc22f4df1a10b03d3c02
```

`AndersenWaveDiff` is invoked in-process on the private `ProgramIr`. Results are mapped to VERITAS references (see §7). SVF headers and native types are confined to `src/analysis/svf`; no public VERITAS API accepts or returns an SVF node, graph, identifier, or command-line artifact.

Pinned SVF is the authoritative V1 owner for Andersen points-to results, alias
classification, SVFG construction, and indirect-call candidates. A
Souffle-native PTA is not a production alternative; M13 may research one only
against explicit correctness, precision, model-coverage, and performance
benchmarks, independently of the M9-M12 critical path.

Standard VERITAS builds cannot disable SVF — there is no `VERITAS_ENABLE_SVF` option — because M5's design invariants require whole-program pointer analysis for correctness of call-graph refinement and value flow.

## 6.4 Upgrade path to L2 / L3

The upgrade is a matter of adding SVF passes and refinement stages, not of rearchitecting:

* **L2 (context-sensitive).** Enable SVF's context-sensitive Andersen or object-sensitive extension; results replace the L1 points-to for functions the scheduler flags as hot (call-graph fan-in above threshold, or Evidence-cited).
* **L3 (demand-driven).** Invoke SUPA-style refinement on a per-query basis inside the Evidence Builder. Facts produced at L3 are stamped with the query context that motivated them; they are not published as global summaries.

The tier of every alias fact participates in the fact's provenance record (see `veritas-thin-summarydb-backends-design.md` §7).

---

# 7. SVF Integration Boundary

M5's committed SVF integration establishes the concrete engineering shape of the alias tier boundary. Three isolation rules keep SVF from leaking into the rest of the platform.

## 7.1 Ownership isolation

Uses of SVF are confined to:

```text
points-to analysis
value-flow graph construction
load/store relation refinement
indirect-call target candidates
```

Everything else is VERITAS's own code.

## 7.2 Header isolation

SVF headers live only under `src/analysis/svf`. Installed public headers under `include/veritas/**` contain no SVF native types. The public boundary exposes:

```cpp
namespace veritas::analysis {

class SvfConfig;      // budgets, node/fact caps
class SvfAnalysisStage;   // private constructor; owned by pipeline

}  // namespace veritas::analysis
```

## 7.3 Identity isolation

SVF node IDs are not persistable identities. Every SVF result is mapped to VERITAS references:

```text
SVF PAG node          →   MemoryRef  |  ValueRef
SVF call site         →   CallSiteRef anchored by SourceAnchorID
SVF indirect target   →   FunctionVariantID
SVF alias pair        →   pair<ValueRef, ValueRef>  (with MustAlias | MayAlias | NoAlias | UnknownAlias)
```

`SvfConfig` participates in `AnalyzerRunID`, so a configuration change is a first-class semantic event (P5).

## 7.4 Lifecycle

SVF is initialized per analysis run and released at the end. There are no long-lived SVF singletons owned by the platform. This preserves reproducibility: the same input produces the same output regardless of prior runs in the process.

---

# 8. Memory Abstraction Model

The minimum durable V2 identity is lossless and structural:

```text
MemoryLocation = AbstractObject + AccessPath + ByteRange
```

`AbstractObject` distinguishes global, stack, heap, argument, function,
external, and unknown objects. `AccessPath` carries fields, array elements or
ranges, and an explicit unknown suffix. `ByteRange` preserves a signed offset
and unsigned size when known and an explicit unknown range otherwise.

The run-local relation projection carries `RangeKind = KNOWN | UNKNOWN`.
Known zero offsets and sizes remain known values. Unknown ranges use canonical
zero payload cells with no semantic range meaning; half-known ranges and
non-zero unknown payloads fail validation. No nullable column or sentinel value
may blur these cases.

The memory abstraction interacts with every other engine. VERITAS's model:

* **Named globals** — one abstract location per canonical global declaration.
* **Parameter objects** — one abstract location per function parameter,
  refined by the callee's field access pattern.
* **Fields** — field-sensitive declared paths, with type-punning preserved as
  explicit overlap or `UnknownAlias`.
* **Heap** — collision-free allocation-site identity from owner function,
  stable semantic/source anchor, allocation kind, and a deterministic local
  fingerprint, with optional k-limited context.
* **Stack** — flow-sensitive within a function; escape analysis marks stack
  objects that outlive the call.

Raw display names and placeholders such as `<local>`, `<unnamed>`, `<param>`,
and `<call-effect>` are diagnostic only. Multiple unnamed values or allocations
in one function must retain distinct stable identities. Opaque-pointer recovery
uses available LLVM types, `DataLayout`, debug/TBAA evidence, allocation sizes,
and SVF results; missing evidence yields an explicit unknown or overlap, never
fabricated precision.

Operations and their guards remain in durable summaries; detailed relation rows
are reconstructed only for a WPA run.

---

# 9. WPA — SCC-Scoped Execution

WPA turns durable function summaries into whole-program facts. VERITAS owns the
call graph and evaluates SCCs in reverse topological order.

## 9.1 Engine-neutral logical component input

One execution unit evaluates one domain for one SCC:

```text
WpaLogicalComponentInput
  LogicalInputHash
  SccId and ComponentKind
  member stable identities and base facts
  stable <-> typed dense mappings
  outgoing call edges
  successor external support facts
  applicable models and semantic configuration
```

This structure has one canonical engine-neutral serialization. Souffle
production and C++ conformance/emergency executions receive byte-identical
logical-input bytes and mappings, then derive distinct valid
`WpaExecutionEnvelope` values and `RunId` values. Each executor rejects an
envelope bearing another engine's identity.

Successor SCC results enter as explicit support relations with stable support
fact identities. Rules may derive only results owned by the current SCC.

## 9.2 Uncertainty in SCC construction

Call edges retain dispatch kind and epistemic labels. `MUST_CALL` and
`MAY_CALL` participate; `UNKNOWN_CALL` remains an explicit scoped fact and never
fans out to every function. SVF-normalized indirect, virtual, and callback
candidates enter as stable `MAY` edges rather than raw SVF identities.

## 9.3 Domains, hashes, and content-addressed reuse

The first production rule bundles are `ReachableCall` and `MayWrite`. M10A
later adds `MayRead`, `GlobalFlow`, `UnknownEffect`, and `SoundnessCoverage` as
independent bundles with models and conformance suites.

Each component records:

```text
LogicalInputHash  canonical semantic input, mappings, schemas, bundles, models,
                  configuration, and successor external facts
FixpointHash      complete canonical results plus selected witnesses
ExternalHash      predecessor-visible semantic results only
```

Revision, `RunId`, engine identity, and tuple order do not enter the logical or
external hashes. A witness-only change may alter `FixpointHash` without changing
`ExternalHash`, so predecessors are not scheduled.

A prior successful immutable component may be reused across revisions only by
`(LogicalInputHash, EngineToolchainIdentity)`. Lookup revalidates content,
rooted inputs, schema/bundle identities, and exact executor provenance. Each run
retains a distinct history reference; C++ output or another Souffle build can
never satisfy the production run.

## 9.4 Failure and stale history

Execution is failure atomic. A missing/incompatible bundle, engine failure,
timeout or resource exhaustion, invalid stable/dense mapping, schema mismatch,
unsupported epistemic output, inconsistent duplicate, or malformed/unrooted
witness publishes no replacement component. Durable diagnostics mark the run
incomplete and the last successful result remains queryable only as stale
history. Partial output is never mixed with old facts.

---

# 10. Compiled Souffle Production Engine

For recursive interprocedural queries, the normal production engine is compiled
Souffle. Before execution, the adapter parses the configured install-provenance
manifest, requires Souffle 2.5 at exact source revision
`5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, hashes the configured executable,
and rejects any mismatch with the manifest's recorded executable digest.
`EngineToolchainIdentity` includes the verified manifest identity/content
digest, source revision, executable digest, generated rule-bundle digest, and
generator/compiler/link toolchain provenance. Generated programs use one
evaluation thread until a separately qualified upgrade retires the upstream
ARM concurrency issue.

Datalog remains a natural execution form:

```text
MayWrite(f,x) :- DirectWrite(f,x).
MayWrite(f,x) :- Call(f,g), MayWrite(g,x).

ReachableCall(f,g) :- DirectCall(f,g).
ReachableCall(f,h) :- DirectCall(f,g), ReachableCall(g,h).
```

Rules and boundaries:

* Function Summary IR remains durable. Typed `relations.v2` EDB/IDB rows and
  dense IDs are a run-local execution projection.
* Relation registry entries define column types, keys, ownership, epistemic
  values, materialization policy, and schema version; untyped string vectors
  are not authoritative V2 schemas.
* Stable identity and dense identity are separate, and negative or unknown
  information is represented explicitly.
* Souffle-generated and public types remain private to the adapter; none appears
  under `include/veritas/**`.
* The C++ engine receives the same byte-identical
  `WpaLogicalComponentInput`, but only in CI conformance or explicitly selected
  `cpp-emergency` mode. Automatic fallback is forbidden, and its engine/run
  identity cannot impersonate or overwrite Souffle.
* VERITAS canonicalizes outputs back to stable engine-neutral facts before any
  durable publication. See `veritas-thin-summarydb-backends-design.md` §12.

Every rule bundle emits generic immediate witness edges from a result semantic
key to input semantic keys. Base and successor-support leaves are stable rooted
input fact IDs. The shared semantic-key codec is versioned, injective,
type-tagged, length-prefixed UTF-8; ambiguous raw-delimiter concatenation is
forbidden. Qualification covers delimiters, empty strings, Unicode, digit
prefixes, and numeric bounds across both engines.

The canonicalizer requires every published result to have a finite acyclic
proof closed over the published facts and rooted inputs. It selects one proof
by fewer derived edges, versioned rule priority, then lexicographic stable input
identity. Souffle interactive proof trees remain useful for rule debugging but
are not the durable VERITAS evidence protocol.

Epistemic joins remain explicit and bundle-versioned. `NO_ALIAS + MUST`, absent
facts, and unknown alias results are distinct; unsupported semantic or
epistemic values are rejected or converted to explicit unknown relations under
the declared bundle contract, never silently omitted.

---

# 11. VFG Primacy for Evidence

For Evidence generation, the VFG is more important than the AST. Most defects are naturally described as VFG paths:

```text
packet.length → parseHeader.len → decodeIE.size → copyIE.length → memcpy.size
```

VERITAS's Evidence Builder queries the VFG plus provenance to construct a compact `EvidenceCase`:

* `path P1 value_flow` — the primary flow.
* `fact F1 range(len, 0, 65535)` — bounds on the flowing value.
* `fact F2 capacity(dst) == 2048` — the target's capacity.
* `fact F3 not dominates(validate, sink)` — the missing check.
* `unknown U1 postcondition(vendor_validate)` — the missing semantics.
* `verify O1 forall path reaching sink: len <= capacity(dst)` — the proof obligation.

The M10B mechanics of these queries are specified in `docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md` and the syntax in `veritas-evidence-ir-design.md`.

---

# 12. Query APIs Exposed to WPA and Evidence

The analyzer subsystems expose semantic APIs, not internal graph handles:

```text
getFunctionSummary(F)
getCallers(F)
getCallees(F)
getTransitiveCallers(F)
getValueFlow(src, dst)
getMayWrites(F)
getReads(F)
getAliases(V)         → { MustAlias, MayAlias, NoAlias, UnknownAlias }
getRanges(V)
getStateTransitions(F)
getDominatorFacts(F)
getUnknownsIn(F)
explainFact(FactID)
getEvidenceSlice(Claim)
```

These APIs are the read interface WPA and the Evidence Builder consume. Backend implementations live in the SummaryDB layer (see `veritas-thin-summarydb-backends-design.md` §8).

---

# 13. CPG Generation Pipeline

The CPG builder evolves in three stages, matched to the platform milestones.

**Stage 1 (M6 delivered):**

```text
AST + CFG + Call Graph
```

Enough to support navigation, call relationships, control flow, source locations.

**Stage 2:**

```text
+ DDG
+ VFG
+ Memory objects
+ Alias edges (all four labels)
```

Evidence generation becomes powerful. Buffer-overflow / null-deref / UAF demos all live at this stage.

**Stage 3 (post-backbone):**

```text
+ Lock graph
+ Thread / task graph
+ State graph
+ Message graph
+ Resource graph
```

Domain-specific semantic graphs; this is where VERITAS differentiates from generic CodeQL / Joern.

---

# 14. Analysis Invariants

Analysis-specific invariants are layered on top of the platform invariants (`veritas-platform-architecture-design.md` §11). This document adds:

| ID | Invariant | Reason |
| --- | --- | --- |
| A1 | No `llvm::Value*` or `clang::Decl*` pointer persists into a VERITAS artifact. | Reproducibility, cross-run stability. |
| A2 | Block IDs derive from mapped source anchors, never from LLVM ordinal or address. | Determinism under recompilation. |
| A3 | `UNKNOWN_CALL` never fans out to all functions. | Prevents SCC collapse. |
| A4 | Alias facts persist all four labels `MustAlias / MayAlias / NoAlias / UnknownAlias`. | Uncertainty is first-class information. |
| A5 | Higher alias tiers may refine lower tiers but may never silently downgrade `MUST` or invent `MustAlias` where a lower tier said `MayAlias`. | Sound tier layering. |
| A6 | SVF native types never appear in any public VERITAS API. | Isolation. |
| A7 | Standard VERITAS builds cannot disable SVF. | M5 correctness. |
| A8 | Approximation weakens epistemic state; it never strengthens it. | Soundness of WPA joins. |
| A9 | `INFERRED` cannot become `MUST` without a deterministic verifier. | P8 enforced through the join algebra. |
| A10 | Function Summary IR is durable; `relations.v2` and dense IDs are run-local. | Preserve the platform boundary. |
| A11 | Compiled Souffle is the normal recursive owner; C++ is conformance or explicit emergency only. | One production semantics owner; no silent fallback. |
| A12 | Every published derived fact has a deterministic finite rooted witness. | Durable explanation is engine-neutral. |
| A13 | A failed component publishes nothing and cannot replace the last successful result. | Failure atomicity and coherent history. |

---

# 15. Reading Order

Start with `veritas-platform-architecture-design.md` for the platform pipeline and principles.
Then this document for how the analysis engines fit together.
Then `veritas-thin-summarydb-backends-design.md` for how identity, hashing, and storage are laid out.
Then `veritas-evidence-ir-design.md` for the IR the Agent consumes.

Milestone specs under `docs/specs/milestones/` refine specific sections:

* M4 (`m4-clang-llvm-local-extraction-design-spec.md`) — frontend and local extraction.
* M5 (`m5-svf-value-flow-pointer-adapter-design-spec.md`) — SVF integration.
* M6 (`m6-thin-veritas-cpg-projection-design-spec.md`) — thin CPG projection.
* M8 (`m8-scc-wpa-souffle-fact-engine-design-spec.md`) — SCC / Datalog WPA.
* M8R (`m8r-souffle-wpa-remediation-design-spec.md`) — approved production-Souffle remediation and M9 gate.
