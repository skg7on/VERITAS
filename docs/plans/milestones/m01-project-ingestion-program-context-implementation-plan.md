# M1 Project Ingestion and Program Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Accept one C/C++ project directory containing `compile_commands.json` and construct the deterministic typed program context used by every later VERITAS analysis stage.

**Architecture:** Resolve and validate a project-level request, load `<project>/compile_commands.json` through Clang LibTooling, and normalize commands into a VERITAS-owned in-memory `AnalysisManifest`. `veritas-build analyze --project <directory>` invokes this library stage directly; serialized manifests are diagnostics and cache material, never public inputs to later commands.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22.x, Clang LibTooling, LLVM SHA-256 and JSON support, GoogleTest, project-local `veritas::Status` and `StatusOr`.

**Spec:** `docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md`

## Global Constraints

- The project directory is the sole public source-input abstraction.
- `<project_root>/compile_commands.json` is mandatory and is the only compilation-database location resolved by M1.
- The production CLI exposes `veritas-build analyze --project <directory>` and no `--compile-db`, `--manifest`, `--bitcode`, `--llvm-module`, or `--svf-input` alternative.
- The typed `AnalysisManifest` is the authoritative downstream handoff; diagnostic JSON is never re-ingested.
- Missing translation units fail project ingestion instead of producing a partial manifest.
- Paths inside the repository are tagged and repository-relative; checkout, output, and temporary paths do not enter semantic hashes.
- M1 does not traverse ASTs, generate LLVM IR, or invoke SVF, but the standard command will continue into those VERITAS-owned stages as later milestones land.

---

### Task 1: Project-Level Request and Input Resolution

**Files:**
- Create: `include/veritas/analysis/ProjectAnalysisRequest.h`
- Create: `include/veritas/build/ProjectInput.h`
- Create: `src/build/ProjectInput.cpp`
- Create: `tests/fixtures/projects/smoke/compile_commands.json`
- Create: `tests/fixtures/projects/smoke/smoke.cpp`
- Create: `tests/fixtures/projects/missing_compile_database/README.md`
- Create: `tests/support/ProjectFixture.h`
- Create: `tests/support/ProjectFixture.cpp`
- Test: `tests/unit/build/ProjectInputTest.cpp`

**Interfaces:**
- Consumes: `veritas::Status`, `veritas::StatusOr`
- Produces: `veritas::analysis::ProjectAnalysisRequest`
- Produces: `veritas::build::ProjectInput`
- Produces: `StatusOr<ProjectInput> ResolveProjectInput(const ProjectAnalysisRequest&)`

- [ ] **Step 1: Add the smoke project fixture**

Create `tests/fixtures/projects/smoke/smoke.cpp`:

```cpp
int identity(int value) { return value; }
```

Create `tests/fixtures/projects/smoke/compile_commands.json` with one entry whose `directory` and `file` are fixture-relative after the test substitutes the absolute fixture root:

```json
[
  {
    "directory": "@PROJECT_ROOT@",
    "arguments": ["clang++", "-std=c++20", "-c", "smoke.cpp"],
    "file": "smoke.cpp"
  }
]
```

Implement `FixtureProject` by copying the named fixture to a test-owned temporary directory and replacing `@PROJECT_ROOT@` in `compile_commands.json` with the canonical temporary path:

```cpp
std::filesystem::path FixtureProject(std::string_view name) {
  const auto source = TestSourceRoot() / "tests/fixtures/projects" / name;
  const auto destination = MakeUniqueTestDirectory(name);
  std::filesystem::copy(source, destination,
      std::filesystem::copy_options::recursive);
  const auto database = destination / "compile_commands.json";
  if (std::filesystem::exists(database)) {
    ReplaceAllInFile(database, "@PROJECT_ROOT@",
                     std::filesystem::canonical(destination).string());
  }
  return destination;
}
```

- [ ] **Step 2: Write failing request-resolution tests**

```cpp
TEST(ProjectInputTest, ResolvesCompileDatabaseInsideProjectRoot) {
  analysis::ProjectAnalysisRequest request{
      .project_root = FixtureProject("smoke"),
      .output_root = {},
  };
  ASSERT_OK_AND_ASSIGN(auto input, build::ResolveProjectInput(request));
  EXPECT_EQ(input.compile_database_path,
            input.project_root / "compile_commands.json");
  EXPECT_EQ(input.output_root, input.project_root / ".veritas");
}

TEST(ProjectInputTest, RejectsProjectWithoutCompileDatabase) {
  analysis::ProjectAnalysisRequest request{
      .project_root = FixtureProject("missing_compile_database"),
      .output_root = {},
  };
  auto result = build::ResolveProjectInput(request);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::FailedPrecondition);
}
```

