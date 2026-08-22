# M1 Project Ingestion and Program Context Design Spec

**Status:** Draft
**Milestone:** M1
**Depends on:** M0 project skeleton and required third-party toolchain
**Feeds:** M2 identity metadata, M4 Clang/LLVM extraction, and every later SummaryDB binding

---

# 1. Purpose

M1 defines the immutable program context that every later VERITAS fact must cite. The user gives VERITAS one project directory. That directory contains `compile_commands.json`; VERITAS validates and loads the compilation database, normalizes its commands, and constructs the repository, revision, build-variant, and translation-unit records used by the rest of the analysis pipeline.

M1 is an internal stage of whole-project analysis, not a standalone preprocessing tool:

```text
project directory
    -> <project>/compile_commands.json
    -> VERITAS project ingestion
    -> normalized in-memory analysis manifest
    -> Clang AST and LLVM IR stages owned by VERITAS
    -> required SVF stage owned and invoked by VERITAS
```

VERITAS may serialize the manifest for diagnostics or caching, but later stages receive the typed in-memory result. They must not ask the user to run a separate configure command or provide a manifest, AST, bitcode file, LLVM module path, or SVF artifact.

---

# 2. Public Project Input Contract

The standard whole-project command is:

```bash
veritas-build analyze --project <project-directory>
```

The project directory is the source root and must contain:

```text
<project-directory>/compile_commands.json
```

Optional output and resource-policy flags may be added, but `--project` is the only required source-input flag. The standard analysis command must not accept any of the following as alternate public entry points:

```text
--compile-db <path>
--manifest <path>
--bitcode <path>
--llvm-module <path>
--svf-input <path>
```

Programmatic callers use the same project-level contract:

```cpp
namespace veritas::analysis {
struct ProjectAnalysisRequest {
  std::filesystem::path project_root;
  std::filesystem::path output_root;
};
}
```

`output_root` defaults to `<project_root>/.veritas`. It is an output location and never participates in source-tree or build-variant hashes.

---

# 3. Reuse and Ownership Strategy

M1 uses Clang LibTooling's compilation-database support instead of implementing a custom JSON parser.

Clang owns:

* parsing the JSON compilation database,
* expanding compilation entries into file-level commands,
* compatibility with common CMake-generated command forms.

VERITAS owns:

* canonicalizing and validating the project root,
* locating `<project_root>/compile_commands.json`,
* rejecting commands that cannot be mapped into the project analysis context,
* canonical path and command-argument normalization,
* build-variant and source-tree hash inputs,
* the analysis-manifest schema and deterministic serialization,
* generated-source and external-source root classification,
* passing the typed result directly to VERITAS-owned AST and IR stages.

No external script or helper executable may parse the compilation database on behalf of the standard pipeline.

---

# 4. Scope

M1 includes:

* project-root validation,
* fixed-location compilation-database discovery,
* repository and revision discovery,
* source-tree manifest hashing,
* translation-unit command normalization,
* build-variant construction,
* deterministic in-memory manifest construction,
* optional diagnostic serialization under `.veritas`,
* project-input validation for `veritas-build analyze`.

M1 excludes:

* function identity,
* Clang AST traversal,
* LLVM IR generation or linking,
* SVF execution,
* SummaryDB storage,
* CPG construction,
* incremental invalidation.

Those exclusions are milestone boundaries, not user-visible tool boundaries. M4 and M5 run immediately downstream in the same VERITAS-owned analysis invocation.

---

# 5. Internal API Contract

```cpp
namespace veritas::build {
enum class PathRootKind {
  kRepository,
  kGenerated,
  kExternal,
  kToolchain,
};

struct TaggedPath {
  PathRootKind root_kind;
  std::string root_id;
  std::filesystem::path relative_path;
};

struct ProjectInput {
  std::filesystem::path project_root;
  std::filesystem::path compile_database_path;
  std::filesystem::path output_root;
};

struct ProgramContext {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::filesystem::path project_root;
  std::string vcs_kind;
  std::string vcs_revision;
  std::string source_tree_hash;
  std::string compilation_database_hash;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
  std::string compile_options_hash;
  std::string macro_set_hash;
  std::string include_closure_hash;
  std::string type_layout_hash;
};

struct TranslationUnitCommand {
  std::string translation_unit_id;
  std::string revision_id;
  std::string build_variant_id;
  TaggedPath source_path;
  TaggedPath working_directory;
  std::vector<std::string> arguments;
  std::string command_hash;
  std::string preprocessor_hash;
};

struct AnalysisManifest {
  ProgramContext context;
  std::vector<TranslationUnitCommand> translation_units;
};

StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request);

StatusOr<AnalysisManifest> LoadProjectManifest(
    const ProjectInput& input);
}
```

