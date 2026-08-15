# M4 VERITAS-Owned Clang/LLVM Project Analysis Design Spec

**Status:** Draft
**Milestone:** M4
**Depends on:** M1 manifest, M2 identity, M3 summary storage
**Feeds:** M5 SVF adapter, M6 thin CPG, M7 dependency index

---

# 1. Purpose

M4 extracts local semantic facts from real C/C++ translation units using Clang and LLVM. It receives the typed M1 project manifest inside the active `veritas-build analyze --project <directory>` invocation, executes every normalized compile command itself, and produces FunctionSummary inputs with direct calls, source anchors, CFG summary facts, dominator facts, memory operations, local value references, and initial unknowns.

M4 also generates per-translation-unit LLVM modules and links them into a private, in-memory whole-program `ProgramIr` for required M5 SVF analysis. AST and IR construction are VERITAS-owned pipeline stages, not standalone user workflows.

M4 must not persist Clang AST node pointers or LLVM `Value*` addresses. It converts frontend objects into VERITAS stable references and retains native objects only for the lifetime of the private M4/M5 pipeline.

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
execution of normalized compile commands
Clang FrontendAction orchestration
LLVM IR emission and whole-program linking
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

The production pipeline does not invoke `clang`, `llvm-link`, or `opt` as user-managed prerequisites. VERITAS may use Clang and LLVM library APIs internally and may cache VERITAS-generated IR as a content-addressed implementation detail.

---

# 3. Project Analysis Boundary

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

M4 receives `build::AnalysisManifest` directly from M1. It must not re-open a user-supplied manifest or accept prebuilt `.bc`/`.ll` files. For every translation unit, it uses the same normalized command to coordinate AST extraction and IR generation, preserving a stable origin map between Clang declarations, LLVM values, and VERITAS identities.

After local extraction, M4 links all compatible translation-unit modules into one move-only `pipeline::ProgramIr`. Failure to parse a required translation unit, generate its IR, or link the complete project is a full-analysis failure; M4 does not silently publish a partial project as complete.

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
struct ProjectAstIndex {
  std::vector<ExtractedFunctionDecl> declarations;
};

class ClangExtractor {
 public:
  StatusOr<ProjectAstIndex> ExtractProject(
      const build::AnalysisManifest& manifest);
};
}
```

```cpp
namespace veritas::analysis::llvm {
class ProjectIrBuilder {
 public:
  StatusOr<pipeline::ProgramIr> BuildProjectIr(
      const build::AnalysisManifest& manifest,
      const frontend::clang::ProjectAstIndex& ast_index);
};
}
```

`pipeline::ProgramIr` is declared only in private source-tree headers. It owns the `LLVMContext`, linked `llvm::Module`, local IR facts, and LLVM-to-VERITAS origin maps needed by M5. No installed public header contains this type or an LLVM native type.

```cpp
namespace veritas::summary {
StatusOr<v1::FunctionSummary> BuildLocalSummary(
    const ExtractedFunctionDecl& decl,
    const analysis::llvm::LocalIrFacts& facts,
    const SummaryBuildContext& context);
}
```

The project-level public API remains `analysis::ProjectAnalyzer::AnalyzeProject(ProjectAnalysisRequest, AnalysisConfig)`. `ClangExtractor`, `ProjectIrBuilder`, and `ProgramIr` are internal stages called by that orchestrator.

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
one project-directory request drives compilation-database loading, AST extraction, and IR generation
all translation units in the manifest are processed or the project analysis fails
overloads produce distinct FunctionSymbolIDs
file-local functions do not collide
macro source anchors include spelling and expansion locations
direct call emits MUST_CALL
function pointer emits UNKNOWN_CALL or MAY_CALL with candidates
memcpy callsite is represented
unsupported inline assembly emits UnknownFact
multi-translation-unit fixture produces one linked in-memory ProgramIr
public CLI accepts no manifest, bitcode, or LLVM-module input
tests do not require user-invoked clang, llvm-link, or opt preprocessing
```

---

# 10. Handoff to M5

M5 synchronously borrows the live, linked `ProgramIr` and its local `ValueRef`/`MemoryRef` origin maps. M5 does not load a module pathname or bitcode supplied by the user.

M4 is independently testable when it can build deterministic local summaries and a linked `ProgramIr` from an M1 manifest. In the standard product build and `veritas-build analyze` workflow, that `ProgramIr` always continues into the required M5 SVF stage before the project analysis is complete.