- [ ] **Step 3: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R ProjectInputTest --output-on-failure`

Expected: compilation fails because `ProjectAnalysisRequest.h`, `ProjectInput.h`, and `ResolveProjectInput` do not exist.

- [ ] **Step 4: Define the request and resolved input types**

```cpp
namespace veritas::analysis {
struct ProjectAnalysisRequest {
  std::filesystem::path project_root;
  std::filesystem::path output_root;
};
}

namespace veritas::build {
struct ProjectInput {
  std::filesystem::path project_root;
  std::filesystem::path compile_database_path;
  std::filesystem::path output_root;
};

StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request);
}
```

- [ ] **Step 5: Implement strict project resolution**

In `ProjectInput.cpp`, canonicalize `project_root`, require it to be a directory, resolve only `project_root / "compile_commands.json"`, require a regular non-empty file, and default an empty `output_root` to `project_root / ".veritas"`. Return `InvalidArgument` for a missing/non-directory project and `FailedPrecondition` for a missing compilation database.

```cpp
StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request) {
  std::error_code error;
  auto root = std::filesystem::weakly_canonical(request.project_root, error);
  if (error || !std::filesystem::is_directory(root)) {
    return Status::InvalidArgument("project root is not a directory");
  }
  auto database = root / "compile_commands.json";
  if (!std::filesystem::is_regular_file(database) ||
      std::filesystem::file_size(database, error) == 0) {
    return Status::FailedPrecondition(
        "project root must contain non-empty compile_commands.json");
  }
  auto output = request.output_root.empty()
      ? root / ".veritas"
      : std::filesystem::absolute(request.output_root);
  return ProjectInput{root, database, output};
}
```

- [ ] **Step 6: Run the focused tests and verify success**

Run: `ctest --test-dir build -R ProjectInputTest --output-on-failure`

Expected: all `ProjectInputTest` cases pass.

- [ ] **Step 7: Commit the project input contract**

```bash
git add include/veritas/analysis/ProjectAnalysisRequest.h include/veritas/build/ProjectInput.h src/build/ProjectInput.cpp tests/fixtures/projects/smoke tests/fixtures/projects/missing_compile_database tests/support/ProjectFixture.* tests/unit/build/ProjectInputTest.cpp
git commit -m "feat: add project directory input contract"
```

---

### Task 2: Tagged Paths and Deterministic Manifest Model

**Files:**
- Create: `include/veritas/build/AnalysisManifest.h`
- Create: `src/build/AnalysisManifest.cpp`
- Test: `tests/unit/build/AnalysisManifestTest.cpp`

**Interfaces:**
- Consumes: M1 `ProjectInput`
- Produces: `PathRootKind`, `TaggedPath`, `ProgramContext`, `TranslationUnitCommand`, and `AnalysisManifest`
- Produces: `std::string ToDiagnosticJson(const AnalysisManifest&)`
- Produces: `std::string ToCanonicalBytes(const AnalysisManifest&)`

- [ ] **Step 1: Write failing deterministic-serialization tests**

```cpp
TEST(AnalysisManifestTest, TranslationUnitOrderDoesNotChangeBytes) {
  auto first = MakeManifest({MakeTu("b.cpp", "hash-b"),
                             MakeTu("a.cpp", "hash-a")});
  auto second = MakeManifest({MakeTu("a.cpp", "hash-a"),
                              MakeTu("b.cpp", "hash-b")});
  EXPECT_EQ(build::ToCanonicalBytes(first), build::ToCanonicalBytes(second));
  EXPECT_EQ(build::ToDiagnosticJson(first), build::ToDiagnosticJson(second));
}

