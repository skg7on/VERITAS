# M5 Required In-Process SVF Value-Flow and Pointer Analysis Design Spec

**Status:** Draft
**Milestone:** M5
**Depends on:** M4 VERITAS-owned Clang/LLVM project analysis
**Feeds:** M6 thin CPG, M7 dependency index, M8 WPA, and M10 Evidence APIs

---

# 1. Purpose

M5 integrates SVF as a required third-party library for whole-program pointer analysis, value-flow graph construction, indirect-call refinement, and memory/value-flow facts.

SVF is part of the standard VERITAS build and the standard whole-project analysis pipeline. It is not an optional capability, a separately installed executable, or a standalone preprocessing tool. VERITAS owns project ingestion, Clang AST traversal, LLVM IR generation and linking, SVF invocation, fact normalization, and publication.

```text
veritas-build analyze --project <directory>
    -> M1 loads <directory>/compile_commands.json
    -> M4 runs Clang and builds VERITAS-owned AST facts and LLVM IR
    -> M4 links the translation-unit IR into one in-memory ProgramIr
    -> M5 invokes the linked SVF libraries on that ProgramIr
    -> M5 maps SVF results into VERITAS Summary IR
```

SVF is not VERITAS's public data model. No public VERITAS API accepts or returns an SVF node, graph, identifier, command-line artifact, or bitcode pathname.

---

# 2. Required Third-Party Dependency

SVF is pinned as a Git submodule:

```text
third_party/SVF
```

The repository records:

```text
.gitmodules entry
exact SVF gitlink revision
upstream repository URL
license and attribution metadata
compatible LLVM configuration
```

The standard CMake configuration must:

1. verify that `third_party/SVF` is initialized at the recorded gitlink revision,
2. add the pinned source tree with `add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)`,
3. link a private VERITAS wrapper target to the pinned SVF core and LLVM targets,
4. build SVF against the same LLVM installation used by VERITAS,
5. reject incompatible LLVM version, RTTI, exception, target, or ABI settings,
6. fail configuration with a command showing how to initialize the submodule when it is absent.

There is no `VERITAS_ENABLE_SVF` option and no SVF-disabled standard build. There is no `FindSVF.cmake` path that silently substitutes an arbitrary system installation.

SVF is distributed under AGPL-3.0-or-later at the time of this design. The repository must preserve upstream license notices and document the pinned dependency for project licensing review. This design records the engineering requirement and does not replace legal review.

---

# 3. Ownership and Isolation Boundary

Use SVF for:

```text
points-to analysis
value-flow graph construction
load/store relation refinement
indirect-call target candidates
interprocedural value-flow candidates
field-sensitive facts when available
demand-driven pointer refinements when configured
```

VERITAS owns:

```text
project and compilation-database input
Clang invocation and AST extraction
LLVM IR generation and whole-program linking
the lifetime of LLVMContext and llvm::Module
the SVF library call and lifecycle cleanup
ValueRef and MemoryRef identity
AliasFact and ValueFlowFact semantics
component hashing and Summary IR publication
source-anchor preservation
analysis status, provenance, budgets, and unknown policy
```

Isolation rules:

* SVF headers appear only in `src/analysis/svf` implementation files and the private third-party wrapper target.
* LLVM native types used to call SVF remain inside the private M4/M5 pipeline implementation.
* Public headers expose only VERITAS types.
* SVF node IDs are transient lookup keys and are never persisted as VERITAS identities.
* SVF command-line tools are not built or invoked by the VERITAS analysis path unless an upstream target is an unavoidable build dependency; their outputs are never pipeline inputs.

---

# 4. Project IR Handoff

M4 produces a private, move-only `ProgramIr` that owns:

```text
LLVMContext
linked whole-program llvm::Module
translation-unit and function origin map
LLVM value -> VERITAS ValueRef map
LLVM memory object -> VERITAS MemoryRef map
source anchors derived from Clang
module and build-variant identity
```

`ProgramIr` is an internal implementation type under `src/analysis/pipeline`; it is not installed as a public header. M5 borrows it synchronously while the LLVM objects and source maps are alive.

