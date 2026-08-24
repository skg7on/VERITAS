# M6 LLVM-Native Thin VERITAS CPG Projection Design Spec

**Status:** Draft
**Milestone:** M6
**Depends on:** M4 live `ProgramIr` and origin map, M5 completed mapped facts
**Feeds:** M10B Evidence Builder and query tools

---

# 1. Purpose

M6 builds the VERITAS Code Property Graph as a VERITAS-owned projection directly from the live, linked LLVM `ProgramIr` and the completed VERITAS facts mapped by M5. The projection runs as a C++ library stage inside `ProjectAnalyzer`; it does not invoke an external CPG generator.

The persistent graph is a compact query index, not compiler IR and not the source of semantic truth. Summary IR remains authoritative for semantic facts. `ProgramIr` supplies project structure that is cheap and precise to recover while the linked module is alive, and the completed mapped summaries supply calls, memory effects, value flow, alias state, unknowns, and provenance.

---

# 2. Ownership and Non-Goals

VERITAS owns:

```text
CPG node and edge schemas
stable node and edge identities
LLVM-to-VERITAS projection logic
fact-to-edge projection logic
in-memory graph construction and validation
persistent adjacency indexes
query budgets and truncation semantics
projection publication and revision/build bindings
```

M6 does not use Joern or PhASAR as a CPG generator. The standard build and runtime must not invoke or communicate with:

```text
Joern CLI, server, plugins, schema generator, exporters, or databases
PhASAR CLI or a standalone PhASAR analysis process
an external CPG service
an external compiler or LLVM analysis executable
```

M6 accepts no `.bc`, `.ll`, serialized CPG, Joern export, PhASAR result, LLVM-module pathname, or subprocess output. It consumes borrowed in-memory VERITAS objects only.

M12 does not relax this boundary. It stores a Joern export as a distinct
provider projection in SummaryDB's Graph Index and exposes a pinned unified
query view above M6. Provider nodes/edges never enter `CpgProjectionInput`,
change an M6 `ProjectionID`, or co-publish with native summary bindings.

M6 does not persist:

```text
LLVM pointers or native LLVM IDs
SVF pointers or native SVF node IDs
Joern or PhASAR node IDs
every LLVM instruction
every AST expression
every temporary SSA value
a repository-wide instruction CFG
```

---

# 3. Pipeline Placement and Lifetime

The M6 projection runs after M5 has mapped and conservatively merged its required SVF results, but before the M4 `ProgramIr` is destroyed and before current project bindings are published:

```text
M4 LocalAnalysisResult
  ProgramIr + local summary drafts
                 |
                 v
M5 required in-process SVF analysis
  completed mapped summaries
                 |
                 v
M6 CpgProjectionStage
  borrow ProgramIr + completed summaries
                 |
                 v
validated in-memory ThinCpg
                 |
                 v
atomic current-binding publication
  summaries + CPG projection
```

M6 introduces a private, engine-neutral input boundary:

```cpp
namespace veritas::analysis::cpg {
struct CpgProjectionInput {
  const pipeline::ProgramIr& program_ir;
  std::span<const summary::v1::FunctionSummary> completed_summaries;
  core::StableId revision_id;
  core::StableId build_variant_id;
};

StatusOr<::veritas::cpg::ThinCpg> BuildThinCpg(
    const CpgProjectionInput& input);
}
```

`completed_summaries` contains the VERITAS facts produced by M4 and mapped by M5. The CPG stage does not include SVF headers and does not consume callback-scoped `SvfSessionView` state. This keeps the projection independent of the analysis engine while still avoiding a SummaryDB readback or serialized interchange.

`BuildThinCpg` may inspect `program_ir.module()` and `program_ir.origin_map()` only during the call. No native pointer may escape in a node, edge, cache key, return value, or diagnostic object.

---

# 4. Projection Sources

The projector uses each input for a distinct purpose.

## 4.1 Live `ProgramIr`

Use the linked LLVM module and M4 origin map to enumerate deterministic project structure:

```text
defined functions
parameters
globals
call instructions and their owning functions
basic-block summary anchors
load/store sites that resolve to VERITAS memory references
```

