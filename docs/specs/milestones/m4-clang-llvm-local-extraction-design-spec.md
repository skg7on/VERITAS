# M4 Clang/LLVM Local Extraction Adapter Design Spec

**Status:** Draft
**Milestone:** M4
**Depends on:** M1 manifest, M2 identity, M3 summary storage
**Feeds:** M5 SVF adapter, M6 thin CPG, M7 dependency index

---

# 1. Purpose

M4 extracts local semantic facts from real C/C++ translation units using Clang and LLVM. It produces FunctionSummary objects with direct calls, source anchors, CFG summary facts, dominator facts, memory operations, local value references, and initial unknowns.

M4 must not persist Clang AST node pointers or LLVM `Value*` addresses. It converts frontend objects into VERITAS stable references.

---

# 2. Reuse Strategy

Use Clang for:

```text
C/C++ semantic AST
canonical declarations and USRs
templates and overloads
source locations
macro spelling and expansion locations
compile command execution
```

Use LLVM for:

```text
SSA values
IR-level CFG
dominator tree
MemorySSA where available
instruction-level memory operations
basic callsite classification
```

VERITAS owns:

```text
FunctionSymbolID
FunctionVariantID
FunctionBodyID
SourceAnchorID
ValueRef
MemoryRef
CallFact
MemoryEffectFact
RangeFact
UnknownFact
```

---

# 3. Local Extraction Boundary

M4 records direct/local facts only. It does not expand callees.

Example:

```text
decodeIE calls validateIE
decodeIE calls copyPayload
packet.len flows to copyPayload.arg2
validateLength dominates memcpy
vendorValidate has unknown postcondition
```

M8 computes transitive effects later.

---

# 4. Function Discovery

For every definition and relevant declaration:

```text
qualified_name
mangled_name
canonical_signature
linkage_kind
template_identity
source_anchor
translation_unit_id
```

Function symbol identity rules:

* overloaded functions are distinct,
* template specializations are distinct when semantics differ,
* internal-linkage functions include enclosing translation unit identity,
* declarations and definitions are linked when Clang can prove they are the same semantic entity.

---

# 5. Source Anchors

Source anchors preserve diagnostics without becoming semantic identity:

```text
path
start_line
start_column
end_line
end_column
spelling_location
expansion_location
macro_expansion_stack
```

Macro-heavy code must preserve both spelling and expansion context.

---

# 6. Local Fact Model

Required summary components:

```text
calls:
    direct calls
    unresolved function pointer calls
    virtual dispatch candidates when Clang/LLVM can enumerate them

memory_effects:
    local loads
    local stores
    calls to known alloc/free/memcpy/memset/memmove

value_flows:
    assignment
    parameter passing
    returns
    load/store local edges
    phi/select local edges

range_facts:
    literal bounds
    simple comparison-derived constraints
    null checks

unknowns:
    unresolved call target
    inline assembly
    unsupported language feature
    external function without model
```

---

# 7. API Contract

```cpp
namespace veritas::frontend::clang {
class ClangExtractor {
 public:
  StatusOr<std::vector<ExtractedFunctionDecl>> ExtractDeclarations(
      const build::TranslationUnitCommand& command);
};
}
```

```cpp
namespace veritas::analysis::llvm {
class LlvmExtractor {
 public:
  StatusOr<LocalIrFacts> ExtractLocalFacts(
      const build::TranslationUnitCommand& command,
      const core::StableId& function_variant_id);
};
}
```

```cpp
namespace veritas::summary {
StatusOr<v1::FunctionSummary> BuildLocalSummary(
    const ExtractedFunctionDecl& decl,
    const analysis::llvm::LocalIrFacts& facts,
    const SummaryBuildContext& context);
}
```

---

# 8. Thin CPG Inputs

M4 must emit enough data for M6:

```text
Function nodes
Parameter nodes
CallSite nodes
BasicBlockSummary nodes
CALLS/MAY_CALL edges
READS/WRITES edges
DOMINATES_SUMMARY edges
source anchor refs
```

It must not persist full instruction graphs globally.

---

# 9. Acceptance Tests

Required fixtures:

```text
direct call
static internal-linkage functions in two files
overloaded functions
template specialization
macro-expanded callsite
simple memcpy path
function pointer call
inline assembly
```

Required assertions:

```text
overloads produce distinct FunctionSymbolIDs
file-local functions do not collide
macro source anchors include spelling and expansion locations
direct call emits MUST_CALL
function pointer emits UNKNOWN_CALL or MAY_CALL with candidates
memcpy callsite is represented
unsupported inline assembly emits UnknownFact
```

---

# 10. Handoff to M5

M5 consumes LLVM module inputs and the local `ValueRef`/`MemoryRef` mapping created here. M4 is complete when local summaries can be produced without SVF and published through M3.