`compile_database_path` always resolves to `<project_root>/compile_commands.json`. It is retained as runtime metadata, not as a host-dependent semantic identity.

M1 may initially derive `type_layout_hash` from the target and compiler configuration. M4 replaces it with frontend-derived layout data before publishing analysis results.

---

# 6. Translation-Unit Normalization

Rules:

* Source paths inside the project use repository-relative `TaggedPath` values.
* Generated files outside the source root use stable generated-root aliases.
* External source and toolchain paths use explicit external/toolchain root tags.
* Arguments are normalized without reordering flags whose order affects compilation.
* Output-only flags may be removed from semantic hashes only when doing so cannot change generated IR.
* Nonsemantic environment values are excluded.
* Compiler paths are normalized to compiler identity and version where possible.
* Absolute checkout, temporary, and output paths do not enter semantic hashes.
* Commands that reference missing source files fail the project load; partial project ingestion is not valid input to the required full-analysis pipeline.

The normalized command remains sufficient for M4 to execute Clang itself. M1 does not run the compiler or delegate command execution to a generated shell script.

---

# 7. Manifest Serialization and Lifetime

The typed `AnalysisManifest` is the authoritative handoff. It has two optional serializations:

```text
Diagnostic JSON
    human inspection, golden tests, and troubleshooting

Canonical bytes
    ID, cache-key, and later metadata-store inputs
```

Both are produced by VERITAS libraries inside the analysis process. Diagnostic serialization is not a required intermediate file and cannot be supplied as a substitute for `--project`.

Serialization requirements:

* translation units sorted by source path and then command hash,
* sorted map keys,
* stable newline behavior,
* no timestamps,
* no checkout-specific or temporary paths,
* byte-identical output for equivalent projects and commands.

---

# 8. Error and Completion Semantics

```text
project directory missing
    -> InvalidArgument

project path is not a directory
    -> InvalidArgument

<project>/compile_commands.json missing
    -> FailedPrecondition

compilation database malformed or empty
    -> InvalidArgument or FailedPrecondition

translation-unit source missing
    -> FailedPrecondition

command cannot be normalized without changing semantics
    -> FailedPrecondition
```

M1 returns no usable manifest on these failures. The standard full-analysis command stops before AST, LLVM, or SVF analysis and reports the failing project path and translation-unit context without silently dropping input files.

---

# 9. Design Constraints

* Project directory is the sole public source-input abstraction.
* The manifest is internal typed state, not a public workflow boundary.
* Equivalent input projects produce byte-identical canonical manifests.
* M1 does not expose Clang command objects to downstream public APIs.
* M1 does not require the user to invoke Clang, LLVM, or SVF tools.
* The required SVF submodule is a build-level dependency established by M0; M1 does not call SVF yet.
* Diagnostic output remains small enough to inspect for a 100-file fixture.

---

# 10. Acceptance Tests

Fixtures:

```text
tests/fixtures/projects/smoke/
    compile_commands.json
    smoke.cpp

tests/fixtures/projects/multiple_tus/
tests/fixtures/projects/generated_source/
tests/fixtures/projects/missing_compile_database/
```

Required assertions:

```text
project root resolves exactly <project>/compile_commands.json
reordered compilation entries produce identical canonical manifest bytes
translation units are sorted deterministically
repository-relative paths remain stable across checkout roots
missing or malformed compilation database fails the whole project load
missing translation-unit source fails without producing a partial manifest
source-tree hash changes when a source file changes
source-tree hash ignores .veritas and temporary output directories
public CLI parsing requires --project and exposes no bitcode/module/SVF input flag
diagnostic serialization is optional and is not consumed by downstream stages
```

---

# 11. Handoff to M2 and M4

M2 consumes the typed `ProgramContext`, translation-unit records, canonical manifest bytes, and optional diagnostic JSON.

M4 consumes the same in-memory `AnalysisManifest` and executes Clang commands itself to construct AST facts and LLVM IR. M4 must not re-parse `compile_commands.json` or ask for a manifest filename.

M1 is complete when a valid project directory produces deterministic typed analysis input that can flow directly into M2 and M4 within one `veritas-build analyze` invocation.