Every LLVM entity must resolve through `OriginMap` or through a deterministic stable-ID builder based on existing M2 identity inputs. An unresolved entity produces a scoped projection unknown; it must not receive an address-derived or allocation-order ID.

## 4.2 Completed mapped summaries

Project semantic graph relations from the completed in-memory summaries:

```text
CallFact          -> CALLS, MAY_CALL, or UNKNOWN_AT
MemoryEffectFact  -> READS or WRITES
ValueFlowFact     -> FLOWS_TO
AliasFact         -> ALIASES with exact alias state
dominator fact    -> DOMINATES_SUMMARY
summary identity  -> SUMMARIZED_BY
unknown fact      -> UNKNOWN_AT
```

Alias facts project to a generic `ALIASES` relation with a required `alias_state` property whose value is exactly one of `MustAlias`, `MayAlias`, `NoAlias`, or `UnknownAlias`. M6 emits an alias edge only for a candidate pair that M5 evaluated; it never manufactures all-pairs `NoAlias` edges.

Every semantic edge carries a sorted, deduplicated list of support records. Each record contains the originating `FunctionSummaryID` and the opaque M3/M5 `summary::ProvenanceRef`. These are not M9 provenance-store IDs. Dedicated persistent `Fact` and provenance-DAG nodes arrive with M9; M6 does not invent those identities early.

The fact is authoritative when LLVM structure and a mapped semantic fact overlap. LLVM traversal supplies topology and source ownership; it must not upgrade `MAY`, `UNKNOWN`, `INFERRED`, or `ASSUMED` facts to `MUST`.

## 4.3 Source-level properties

M6 V1 does not create standalone namespace, type, field, or translation-unit nodes because M4's current handoff does not retain a complete source-semantic index. Source anchors, translation-unit IDs, declared type strings, namespace-qualified names, and `MemoryRef` field paths may be retained as properties when they already exist in `OriginMap` or the completed summaries. M6 must not guess source-language semantics from lowered LLVM names when a mapping is absent. Adding source-semantic node kinds requires a later M4 handoff design that supplies stable source identities explicitly.

---

# 5. Persistent Graph Scope

Persistent nodes:

```text
Function
Parameter
Global
CallSite
MemoryObject
BasicBlockSummary
Summary
Unknown
```

Persistent edges:

```text
CONTAINS
DECLARES
CALLS
MAY_CALL
READS
WRITES
FLOWS_TO
ALIASES
DOMINATES_SUMMARY
SUMMARIZED_BY
UNKNOWN_AT
```

Instruction-level nodes may be materialized later for one Evidence Case while its `ProgramIr` or regenerated case IR is available. They are excluded from the global M6 projection.

---

# 6. Graph Identity and Determinism

Graph node IDs are VERITAS stable IDs:

```text
Function node          -> FunctionVariantID from OriginMap
Parameter node         -> mapped ValueRef ID for the llvm::Argument
Global node            -> mapped MemoryRef ID for the llvm::GlobalValue
CallSite node          -> CallSiteID from the completed CallFact/origin map
MemoryObject node      -> MemoryRef ID
BasicBlockSummary node -> BasicBlockSummaryID carried by mapped M4 summary facts
Summary node           -> FunctionSummaryID
Unknown node           -> stable hash of scope, kind, and provenance
```

M4 constructs `BasicBlockSummaryID` with M2 canonical hashing over the owning `FunctionVariantID`, the block's ordered mapped semantic `SourceAnchorID` members, and its sorted mapped predecessor/successor anchor IDs. It emits this reference only for blocks represented by local summary facts. M6 does not use LLVM block order or pointer identity. If any required node mapping is absent, M6 emits a scoped `Unknown`/`UNKNOWN_AT` relation instead of inventing an ID.

Edge IDs are canonical hashes of:

```text
revision_id
build_variant_id
edge_kind
source_node_id
target_node_id
semantic qualifiers
epistemic state
sorted support records: FunctionSummaryID + summary::ProvenanceRef
```

`ProjectionID` is the M2 canonical hash of:

