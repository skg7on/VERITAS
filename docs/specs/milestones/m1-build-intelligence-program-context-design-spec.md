# M1 Build Intelligence and Program Context Design Spec

**Status:** Draft
**Milestone:** M1
**Depends on:** M0 project skeleton
**Feeds:** M2 identity metadata, M4 Clang/LLVM extraction, all later SummaryDB bindings

---

# 1. Purpose

M1 defines the immutable program context that every later VERITAS fact must cite. It ingests a C/C++ compilation database, normalizes build commands, and emits an analysis manifest containing repository, revision, build variant, and translation unit records.

The goal is not to analyze code yet. The goal is to make later analysis reproducible:

```text
compile_commands.json
    -> normalized analysis manifest
    -> repository/revision/build_variant/translation_unit IDs
    -> deterministic input set for local extraction
```

Every summary, CPG node, derived fact, and Evidence IR slice depends on the program context created here.

---

# 2. Reuse Strategy

M1 should reuse Clang LibTooling's compilation database support instead of writing a custom parser for `compile_commands.json`.

VERITAS owns:

* canonical path normalization,
* command argument normalization,
* build variant hash inputs,
* manifest schema,
* deterministic serialization,
* explicit treatment of generated source and non-Git source roots.

Clang owns:

* parsing JSON compilation databases,
* expanding compile commands into file-level tool invocations,
* compatibility with common CMake compile database behavior.

---

# 3. Scope

M1 includes:

* repository root discovery,
* source tree manifest hashing,
* compilation database loading,
* translation unit command normalization,
* build variant construction,
* diagnostic JSON manifest output,
* CLI command `veritas-build configure`.

M1 excludes:

* function identity,
* source AST extraction,
* LLVM IR generation,
* SummaryDB storage,
* CPG construction,
* incremental invalidation.

---

# 4. Program Context Model

```text
ProgramContext {
    repository_id
    revision_id
    build_variant_id
    root_path
    vcs_kind
    vcs_revision
    source_tree_hash
    target_triple
    compiler_id
    compiler_version
    compile_options_hash
    macro_set_hash
    include_closure_hash
    type_layout_hash
}
```

M1 can initially set `type_layout_hash` to a deterministic placeholder value derived from target and compiler configuration. M4 will replace it with frontend-derived type layout data.

---

# 5. Translation Unit Manifest

```text
TranslationUnitCommand {
    translation_unit_id
    revision_id
    build_variant_id
    source_path
    directory
    arguments[]
    command_hash
    preprocessor_hash
}
```

Rules:

* `source_path` is repository-relative when the file is inside the repository.
* Generated files outside the repository are represented with a stable generated-source root alias.
* `arguments[]` are normalized without changing compiler semantics.
* Nonsemantic environment variables are excluded.
* Compiler path is normalized into `compiler_id` plus `compiler_version` when possible.
* Absolute local paths do not enter semantic hashes unless they identify an external source root.

---

# 6. Manifest Serialization

The manifest should have two serializations:

```text
Diagnostic JSON
    human inspection and golden tests

Canonical bytes
    ID/hash input for later metadata storage
```

JSON output must be deterministic:

* sorted translation units by source path then command hash,
* sorted map keys,
* stable newline behavior,
* no timestamps,
* no host-specific temporary paths.

---

# 7. CLI Contract

```bash
veritas-build configure \
    --compile-db build/compile_commands.json \
    --repo-root . \
    --output .veritas/manifest.json
```

Required output summary:

```text
Repository: <repository_id>
Revision: <revision_id>
Build Variant: <build_variant_id>
Translation Units: <count>
Manifest: .veritas/manifest.json
```

Failure modes:

```text
compile database missing -> InvalidArgument
compile database parse failure -> InvalidArgument
source file missing -> FailedPrecondition unless --allow-missing-source
no translation units -> FailedPrecondition
```

---

# 8. Design Constraints

* The manifest is an input artifact, not a mutable database.
* A manifest generated twice from equivalent inputs must be byte-identical.
* M1 must not require RocksDB, SVF, Souffle, or CPG storage.
* M1 may use SQLite only if M2's metadata store already exists; otherwise file output is enough.
* The manifest should be small enough to inspect for a 100-file fixture.

---

# 9. Acceptance Tests

Fixtures:

```text
tests/fixtures/cpp/smoke
tests/fixtures/cpp/multiple_tus
tests/fixtures/cpp/generated_source
```

Assertions:

```text
same compile database -> identical manifest bytes
translation units are sorted deterministically
relative paths are stable
missing compile database fails with InvalidArgument
source tree hash changes when a source file changes
source tree hash does not include temporary output directories
```

---

# 10. Handoff to M2

M2 consumes:

```text
ProgramContext
TranslationUnitCommand[]
canonical manifest bytes
diagnostic manifest JSON
```

M1 is complete when M2 can compute and store repository, revision, build variant, and translation unit rows without re-parsing `compile_commands.json`.

