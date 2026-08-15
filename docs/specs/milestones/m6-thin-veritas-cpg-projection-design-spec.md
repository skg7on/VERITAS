# M6 Thin VERITAS CPG Projection Design Spec

**Status:** Draft
**Milestone:** M6
**Depends on:** M4 local summaries, M5 SVF facts
**Feeds:** M10 Evidence Builder and query tools

---

# 1. Purpose

M6 builds the VERITAS Code Property Graph as a thin projection over SummaryDB data. The graph is a query index and retrieval surface, not the source of truth.

The persistent graph should be function- and object-centric. Instruction-level detail is generated on demand from Clang/LLVM or cached for a specific Evidence Case.

---

# 2. Reuse Strategy

Use Joern and the Code Property Graph specification as schema inspiration:

```text
AST vocabulary
CFG vocabulary
CALL edges
data-flow vocabulary
type and method concepts
```

Do not require Joern as a runtime dependency in V1.

VERITAS owns:

```text
node IDs
edge IDs
summary-backed graph projection
query API
budgeted path traversal
explicit unknown/summarized edges
```

---

# 3. Persistent Graph Scope

Persistent nodes:

```text
TranslationUnit
Namespace
Type
Function
Parameter
Global
CallSite
MemoryObject
Field
BasicBlockSummary
Summary
Fact
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
MAY_ALIAS
DOMINATES_SUMMARY
SUMMARIZED_BY
SUPPORTED_BY
UNKNOWN_AT
```

Excluded from persistent V1 graph:

```text
every LLVM instruction
every AST expression
every temporary SSA value
full CFG node graph for all functions
```

---

# 4. Graph Identity

Graph node IDs are VERITAS stable IDs:

```text
Function node -> FunctionVariantID
CallSite node -> CallSiteID
MemoryObject node -> MemoryRef ID
Summary node -> FunctionSummaryID
Fact node -> FactID
```

Edge IDs are canonical hashes of:

```text
revision_id
build_variant_id
edge_kind
source_node_id
target_node_id
semantic qualifiers
provenance_id
```

---

# 5. Summary Edges

Interprocedural and internal detail can be represented by summary edges:

```text
FLOWS_TO(packet.len, copyPayload.arg2)
    summarized_by = FunctionSummaryID
    expandable = true
```

This preserves context-efficiency while allowing Evidence Builder to request expansion.

---

# 6. Query API

```cpp
namespace veritas::cpg {
class CpgQuery {
 public:
  std::vector<CpgNode> GetCallees(core::StableId function_variant_id) const;
  std::vector<CpgNode> GetCallers(core::StableId function_variant_id) const;
  std::vector<CpgNode> GetWriters(core::StableId memory_object_id) const;
  std::vector<CpgPath> GetValueFlow(core::StableId src, core::StableId dst, int max_depth) const;
  std::vector<CpgPath> GetCallPaths(core::StableId src, core::StableId dst, int max_depth) const;
};
}
```

Queries must accept a budget:

```text
max_depth
max_nodes
max_paths
```

Budget truncation must be explicit in query results.

---

# 7. Storage Model

V1 can store the graph as typed adjacency tables in SQLite or as RocksDB adjacency lists.

Required logical indexes:

```text
node_id -> node
source_node_id + edge_kind -> outgoing edges
target_node_id + edge_kind -> incoming edges
function_variant_id -> callsites
memory_object_id -> readers/writers
value_ref_id -> flow edges
```

Do not introduce Neo4j or a graph database in V1 unless query needs prove the typed adjacency layer is insufficient.

---

# 8. Acceptance Tests

Required assertions:

```text
function and callsite nodes deduplicate across rebuilds
CALLS edge can cite source anchor and summary ID
FLOWS_TO path traverses summary edges
unknown calls create unknown nodes or MAY_CALL edges, not full fanout
persistent graph size is not proportional to LLVM instruction count
budgeted traversal reports truncation
```

---

# 9. Handoff to M10

M10 consumes:

```text
CpgQuery
summary-backed flow paths
call paths
memory reader/writer adjacency
unknown nodes and edges
provenance refs
```

M6 is complete when Evidence Builder can ask graph questions without knowing whether facts came from Clang, LLVM, or SVF.