VERITAS generates per-translation-unit IR from the normalized commands supplied by M1 and links those modules itself. The production analysis API must not accept:

```text
user-supplied .bc or .ll files
an LLVM module path
a serialized SVF graph
an output directory from an SVF executable
```

Content-addressed IR caching is permitted as an internal optimization. A cache entry is created and validated by VERITAS, keyed by the normalized translation-unit command, source inputs, compiler identity, LLVM schema/version, and build variant. Cache files cannot bypass the project-level input contract.

---

# 5. Pipeline API Contract

The public entry point remains project-level and contains no Clang, LLVM, or SVF types:

```cpp
namespace veritas::analysis {
enum class AnalysisCompletion {
  kComplete,
  kCompleteWithUnknowns,
};

struct ProjectAnalysisResult {
  AnalysisCompletion completion;
  core::StableId program_context_id;
  std::vector<core::StableId> published_summary_ids;
  std::vector<summary::UnknownFact> unknowns;
};

class ProjectAnalyzer {
 public:
  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request,
      const AnalysisConfig& config);
};
}
```

The private M5 stage may use the following implementation contract:

```cpp
namespace veritas::analysis::svf {
struct SvfFacts {
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::AliasFact> aliases;
  std::vector<summary::MemoryEffectFact> refined_memory_effects;
  std::vector<summary::CallFact> refined_calls;
  std::vector<summary::UnknownFact> unknowns;
  std::vector<summary::DependencyEdge> dependencies;
};

StatusOr<SvfFacts> AnalyzeProgramIr(
    pipeline::ProgramIr& program_ir,
    const SvfConfig& config,
    const AnalyzerRunContext& run_context);
}
```

This private function is called only by `ProjectAnalyzer`. It is not a separate user-facing tool or reusable public ingestion boundary.

---

# 6. Configuration and Analyzer Identity

```text
SvfConfig {
    pointer_analysis_level:
        basic
        andersen
        demand

    soft_analysis_budget_seconds
    max_graph_nodes
    max_emitted_facts
    field_sensitivity
}
```

The standard configuration always runs SVF. Every value that can alter emitted facts contributes to `AnalyzerRunID`, together with:

```text
SVF pinned commit
LLVM version and ABI properties
VERITAS adapter version
Summary IR schema version
whole-program module hash
```

The configuration may select an SVF analysis mode, but it cannot disable the SVF stage.

---

# 7. SVF Lifecycle

The adapter owns the complete SVF lifecycle for one project analysis:

1. receive the live, linked `ProgramIr`,
2. construct the SVF module directly from its `llvm::Module`,
3. build the SVF IR and configured pointer/value-flow analysis,
4. translate results while both LLVM and SVF objects are alive,
5. release SVF singleton/global state before the next project analysis,
6. return only VERITAS facts and provenance references.

Until the pinned SVF revision is proven safe for concurrent independent contexts, VERITAS serializes the SVF stage inside one process. Parallel Clang/IR preparation may occur before that serialized stage.

Cleanup runs on success and every error path. No result may retain a pointer or reference into SVF or LLVM storage after `AnalyzeProgramIr` returns.

---

# 8. Fact Mapping

SVF outputs are normalized into:

```text
ValueFlowFact
AliasFact
MemoryEffectFact
CallFact
UnknownFact
DependencyEdge
ProvenanceRef
```

Alias mapping:

```text
SVF must-alias evidence -> MUST_ALIAS
SVF may-alias evidence -> MAY_ALIAS
SVF no-alias evidence -> NO_ALIAS
unsupported or truncated query -> UNKNOWN_ALIAS
```

Value-flow mapping:

```text
SVF value-flow edge
    -> VERITAS ValueFlowFact
    -> source and destination ValueRef
    -> optional MemoryRef and field path
    -> epistemic state
    -> provenance citing AnalyzerRunID and LLVM/source origins
```

Indirect-call mapping:

