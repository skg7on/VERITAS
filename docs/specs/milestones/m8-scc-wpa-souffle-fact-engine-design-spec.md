# M8 SCC-Aware WPA and Souffle Fact Engine Design Spec

**Status:** Draft
**Milestone:** M8
**Depends on:** M7 scheduler, M6 thin CPG optional projection
**Feeds:** M9 fact store and provenance, M10 Evidence APIs

---

# 1. Purpose

M8 computes whole-program facts from local summaries using SCC-aware fixpoint propagation and recursive fact derivation.

It starts with a C++ fixpoint engine for the first domains, then integrates Souffle for recursive relations once fact schemas stabilize.

---

# 2. Reuse Strategy

Use Souffle for:

```text
recursive call reachability
transitive memory effects
recursive value-flow relations
rule-based derivations over normalized facts
```

VERITAS owns:

```text
base fact export schema
tuple IDs
epistemic joins
SCC state hashes
provenance capture
fact publication
unknown call policy
```

Souffle facts are an execution format, not the durable VERITAS fact model.

---

# 3. SCC Construction

Input call edge kinds:

```text
MUST_CALL
MAY_CALL
UNKNOWN_CALL
```

SCC policy:

```text
MUST_CALL participates in SCC graph.
MAY_CALL participates in SCC graph with MAY epistemic label.
UNKNOWN_CALL does not connect to all functions.
UNKNOWN_CALL creates unknown external effect facts.
```

This avoids one unknown indirect call collapsing the whole program into one SCC.

---

# 4. Fixpoint Domains

Initial domains:

```text
TransitiveCalls: set<FunctionVariantID>
MayRead: set<MemoryRef>
MayWrite: set<MemoryRef>
GlobalValueFlow: relation<ValueRef, ValueRef>
```

Each domain defines:

```text
Bottom
Join
Transfer
Widen
Equivalent
ExternalHash
```

Domains must be monotone or explicitly bounded.

---

# 5. SCC State

```text
SccState {
    scc_id
    component_kind
    fixpoint_hash
    externally_visible_hash
    iteration_count
    status
}
```

Status:

```text
CONVERGED
APPROXIMATED
TIMEOUT
UNSUPPORTED
```

`APPROXIMATED` is valid but must affect fact epistemic state and provenance.

---

# 6. Souffle Relation Contract

Base relations include tuple IDs:

```text
DirectCall(tuple_id, caller, callee, epistemic).
DirectRead(tuple_id, function, memory, epistemic).
DirectWrite(tuple_id, function, memory, epistemic).
LocalFlow(tuple_id, src_value, dst_value, function, epistemic).
MayAlias(tuple_id, memory_a, memory_b, epistemic).
```

Derived relations also carry tuple IDs:

```text
ReachableCall(tuple_id, src, dst, epistemic).
MayWrite(tuple_id, function, memory, epistemic).
GlobalFlow(tuple_id, src_value, dst_value, epistemic).
```

If Souffle cannot export enough provenance for a V1 rule, VERITAS reconstructs immediate derivation inputs from rule-specific joins.

---

# 7. Epistemic Policy

```text
MUST + MUST through sound rule -> MUST
MUST + MAY through sound rule -> MAY
MAY + MAY through sound rule -> MAY
UNKNOWN input -> UNKNOWN or MAY according to rule
APPROXIMATED SCC state -> output confidence reduced or epistemic weakened
```

M8 does not create `INFERRED` facts. Those come from semantic/LLM systems later.

---

# 8. API Contract

```cpp
namespace veritas::wpa {
class SccGraph {
 public:
  static SccGraph Build(const CallGraph& call_graph);
  core::StableId SccForFunction(core::StableId function_variant_id) const;
  std::vector<core::StableId> Members(core::StableId scc_id) const;
  std::vector<core::StableId> Predecessors(core::StableId scc_id) const;
};

class FixpointEngine {
 public:
  StatusOr<SccResult> Compute(
      core::StableId scc_id,
      summary::ComponentKind component_kind);
};
}
```

---

# 9. Acceptance Tests

Required tests:

```text
acyclic graph processes in reverse topological order
self-recursive function converges
mutually recursive group converges as one SCC
unknown call emits unknown fact without full graph fanout
internal SCC change with same external hash stops propagation
MayWrite fact derives through A -> B -> C
derived tuple records rule and input tuple IDs
Souffle disabled build still runs C++ fixpoint tests
```

---

# 10. Handoff to M9

M9 consumes derived fact tuples and provenance hooks. M8 is complete when transitive call and may-write facts are derived, carry tuple IDs, and can be published by the fact store without recomputation.

