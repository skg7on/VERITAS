# M1 Build Intelligence and Program Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build deterministic ingestion of `compile_commands.json` into a VERITAS analysis manifest.

**Architecture:** Use Clang LibTooling to parse the compilation database, then normalize paths, command arguments, repository context, and build variant fields into VERITAS-owned data structures. Emit deterministic diagnostic JSON that later milestones can store without re-parsing the compile database.

**Tech Stack:** C++20, CMake, Clang LibTooling, GoogleTest, local `veritas::Status`.

**Spec:** `docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md`

## Global Constraints

- M1 must not perform function extraction or LLVM IR generation.
- Manifest output must be deterministic for equivalent inputs.
- Source paths inside the repository must be repository-relative.
- Host-specific temporary paths must not enter semantic hashes.
- The CLI command is `veritas-build configure`.

---

### Task 1: Program Context Data Model

**Files:**
- Create: `include/veritas/build/ProgramContext.h`
- Create: `src/build/ProgramContext.cpp`
- Test: `tests/unit/build/ProgramContextTest.cpp`

**Interfaces:**
- Produces: `veritas::build::ProgramContext`
- Produces: `veritas::build::TranslationUnitCommand`
- Produces: `veritas::build::AnalysisManifest`
- Consumes: `veritas::Status`, `veritas::StatusOr`

- [ ] **Step 1: Write the failing model serialization test**

```cpp
TEST(ProgramContextTest, ManifestSerializationIsDeterministic) {
  veritas::build::AnalysisManifest manifest = MakeSmokeManifest();
  EXPECT_EQ(ToDiagnosticJson(manifest), ToDiagnosticJson(manifest));
}
```

- [ ] **Step 2: Run the focused test**

Run: `ctest --test-dir build -R ProgramContextTest --output-on-failure`

Expected: fails because `ProgramContext.h` and `ToDiagnosticJson` do not exist.

- [ ] **Step 3: Define the structs**

```cpp
namespace veritas::build {
struct ProgramContext {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::filesystem::path root_path;
  std::string vcs_kind;
  std::string vcs_revision;
  std::string source_tree_hash;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
};

struct TranslationUnitCommand {
  std::string translation_unit_id;
  std::filesystem::path source_path;
  std::filesystem::path directory;
  std::vector<std::string> arguments;
  std::string command_hash;
  std::string preprocessor_hash;
};

struct AnalysisManifest {
  ProgramContext context;
  std::vector<TranslationUnitCommand> translation_units;
};
}
```

- [ ] **Step 4: Implement deterministic diagnostic JSON**

Sort translation units by `source_path.string()` then `command_hash`. Keep field order fixed.

- [ ] **Step 5: Run the focused test again**

Run: `ctest --test-dir build -R ProgramContextTest --output-on-failure`

Expected: pass.

---

### Task 2: Compilation Database Loader

**Files:**
- Create: `include/veritas/build/CompilationDatabaseLoader.h`
- Create: `src/build/CompilationDatabaseLoader.cpp`
- Create: `tests/fixtures/cpp/smoke/compile_commands.json`
- Create: `tests/fixtures/cpp/smoke/smoke.cpp`
- Test: `tests/integration/build/CompilationDatabaseLoaderTest.cpp`

**Interfaces:**
- Consumes: `ProgramContext`
- Produces: `LoadCompilationDatabase(const std::filesystem::path&, const ProgramContext&)`

- [ ] **Step 1: Write the integration test**

```cpp
TEST(CompilationDatabaseLoaderTest, LoadsSmokeFixtureDeterministically) {
  auto manifest = veritas::build::LoadCompilationDatabase(
      "tests/fixtures/cpp/smoke/compile_commands.json",
      MakeBaseProgramContext()).value();
  ASSERT_EQ(manifest.translation_units.size(), 1);
  EXPECT_EQ(manifest.translation_units[0].source_path, "smoke.cpp");
}
```

- [ ] **Step 2: Run the integration test**

Run: `ctest --test-dir build -R CompilationDatabaseLoaderTest --output-on-failure`

Expected: fails because the loader is missing.

- [ ] **Step 3: Implement Clang LibTooling loading**

Use `clang::tooling::JSONCompilationDatabase` and return `InvalidArgument` on parse failure.

- [ ] **Step 4: Normalize source paths and arguments**

Convert paths inside the repo root to relative paths. Preserve argument order because compiler argument order is semantic.

- [ ] **Step 5: Add missing-source behavior**

Return `FailedPrecondition` when a source file is missing unless the caller passed the future `allow_missing_source` option as true.

- [ ] **Step 6: Run the integration test again**

Run: `ctest --test-dir build -R CompilationDatabaseLoaderTest --output-on-failure`

Expected: pass.

---

### Task 3: Configure CLI

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Test: `tests/integration/build/VeritasBuildConfigureCliTest.cpp`

**Interfaces:**
- Consumes: `LoadCompilationDatabase`
- Produces: `veritas-build configure --compile-db <path> --repo-root <path> --output <path>`

- [ ] **Step 1: Write the CLI test**

Run the binary against the smoke fixture and assert `.veritas/manifest.json` is created.

- [ ] **Step 2: Implement command parsing**

Support only:

```text
veritas-build configure --compile-db <path> --repo-root <path> --output <path>
```

- [ ] **Step 3: Print the required summary**

Output repository ID, revision ID, build variant ID, translation unit count, and output path.

- [ ] **Step 4: Run CLI test**

Run: `ctest --test-dir build -R VeritasBuildConfigureCliTest --output-on-failure`

Expected: pass.

---

### Task 4: Milestone Verification

**Files:**
- Modify: none

**Interfaces:**
- Consumes: all M1 targets
- Produces: verified M1 handoff artifact

- [ ] **Step 1: Run M1 tests**

Run: `ctest --test-dir build -R "ProgramContext|CompilationDatabaseLoader|VeritasBuildConfigure" --output-on-failure`

- [ ] **Step 2: Run the smoke CLI manually**

Run:

```bash
./build/src/tools/veritas-build configure \
  --compile-db tests/fixtures/cpp/smoke/compile_commands.json \
  --repo-root tests/fixtures/cpp/smoke \
  --output /tmp/veritas-smoke-manifest.json
```

- [ ] **Step 3: Commit**

```bash
git add include/veritas/build src/build src/tools/veritas-build.cpp tests/fixtures/cpp/smoke tests/unit/build tests/integration/build
git commit -m "feat: add build manifest ingestion"
```