TEST(AnalysisManifestTest, OutputRootIsNotSerializedAsSemanticInput) {
  auto first = MakeManifestWithOutputRoot("/tmp/output-a");
  auto second = MakeManifestWithOutputRoot("/tmp/output-b");
  EXPECT_EQ(build::ToCanonicalBytes(first), build::ToCanonicalBytes(second));
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R AnalysisManifestTest --output-on-failure`

Expected: compilation fails because the manifest model and serializers are missing.

- [ ] **Step 3: Define the exact manifest types**

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
}
```

- [ ] **Step 4: Implement canonical ordering and serialization**

Copy the translation-unit vector, sort by `source_path.root_kind`, `source_path.root_id`, `source_path.relative_path.generic_string()`, then `command_hash`, and serialize fixed field names in fixed order. Omit `project_root` and output locations from canonical semantic bytes; include a display-only normalized project root in diagnostic JSON.

```cpp
auto OrderedTranslationUnits(const AnalysisManifest& manifest) {
  auto units = manifest.translation_units;
  std::ranges::sort(units, {}, [](const TranslationUnitCommand& unit) {
    return std::tuple{unit.source_path.root_kind,
                      unit.source_path.root_id,
                      unit.source_path.relative_path.generic_string(),
                      unit.command_hash};
  });
  return units;
}

std::string ToCanonicalBytes(const AnalysisManifest& manifest) {
  // AppendField writes: uint32 key_size, key bytes, uint64 value_size,
  // value bytes, all integers in network byte order.
  std::string bytes;
  AppendField(bytes, "repository_id", manifest.context.repository_id);
  AppendField(bytes, "revision_id", manifest.context.revision_id);
  AppendField(bytes, "build_variant_id",
              manifest.context.build_variant_id);
  AppendField(bytes, "source_tree_hash",
              manifest.context.source_tree_hash);
  AppendField(bytes, "compilation_database_hash",
              manifest.context.compilation_database_hash);
  AppendField(bytes, "target_triple", manifest.context.target_triple);
  AppendField(bytes, "compiler_id", manifest.context.compiler_id);
  AppendField(bytes, "compiler_version",
              manifest.context.compiler_version);
  AppendField(bytes, "compile_options_hash",
              manifest.context.compile_options_hash);
  AppendField(bytes, "macro_set_hash", manifest.context.macro_set_hash);
  AppendField(bytes, "include_closure_hash",
              manifest.context.include_closure_hash);
  AppendField(bytes, "type_layout_hash",
              manifest.context.type_layout_hash);
  for (const auto& unit : OrderedTranslationUnits(manifest)) {
    AppendField(bytes, "translation_unit", ToCanonicalBytes(unit));
  }
  return bytes;
}

std::string ToCanonicalBytes(const TranslationUnitCommand& unit) {
  std::string bytes;
  AppendField(bytes, "translation_unit_id", unit.translation_unit_id);
  AppendField(bytes, "revision_id", unit.revision_id);
  AppendField(bytes, "build_variant_id", unit.build_variant_id);
  AppendField(bytes, "source_root_kind",
              std::to_string(std::to_underlying(unit.source_path.root_kind)));
  AppendField(bytes, "source_root_id", unit.source_path.root_id);
  AppendField(bytes, "source_path",
              unit.source_path.relative_path.generic_string());
  AppendField(bytes, "working_directory_root_kind",
              std::to_string(std::to_underlying(
                  unit.working_directory.root_kind)));
  AppendField(bytes, "working_directory_root_id",
              unit.working_directory.root_id);
  AppendField(bytes, "working_directory",
              unit.working_directory.relative_path.generic_string());
  for (const auto& argument : unit.arguments) {
    AppendField(bytes, "argument", argument);
  }
  AppendField(bytes, "command_hash", unit.command_hash);
  AppendField(bytes, "preprocessor_hash", unit.preprocessor_hash);
  return bytes;
}
```

Declare the translation-unit overload before the manifest overload. Implement
`AppendField` as the private length-prefixed encoder described in the comment;
do not use delimiter-separated strings or unordered JSON objects as hash input.

- [ ] **Step 5: Run the focused tests and verify success**

Run: `ctest --test-dir build -R AnalysisManifestTest --output-on-failure`

Expected: deterministic-order and output-root-exclusion cases pass.

- [ ] **Step 6: Commit the manifest model**

```bash
git add include/veritas/build/AnalysisManifest.h src/build/AnalysisManifest.cpp tests/unit/build/AnalysisManifestTest.cpp
git commit -m "feat: add deterministic analysis manifest"
```

---

### Task 3: Compilation Database Loading and Normalization

**Files:**
- Create: `include/veritas/build/ProjectManifestLoader.h`
- Create: `src/build/ProjectManifestLoader.cpp`
- Create: `tests/fixtures/projects/multiple_tus/compile_commands.json`
- Create: `tests/fixtures/projects/multiple_tus/a.cpp`
- Create: `tests/fixtures/projects/multiple_tus/b.cpp`
- Create: `tests/fixtures/projects/missing_source/compile_commands.json`
- Create: `tests/fixtures/projects/missing_source/README.md`
- Test: `tests/integration/build/ProjectManifestLoaderTest.cpp`

**Interfaces:**
- Consumes: `ProjectInput`, `AnalysisManifest`, and Clang `JSONCompilationDatabase`
- Produces: `StatusOr<AnalysisManifest> LoadProjectManifest(const ProjectInput&)`
- Produces: repository-, generated-, external-, and toolchain-tagged paths

- [ ] **Step 1: Write failing loader integration tests**

```cpp
TEST(ProjectManifestLoaderTest, LoadsEveryTranslationUnitDeterministically) {
  ASSERT_OK_AND_ASSIGN(auto input, ResolveFixtureProject("multiple_tus"));
  ASSERT_OK_AND_ASSIGN(auto first, build::LoadProjectManifest(input));
  ASSERT_OK_AND_ASSIGN(auto second, build::LoadProjectManifest(input));
  ASSERT_EQ(first.translation_units.size(), 2u);
  EXPECT_EQ(build::ToCanonicalBytes(first), build::ToCanonicalBytes(second));
  EXPECT_EQ(first.translation_units[0].source_path.relative_path, "a.cpp");
  EXPECT_EQ(first.translation_units[1].source_path.relative_path, "b.cpp");
}

TEST(ProjectManifestLoaderTest, MissingTranslationUnitFailsWholeLoad) {
  ASSERT_OK_AND_ASSIGN(auto input, ResolveFixtureProject("missing_source"));
  auto result = build::LoadProjectManifest(input);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::FailedPrecondition);
}
```

- [ ] **Step 2: Run the integration tests and verify failure**

Run: `ctest --test-dir build -R ProjectManifestLoaderTest --output-on-failure`

Expected: compilation fails because `LoadProjectManifest` is missing.

- [ ] **Step 3: Load the fixed compilation database path with LibTooling**

Use:

```cpp
std::string error;
auto database = clang::tooling::JSONCompilationDatabase::loadFromFile(
    input.compile_database_path.string(), error,
    clang::tooling::JSONCommandLineSyntax::AutoDetect);
if (!database) {
  return Status::InvalidArgument("invalid compile_commands.json: " + error);
}
```

Call `getAllCompileCommands()`, reject an empty result, and never accept a caller-provided compilation-database path.

- [ ] **Step 4: Normalize paths and arguments**

Resolve each command's source path against its command directory, require the source to exist, classify repository paths with `PathRootKind::kRepository`, and preserve argument order. Replace checkout-specific compiler and include roots with tagged root IDs before hashing, while retaining executable arguments needed by M4.

```cpp
StatusOr<TaggedPath> NormalizeSourcePath(
    const ProjectInput& input,
    const clang::tooling::CompileCommand& command) {
  auto source = std::filesystem::weakly_canonical(
      std::filesystem::path(command.Directory) / command.Filename);
  if (!std::filesystem::is_regular_file(source)) {
    return Status::FailedPrecondition("translation-unit source is missing: " +
                                      source.string());
  }
  if (IsWithin(source, input.project_root)) {
    return TaggedPath{PathRootKind::kRepository, "repository",
                      std::filesystem::relative(source, input.project_root)};
  }
  return ClassifyExternalOrGeneratedPath(source, input);
}
```

- [ ] **Step 5: Compute deterministic context and command hashes**

Use `llvm::SHA256` over domain-separated canonical bytes:

```text
veritas.repository.v1 || canonical repository identity
veritas.revision.v1 || VCS tree or source manifest
veritas.build_variant.v1 || target/compiler/ordered semantic options
veritas.translation_unit.v1 || revision/build/source/command
```

Store lower-case hexadecimal digests. Hash the exact compilation-database bytes separately as `compilation_database_hash`.

```cpp
std::string DomainHash(std::string_view domain, std::string_view bytes) {
  llvm::SHA256 hash;
  hash.update(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t*>(domain.data()), domain.size()));
  hash.update(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  return llvm::toHex(hash.final(), /*LowerCase=*/true);
}
```

- [ ] **Step 6: Run loader and manifest tests**

Run: `ctest --test-dir build -R "ProjectManifestLoader|AnalysisManifest" --output-on-failure`

Expected: all tests pass, including reordered-entry determinism and missing-source failure.

- [ ] **Step 7: Commit project manifest loading**

```bash
git add include/veritas/build/ProjectManifestLoader.h src/build/ProjectManifestLoader.cpp tests/fixtures/projects tests/integration/build/ProjectManifestLoaderTest.cpp
git commit -m "feat: load project compilation database"
```

---

### Task 4: Standard Analyze Command Input Contract

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Test: `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`

**Interfaces:**
- Consumes: `ProjectAnalysisRequest`, `ResolveProjectInput`, and `LoadProjectManifest`
- Produces: `veritas-build analyze --project <directory> [--output <directory>]`
- Produces: optional `<output-root>/manifest.json` diagnostic output without any manifest-input mode

- [ ] **Step 1: Write failing CLI contract tests**

```cpp
TEST(VeritasBuildAnalyzeCliTest, AcceptsOnlyProjectLevelSourceInput) {
  auto ok = RunVeritasBuild({"analyze", "--project", FixtureProject("smoke")});
  EXPECT_EQ(ok.exit_code, 0);
  EXPECT_THAT(ok.stdout_text, HasSubstr("Translation Units: 1"));

  auto rejected = RunVeritasBuild(
      {"analyze", "--compile-db", "compile_commands.json"});
  EXPECT_NE(rejected.exit_code, 0);
  EXPECT_THAT(rejected.stderr_text, HasSubstr("--project is required"));
}

TEST(VeritasBuildAnalyzeCliTest, RejectsArtifactInputFlags) {
  for (std::string_view flag : {"--manifest", "--bitcode", "--llvm-module",
                                "--svf-input"}) {
    auto result = RunVeritasBuild({"analyze", std::string(flag), "input"});
    EXPECT_NE(result.exit_code, 0) << flag;
  }
}
```

- [ ] **Step 2: Run the CLI tests and verify failure**

Run: `ctest --test-dir build -R VeritasBuildAnalyzeCliTest --output-on-failure`

Expected: tests fail because `analyze --project` is not implemented.

- [ ] **Step 3: Implement exact command parsing**

Accept `analyze`, one required `--project` value, and one optional `--output` value. Reject unknown flags and duplicate source-input flags. Construct `ProjectAnalysisRequest`, resolve it, load the typed manifest, and print repository, revision, build variant, translation-unit count, and the diagnostic output path.

```cpp
if (args.empty() || args.front() != "analyze") {
  return PrintUsageAndExit();
}
auto parsed = ParseAnalyzeArguments(args.subspan(1));
if (!parsed.ok()) return PrintStatusAndExit(parsed.status());
analysis::ProjectAnalysisRequest request{
    .project_root = parsed->project,
    .output_root = parsed->output,
};
VERITAS_ASSIGN_OR_RETURN(auto input, build::ResolveProjectInput(request));
VERITAS_ASSIGN_OR_RETURN(auto manifest, build::LoadProjectManifest(input));
```

- [ ] **Step 4: Keep serialization one-way**

Write diagnostic JSON after a successful load when output is enabled, but add no code path that reads that file. On any M1 failure, return nonzero without creating a manifest.

```cpp
std::filesystem::create_directories(input.output_root);
VERITAS_RETURN_IF_ERROR(WriteFileAtomically(
    input.output_root / "manifest.json",
    build::ToDiagnosticJson(manifest)));
```

- [ ] **Step 5: Run the M1 test suite**

Run: `ctest --test-dir build -R "ProjectInput|AnalysisManifest|ProjectManifestLoader|VeritasBuildAnalyze" --output-on-failure`

Expected: all M1 tests pass.

- [ ] **Step 6: Run the smoke project through the standard command**

Run:

```bash
./build/src/tools/veritas-build analyze \
  --project tests/fixtures/projects/smoke \
  --output /tmp/veritas-m1-smoke
```

Expected: exit 0, one translation unit reported, and `/tmp/veritas-m1-smoke/manifest.json` written as a diagnostic artifact. Without `--output`, the same diagnostic is written to `<project>/.veritas/manifest.json`.

- [ ] **Step 7: Commit the M1 CLI stage**

```bash
git add src/tools/veritas-build.cpp tests/integration/build/VeritasBuildAnalyzeCliTest.cpp
git commit -m "feat: accept project directory for analysis"
```

---

## Milestone Verification

- [ ] Configure and build with the required toolchain:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build --target veritas-build
```

- [ ] Run all M1 tests:

```bash
ctest --test-dir build -R "ProjectInput|AnalysisManifest|ProjectManifestLoader|VeritasBuildAnalyze" --output-on-failure
```

- [ ] Verify forbidden public artifact flags are absent from production command handling:

```bash
if rg -n '"--(compile-db|manifest|bitcode|llvm-module|svf-input)"' src include; then
  exit 1
fi
```

- [ ] Verify the branch diff is clean:

```bash
git diff --check
git status --short
```