```text
resolved singleton target with sufficient evidence -> bounded MUST_CALL only when justified
multiple feasible targets -> MAY_CALL facts
empty or truncated candidate set -> UNKNOWN_CALL
```

Every mapped fact must resolve through the M4 origin maps. An SVF result that cannot be mapped becomes a scoped `UnknownFact`; it is never published with a raw SVF identifier.

---

# 9. Merge Rules

M5 refines M4 summaries; it does not replace their source-grounded facts.

```text
M4 direct calls remain authoritative for direct-call syntax.
SVF value-flow facts augment the ValueFlow component.
SVF alias facts augment the AliasFacts component.
SVF indirect-call candidates refine unresolved M4 callsites.
SVF memory effects may add conservative MAY effects.
SVF cannot remove an M4 MUST fact unless a verifier-quality contradiction policy exists.
SVF unknowns remain explicit in the Unknowns component.
Clang spelling and expansion anchors remain the display and evidence anchors.
```

Deduplication uses VERITAS fact identity after mapping, not SVF node or edge identity.

---

# 10. Failure, Budget, and Completion Semantics

SVF is required, so these are fatal project-analysis failures:

```text
submodule missing or uninitialized
SVF or LLVM ABI incompatibility
failure to build the in-memory SVF module
SVF internal failure that invalidates all results
failure to release required SVF state safely
```

Supported precision or resource limits may complete with explicit unknowns:

```text
partial results available at a supported checkpoint
    -> publish validated partial facts
    -> emit scope-specific UnknownFact records
    -> return kCompleteWithUnknowns

query or emission budget exhausted
    -> mark affected scope truncated
    -> preserve budget provenance
    -> return kCompleteWithUnknowns

unsupported language/IR construct
    -> emit a scoped UnknownFact
    -> continue when the remaining analysis is valid
```

VERITAS must not claim a hard in-process timeout that the pinned SVF API cannot interrupt safely. Any hard-isolation worker design is a later architecture decision; it must remain an internal VERITAS implementation detail and cannot introduce a user-managed SVF preprocessing step.

---

# 11. Thin CPG Inputs

M5 emits VERITAS-normalized inputs for:

```text
FLOWS_TO edges
MAY_ALIAS and NO_ALIAS edges
resolved or bounded MAY_CALL edges
summary edge annotations
MemoryObject refs
Field refs when field-sensitive data is available
```

The CPG projection cites VERITAS IDs, source anchors, summary IDs, and provenance references. It never stores SVF node IDs as graph identity.

---

# 12. Acceptance Tests

Build and dependency assertions:

```text
clean recursive clone initializes third_party/SVF at the recorded revision
standard CMake build produces the required SVF-linked VERITAS analysis target
missing submodule fails configuration with an initialization command
incompatible LLVM configuration fails before compilation or analysis
there is no VERITAS_ENABLE_SVF option or SVF-disabled standard build
license and attribution metadata identify the pinned SVF dependency
```

End-to-end fixture projects:

```text
parameter to return
pointer store then load
field access
alias through pointer assignment
function pointer callback
multiple translation units requiring IR linking
unsupported construct producing an unknown
```

Required analysis assertions:

```text
veritas-build analyze --project <fixture> owns every stage from compile database through SVF
no test invokes clang, llvm-link, opt, wpa, or another SVF executable as a prerequisite
arg0 -> return is emitted
store/load relation includes alias provenance
field path is preserved when available
function-pointer targets refine M4 call facts
unsupported or truncated analysis produces UnknownFact
public CLI exposes no manifest, bitcode, LLVM-module, or SVF-input flag
public headers contain no SVF types and no project-input API contains LLVM types
repeated analysis releases SVF state and produces deterministic facts
```

---

# 13. Handoff to M6

M6 consumes only SVF-normalized facts through Summary IR and provenance APIs.

M5 is complete when the standard required build contains the pinned SVF submodule, one project-directory command runs VERITAS-owned compilation-database ingestion, AST/IR construction, in-process SVF analysis, and Summary IR publication, and no public contract or required user workflow exposes SVF-native or prebuilt-IR inputs.