```text
graph schema version = veritas.cpg.v1
revision_id
build_variant_id
ProgramIr module_hash
sorted completed FunctionSummaryIDs
sorted canonical node records
sorted canonical edge records
```

`CpgCanonicalizer::CanonicalBytes` uses M2's versioned, length-prefixed canonical writer. Tests compare these canonical bytes and `ProjectionID`; they do not compare SQLite database-file bytes.

The projector sorts functions, nodes, facts, support records, and edges by canonical VERITAS keys before insertion. LLVM iteration order, pointer addresses, SVF allocation order, hash-table order, and thread scheduling must not affect canonical graph bytes or projection IDs.

Duplicate nodes and edges are idempotent. A duplicate stable ID with different canonical content is a fatal consistency error.

---

# 7. In-Memory Graph and Validation

`ThinCpg` owns nodes, edges, and temporary adjacency indexes while the projection is built:

```cpp
namespace veritas::cpg {
class ThinCpg {
 public:
  Status AddNode(CpgNode node);
  Status AddEdge(CpgEdge edge);
  Status Validate() const;
  std::span<const CpgNode> nodes() const;
  std::span<const CpgEdge> edges() const;
};
}
```

Validation requires:

```text
every edge endpoint exists
every node and edge ID matches its canonical content
every semantic edge has revision/build context
every mapped fact edge retains epistemic state and provenance
every summary edge is marked expandable
no native pointer or transient third-party ID appears in persistent fields
no instruction node kind appears in the global projection
unknown calls do not fan out to all functions
```

---

# 8. Alias Analysis Policy

M6 projects the alias facts already produced by M5 `AndersenWaveDiff`. It does not run a second whole-program pointer analysis by default.

A PhASAR-inspired pointer/alias algorithm may be reimplemented inside VERITAS only when a checked-in acceptance fixture demonstrates a required alias relation that the pinned M5 analysis reports as unknown or cannot represent. It belongs in a versioned M5 refinement stage before summary completion; M6 only projects its resulting VERITAS `AliasFact` values. Such an addition requires all of the following:

```text
a failing precision fixture and stated expected epistemic result
a separate design decision identifying the exact published algorithm
a clean-room C++ implementation over the live LLVM 22 ProgramIr
no copied PhASAR source and no PhASAR runtime dependency
license and literature attribution review before merge
bounded execution with explicit truncation/unknown facts
its own analyzer name, version, configuration, provenance, and analyzer identity input
output mapped only to VERITAS AliasFact and related stable references
```

The refinement may narrow `UnknownAlias` to `MayAlias` or add justified relations. It must not overwrite an existing contradictory M4/M5 fact or silently upgrade a result to `MustAlias`.

This policy keeps the V1 implementation focused while preserving a safe extension path for algorithms described by LLVM-native frameworks such as PhASAR.

---

# 9. Storage and Atomic Publication

V1 stores typed nodes, edges, and adjacency indexes in SQLite alongside the metadata/current-binding transaction. Required logical indexes are:

```text
projection_id + node_id -> node
projection_id + source_node_id + edge_kind -> outgoing edges
projection_id + target_node_id + edge_kind -> incoming edges
projection_id + function_variant_id -> callsites
projection_id + memory_object_id -> readers/writers
projection_id + value_ref_id -> flow edges
revision_id + build_variant_id -> current projection_id
```

Projection construction and validation happen entirely in memory. Immutable summary objects may be written before the transaction, but the transaction that advances current summary bindings must also insert the validated graph and advance the current CPG binding. If projection validation or graph insertion fails, neither current binding advances.

Before opening the transaction, the publication coordinator verifies that the graph metadata's revision ID, build-variant ID, module hash, and canonical sorted `FunctionSummaryID` set exactly match the completed project analysis being published. A mismatch is a fatal precondition error and writes no current binding.

Historical projections remain addressable by `ProjectionID`. Rebuilding identical inputs produces the same projection ID and canonical node/edge content.

---

# 10. Query API

