# M5 SVF Value-Flow and Pointer Adapter Design Spec

**Status:** Draft
**Milestone:** M5
**Depends on:** M4 local extraction
**Feeds:** M6 thin CPG, M7 dependency index, M8 WPA, M10 Evidence APIs

---

# 1. Purpose

M5 integrates SVF as an optional analysis provider for pointer analysis, value-flow graph extraction, and improved memory/value-flow facts.

SVF is not VERITAS's internal model. The adapter translates SVF output into VERITAS Summary IR components while preserving uncertainty and provenance.

---

# 2. Reuse Strategy

Use SVF for:

```text
points-to analysis
value-flow graph construction
load/store relation refinement
interprocedural value-flow candidates
field-sensitive facts when available
demand-driven pointer refinements when configured
```

VERITAS owns:

```text
adapter boundary
ValueRef and MemoryRef identity
AliasFact semantics
ValueFlowFact semantics
component hashes
timeout and unknown policy
source anchor preservation through Clang/LLVM mapping
```

No code outside `analysis/svf` should include SVF headers.

---

# 3. Configuration

```text
SvfConfig {
    pointer_analysis_level:
        basic
        andersen
        demand

    max_analysis_seconds
    max_memory_mb
    emit_unknowns_on_timeout
    field_sensitivity
}
```

The selected config is part of analyzer identity. If a config value can alter emitted facts, it contributes to `AnalyzerRunID`.

---

# 4. Fact Mapping

SVF outputs are normalized into:

```text
ValueFlowFact
AliasFact
MemoryEffectFact
UnknownFact
DependencyEdge
ProvenanceRef
```

Alias mapping:

```text
SVF must-alias evidence -> MUST_ALIAS
SVF may-alias evidence -> MAY_ALIAS
SVF no-alias evidence -> NO_ALIAS
timeout or unsupported query -> UNKNOWN_ALIAS
```

Value-flow mapping:

```text
SVF value-flow edge
    -> VERITAS ValueFlowFact
    -> source/destination ValueRef
    -> optional MemoryRef
    -> epistemic state
    -> provenance ref to SVF analyzer run and LLVM source values
```

---

# 5. Timeout and Precision Policy

SVF can be expensive. M5 must never silently omit uncertain facts.

Timeout behavior:

```text
partial facts available -> publish partial facts with analyzer budget provenance
query timeout -> emit UnknownFact
module timeout -> emit UnknownFact scoped to translation unit or function
```

Precision behavior:

```text
over-approximation -> MAY
proven no-alias -> MUST_NOT alias predicate or NO_ALIAS fact
unsupported construct -> UNKNOWN
```

---

# 6. API Contract

```cpp
namespace veritas::analysis::svf {
struct SvfFacts {
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::AliasFact> aliases;
  std::vector<summary::MemoryEffectFact> refined_memory_effects;
  std::vector<summary::UnknownFact> unknowns;
  std::vector<summary::DependencyEdge> dependencies;
};

class SvfAdapter {
 public:
  StatusOr<SvfFacts> AnalyzeModule(
      const LlvmModuleInput& module,
      const SvfConfig& config);
};
}
```

---

# 7. Summary Merge Rules

M5 refines M4 summaries. It does not replace them wholesale.

Merge rules:

```text
M4 direct call facts remain authoritative for direct calls.
SVF value-flow facts augment ValueFlow component.
SVF alias facts augment AliasFacts component.
SVF refined memory effects can add MAY effects.
SVF cannot remove M4 MUST facts unless a verifier-quality no-fact is available.
SVF unknowns are preserved in Unknowns component.
```

---

# 8. Thin CPG Inputs

M5 should emit:

```text
FLOWS_TO edges
MAY_ALIAS edges
summary edge annotations
MemoryObject refs
Field refs when field-sensitive data is available
```

The CPG projection must cite VERITAS refs, not SVF node IDs.

---

# 9. Acceptance Tests

Required fixtures:

```text
parameter to return
pointer store then load
field access
aliasing through pointer assignment
function pointer callback
SVF timeout fixture or injected timeout fake
```

Required assertions:

```text
arg0 -> return is emitted
store/load relation includes alias provenance
field path is preserved when available
timeout produces UnknownFact
SVF disabled build still compiles non-SVF milestones
public headers outside analysis/svf do not include SVF headers
```

---

# 10. Handoff to M6

M6 consumes SVF-normalized facts through Summary IR. M5 is complete when value-flow and alias components improve over M4 while VERITAS public contracts remain free of SVF-native types.