```cpp
namespace veritas::cpg {
enum class TruncationReason {
  kMaxDepth,
  kMaxNodes,
  kMaxPaths,
};

struct QueryBudget {
  std::size_t max_depth;
  std::size_t max_nodes;
  std::size_t max_paths;
};

template <typename T>
struct TraversalResult {
  std::vector<T> items;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t explored_nodes;
  std::size_t explored_paths;
};

class CpgQuery {
 public:
  static StatusOr<CpgQuery> OpenProjection(
      const CpgRepository& repository, core::StableId projection_id);
  static StatusOr<CpgQuery> OpenCurrent(
      const CpgRepository& repository, core::StableId revision_id,
      core::StableId build_variant_id);

  core::StableId projection_id() const;
  StatusOr<std::vector<CpgNode>> GetCallees(
      core::StableId function_variant_id) const;
  StatusOr<std::vector<CpgNode>> GetCallers(
      core::StableId function_variant_id) const;
  StatusOr<std::vector<CpgNode>> GetWriters(
      core::StableId memory_object_id) const;
  StatusOr<TraversalResult<CpgPath>> GetValueFlow(
      core::StableId src, core::StableId dst, QueryBudget budget) const;
  StatusOr<TraversalResult<CpgPath>> GetCallPaths(
      core::StableId src, core::StableId dst, QueryBudget budget) const;
};
}
```

`OpenCurrent` resolves the current binding once and returns a query object pinned to that immutable snapshot. Every traversal result reports each budget that truncated the search plus explored-node and explored-path counts. An empty, untruncated result means no path; a non-empty `truncation_reasons` list means the result is incomplete. `Status` remains reserved for operational errors. Queries never expand unknown calls into whole-program fanout.

---

# 11. Failure Handling

Fatal projection failures include:

```text
stable-ID collision with different content
edge with a missing endpoint
invalid revision/build ownership
native or third-party transient identity in persistent content
failure to write the atomic current-binding transaction
```

Recoverable loss of precision includes:

```text
LLVM entity not resolvable through OriginMap
unknown indirect-call target
unmapped memory location
alias refinement budget exhaustion
query budget exhaustion
```

Recoverable cases produce scoped unknown or truncation records with provenance. They are never silently dropped or expanded conservatively to every graph node.

---

# 12. Acceptance Tests

Required assertions:

```text
the projector consumes a live ProgramIr and completed in-memory summaries
no bitcode, LLVM-module path, serialized CPG, Joern, or PhASAR process is accepted
function and callsite nodes deduplicate across deterministic rebuilds
parameters, globals, and basic-block summaries use their specified mapped stable IDs
CALLS and MAY_CALL edges retain source anchors, epistemic state, and summary provenance
FLOWS_TO paths traverse expandable summary edges
SVF MustAlias/MayAlias/NoAlias/UnknownAlias map without epistemic upgrades
unknown calls create UNKNOWN_AT or bounded MAY_CALL edges, not full fanout
persistent graph size is not proportional to LLVM instruction count
two identical project analyses produce the same ProjectionID and canonical graph bytes
projection failure advances neither summary nor CPG current bindings
mismatched graph/summary snapshot metadata is rejected before any publication write
budgeted traversal reports the exact truncation reason
using a budget exactly is complete unless additional eligible work is rejected
installed public headers contain no LLVM, SVF, Joern, or PhASAR native types
```

Boundary tests scan `CMakeLists.txt`, `cmake/`, `src/`, `include/`, and `tests/` for prohibited artifact-input flags and external-tool invocation. A separate public-header scan rejects native analysis includes/types while allowing only explanatory comments in documentation.

---

# 13. Handoff to M10B

M10B consumes:

```text
CpgQuery
summary-backed flow paths
call paths
memory reader/writer adjacency
alias edges with epistemic state
unknown nodes and edges
provenance refs
```

M6 is complete when Evidence Builder can ask graph questions without knowing whether a relation originated in direct LLVM inspection, M4 extraction, or M5 SVF mapping, while every returned relation remains traceable to its VERITAS provenance.

M12C may later overlay explicitly selected external provider observations at
the SummaryDB query layer. Those observations retain separate provider
projection identity, epistemic state, capabilities, assumptions, and
provenance; they do not become M6 relations.
