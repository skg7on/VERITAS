# M4 VERITAS-Owned Clang/LLVM Project Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build AST facts, LLVM IR, local analysis facts, and unpublished summary drafts for every translation unit in an M1 project manifest, entirely inside VERITAS.

**Architecture:** Run Clang frontend actions from normalized M1 commands, preserve source/identity mappings, emit per-translation-unit LLVM modules, and link them into one private move-only `ProgramIr`. Extract direct/local LLVM facts into M3 summary drafts, but keep the drafts and live `ProgramIr` together for the required M5 SVF merge before publication.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22.x libraries, Clang LibTooling and CodeGen, LLVM IR Linker, DominatorTree and MemorySSA, Protobuf Summary IR, GoogleTest.

**Spec:** `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md`

## Global Constraints

- M4 consumes the typed M1 `AnalysisManifest`; it never accepts a manifest pathname or re-parses `compile_commands.json`.
- VERITAS invokes Clang and LLVM through library APIs; no user-managed `clang`, `llvm-link`, or `opt` prerequisite is allowed.
- User-supplied `.bc` and `.ll` files are not production inputs.
- Every translation unit is processed successfully or the full project analysis fails.
- Clang AST pointers and LLVM `Value*` addresses are never persisted as VERITAS identity.
- `ProgramIr` and every header containing native LLVM types remain under `src/analysis` and are not installed.
- M4 emits direct/local facts only and does not claim that required full analysis is complete.
- Local summary drafts are not published until M5 maps and merges required SVF results.

---

### Task 1: Project-Wide Clang AST Extraction

**Files:**
- Create: `src/frontend/clang/ProjectAstExtractor.h`
- Create: `src/frontend/clang/ProjectAstExtractor.cpp`
- Create: `src/frontend/clang/SourceAnchorBuilder.h`
- Create: `src/frontend/clang/SourceAnchorBuilder.cpp`
- Create: `tests/fixtures/projects/frontend_features/compile_commands.json`
- Create: `tests/fixtures/projects/frontend_features/overloads.cpp`
- Create: `tests/fixtures/projects/frontend_features/templates.cpp`
- Create: `tests/fixtures/projects/frontend_features/internal_a.cpp`
- Create: `tests/fixtures/projects/frontend_features/internal_b.cpp`
- Create: `tests/fixtures/projects/frontend_features/macros.cpp`
- Test: `tests/integration/frontend/ProjectAstExtractorTest.cpp`

**Interfaces:**
- Consumes: `build::AnalysisManifest`, M2 stable-ID builders, and Summary IR source-anchor types
- Produces: `frontend::clang::ExtractedFunctionDecl`
- Produces: `frontend::clang::ProjectAstIndex`
- Produces: `StatusOr<ProjectAstIndex> ProjectAstExtractor::ExtractProject(const AnalysisManifest&)`

- [ ] **Step 1: Write failing project AST tests**

```cpp
TEST(ProjectAstExtractorTest, ProcessesEveryTranslationUnitAndPreservesIdentity) {
  ASSERT_OK_AND_ASSIGN(auto manifest, LoadFixtureManifest("frontend_features"));
  frontend::clang::ProjectAstExtractor extractor;
  ASSERT_OK_AND_ASSIGN(auto index, extractor.ExtractProject(manifest));
  EXPECT_EQ(index.processed_translation_units,
            manifest.translation_units.size());
  EXPECT_HAS_DISTINCT_SYMBOLS(index.declarations,
                              "overloaded(int)", "overloaded(double)");
  EXPECT_INTERNAL_LINKAGE_DOES_NOT_COLLIDE(
      index.declarations, "internal_a.cpp", "internal_b.cpp");
  EXPECT_MACRO_ANCHOR_HAS_SPELLING_AND_EXPANSION(index, "CALL_TARGET");
}

TEST(ProjectAstExtractorTest, OneTranslationUnitFailureFailsTheProject) {
  auto manifest = LoadFixtureManifest("frontend_features").value();
  manifest.translation_units.push_back(MakeInvalidCommand("broken.cpp"));
  auto result = frontend::clang::ProjectAstExtractor().ExtractProject(manifest);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::FailedPrecondition);
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R ProjectAstExtractorTest --output-on-failure`

Expected: compilation fails because the extractor and project index do not exist.

- [ ] **Step 3: Define private frontend result types**

```cpp
namespace veritas::frontend::clang {
struct ExtractedFunctionDecl {
  core::StableId function_symbol_id;
  core::StableId translation_unit_id;
  std::string qualified_name;
  std::string mangled_name;
  std::string canonical_signature;
  std::string linkage_kind;
  std::string template_identity;
  summary::SourceAnchor source_anchor;
};

struct ProjectAstIndex {
  std::vector<ExtractedFunctionDecl> declarations;
  std::size_t processed_translation_units = 0;

  const ExtractedFunctionDecl* FindByMangledName(
      std::string_view mangled_name) const;
};

class ProjectAstExtractor {
 public:
  StatusOr<ProjectAstIndex> ExtractProject(
      const build::AnalysisManifest& manifest);
};
}
```

- [ ] **Step 4: Implement one `FrontendAction` per normalized command**

Create a `clang::tooling::FixedCompilationDatabase` from each `TranslationUnitCommand`'s working directory and arguments, run a VERITAS `ASTFrontendAction`, and collect definitions/declarations through a `RecursiveASTVisitor`. Stop and return `FailedPrecondition` with the translation-unit ID and source path on any nonzero Clang result.

```cpp
struct ClangToolCommand {
  std::string working_directory;
  std::string source_path;
  std::vector<std::string> arguments;
};

StatusOr<ClangToolCommand> ToClangToolCommand(
    const std::filesystem::path& project_root,
    const build::TranslationUnitCommand& command);

class ProjectAstActionFactory final
    : public ::clang::tooling::FrontendActionFactory {
 public:
  ProjectAstActionFactory(core::StableId translation_unit_id,
                          ProjectAstIndex* output);
  std::unique_ptr<::clang::FrontendAction> create() override;
};

for (const auto& command : manifest.translation_units) {
  VERITAS_ASSIGN_OR_RETURN(auto invocation,
      ToClangToolCommand(manifest.context.project_root, command));
  clang::tooling::FixedCompilationDatabase database(
      invocation.working_directory, invocation.arguments);
  clang::tooling::ClangTool tool(database, {invocation.source_path});
  auto action = std::make_unique<ProjectAstActionFactory>(
      core::ParseStableId(command.translation_unit_id), &index);
  if (tool.run(action.get()) != 0) {
    return Status::FailedPrecondition(
        "Clang AST extraction failed for " + command.translation_unit_id);
  }
  ++index.processed_translation_units;
}
```

- [ ] **Step 5: Implement canonical declarations and source anchors**

Use canonical `FunctionDecl`, Clang mangling APIs, linkage, template specialization identity, spelling location, expansion location, and macro caller chain. Convert paths through M1 `TaggedPath`; do not use line/column as function identity.

```cpp
ExtractedFunctionDecl BuildFunctionDecl(const ::clang::FunctionDecl& decl) {
  const auto* canonical = decl.getCanonicalDecl();
  return ExtractedFunctionDecl{
      .function_symbol_id = BuildFunctionSymbolId(*canonical),
      .translation_unit_id = current_translation_unit_id_,
      .qualified_name = canonical->getQualifiedNameAsString(),
      .mangled_name = MangleName(*canonical),
      .canonical_signature = CanonicalSignature(*canonical),
      .linkage_kind = LinkageName(canonical->getFormalLinkage()),
      .template_identity = TemplateIdentity(*canonical),
      .source_anchor = source_anchor_builder_.Build(
          canonical->getSourceRange(), source_manager_),
  };
}
```

- [ ] **Step 6: Run the frontend tests and verify success**

Run: `ctest --test-dir build -R ProjectAstExtractorTest --output-on-failure`

Expected: all frontend fixtures pass and the broken command fails the whole project.

- [ ] **Step 7: Commit project AST extraction**

```bash
git add src/frontend/clang tests/fixtures/projects/frontend_features tests/integration/frontend/ProjectAstExtractorTest.cpp
git commit -m "feat: extract project AST facts in process"
```

---

### Task 2: Private Program IR Ownership and Whole-Program Linking

**Files:**
- Create: `src/analysis/pipeline/ProgramIr.h`
- Create: `src/analysis/pipeline/ProgramIr.cpp`
- Create: `src/analysis/llvm/ProjectIrBuilder.h`
- Create: `src/analysis/llvm/ProjectIrBuilder.cpp`
- Create: `src/analysis/llvm/OriginMap.h`
- Create: `src/analysis/llvm/OriginMap.cpp`
- Create: `tests/fixtures/projects/multiple_tus_flow/compile_commands.json`
- Create: `tests/fixtures/projects/multiple_tus_flow/source.cpp`
- Create: `tests/fixtures/projects/multiple_tus_flow/sink.cpp`
- Test: `tests/integration/analysis/ProjectIrBuilderTest.cpp`

**Interfaces:**
- Consumes: `build::AnalysisManifest` and `frontend::clang::ProjectAstIndex`
- Produces: private `analysis::pipeline::ProgramIr`
- Produces: `analysis::llvm::OriginMap`
- Produces: `StatusOr<ProgramIr> ProjectIrBuilder::BuildProjectIr(const AnalysisManifest&, const ProjectAstIndex&)`

- [ ] **Step 1: Write failing linked-module tests**

```cpp
TEST(ProjectIrBuilderTest, LinksEveryTranslationUnitIntoOneOwnedModule) {
  ASSERT_OK_AND_ASSIGN(auto manifest, LoadFixtureManifest("multiple_tus_flow"));
  ASSERT_OK_AND_ASSIGN(auto ast, ExtractFixtureAst(manifest));
  analysis::llvm::ProjectIrBuilder builder;
  ASSERT_OK_AND_ASSIGN(auto program_ir,
                       builder.BuildProjectIr(manifest, ast));
  EXPECT_EQ(program_ir.translation_unit_count(), 2u);
  EXPECT_NE(program_ir.module().getFunction("source"), nullptr);
  EXPECT_NE(program_ir.module().getFunction("sink"), nullptr);
  EXPECT_FALSE(program_ir.module_hash().empty());
}

TEST(ProjectIrBuilderTest, RejectsPartialLink) {
  auto manifest = LoadFixtureManifest("multiple_tus_flow").value();
  manifest.translation_units[1].arguments.push_back("-invalid-clang-flag");
  auto result = BuildFixtureProgramIr(manifest);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::FailedPrecondition);
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R ProjectIrBuilderTest --output-on-failure`

Expected: compilation fails because `ProgramIr` and `ProjectIrBuilder` are missing.

- [ ] **Step 3: Define the move-only private `ProgramIr`**

```cpp
namespace veritas::analysis::pipeline {
class ProgramIr {
 public:
  ProgramIr(std::unique_ptr<::llvm::LLVMContext> context,
            std::unique_ptr<::llvm::Module> module,
            ::veritas::analysis::llvm::OriginMap origin_map,
            std::size_t translation_unit_count,
            std::string module_hash);
  ProgramIr(ProgramIr&&) noexcept;
  ProgramIr& operator=(ProgramIr&&) noexcept;
  ProgramIr(const ProgramIr&) = delete;
  ProgramIr& operator=(const ProgramIr&) = delete;

  ::llvm::Module& module();
  const ::veritas::analysis::llvm::OriginMap& origin_map() const;
  std::size_t translation_unit_count() const;
  std::string_view module_hash() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
```

Keep native LLVM members in `ProgramIr.cpp` via a private implementation object so callers outside the private pipeline cannot accidentally retain them.

Define the private builder and origin map at the same boundary:

```cpp
namespace veritas::analysis::llvm {
class OriginMap {
 public:
  OriginMap();
  ~OriginMap();
  OriginMap(OriginMap&&) noexcept;
  OriginMap& operator=(OriginMap&&) noexcept;
  OriginMap(const OriginMap&) = delete;
  OriginMap& operator=(const OriginMap&) = delete;

  void BindFunction(const ::llvm::Function*, core::StableId function_id,
                    summary::SourceAnchor anchor);
  void BindValue(const ::llvm::Value*, summary::ValueRef value);
  void BindMemory(const ::llvm::Value*, summary::MemoryRef memory);
  StatusOr<core::StableId> FunctionVariantId(
      const ::llvm::Function&) const;
  std::optional<summary::ValueRef> FindValue(const ::llvm::Value*) const;
  std::optional<summary::MemoryRef> FindMemory(const ::llvm::Value*) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class ProjectIrBuilder {
 public:
  StatusOr<pipeline::ProgramIr> BuildProjectIr(
      const build::AnalysisManifest& manifest,
      const frontend::clang::ProjectAstIndex& ast_index);
};
}
```

- [ ] **Step 4: Generate each LLVM module with Clang CodeGen APIs**

For each normalized command, create a `clang::CompilerInvocation`, remove only output-path flags, and run `clang::EmitLLVMOnlyAction` in the VERITAS process. Move `takeModule()` into the shared project `LLVMContext`. Return `FailedPrecondition` with the translation-unit ID if diagnostics contain an error or no module is produced.

```cpp
StatusOr<std::unique_ptr<::llvm::Module>> BuildTranslationUnitModule(
    const build::TranslationUnitCommand& command,
    ::llvm::LLVMContext& context) {
  ::clang::CompilerInstance compiler;
  ConfigureDiagnosticsAndFileManager(compiler);
  auto arguments = CodeGenArguments(command.arguments);
  if (!::clang::CompilerInvocation::CreateFromArgs(
          compiler.getInvocation(), arguments, compiler.getDiagnostics())) {
    return Status::FailedPrecondition("invalid Clang invocation");
  }
  ::clang::EmitLLVMOnlyAction action(&context);
  if (!compiler.ExecuteAction(action)) {
    return Status::FailedPrecondition("LLVM IR generation failed");
  }
  auto module = action.takeModule();
  if (!module) return Status::Internal("Clang produced no LLVM module");
  return module;
}
```

- [ ] **Step 5: Link modules and compute the module hash**

Create a destination `llvm::Module`, link each translation-unit module with `llvm::Linker::linkInModule`, reject target-triple or data-layout mismatches, sort named metadata whose order is nonsemantic, and hash canonical bitcode written to an in-memory buffer with `llvm::SHA256`.

```cpp
auto linked = std::make_unique<::llvm::Module>("veritas.project", context);
::llvm::Linker linker(*linked);
for (auto& module : modules) {
  VERITAS_RETURN_IF_ERROR(VerifyCompatibleLayout(*linked, *module));
  if (linker.linkInModule(std::move(module))) {
    return Status::FailedPrecondition("LLVM module link failed");
  }
}
std::string bytes;
::llvm::raw_string_ostream stream(bytes);
::llvm::WriteBitcodeToFile(*linked, stream, /*ShouldPreserveUseListOrder=*/true);
const std::string module_hash = Sha256Hex(bytes);
```

- [ ] **Step 6: Build the LLVM-to-VERITAS origin map**

Map functions by mangled name and translation-unit identity. Map arguments, returns, instructions, allocations, globals, and field paths to `ValueRef`/`MemoryRef` using function variant IDs plus structural position; retain Clang spelling/expansion anchors separately.

```cpp
for (auto& function : *linked) {
  auto declaration = ast_index.FindByMangledName(function.getName());
  if (!declaration) continue;
  origin_map.BindFunction(&function,
                          BuildFunctionVariantId(*declaration,
                                                 manifest.context),
                          declaration->source_anchor);
  for (auto& argument : function.args()) {
    origin_map.BindValue(&argument,
        BuildArgumentValueRef(declaration->function_symbol_id,
                              argument.getArgNo()));
  }
  BindInstructionAndMemoryOrigins(function, declaration->source_anchor,
                                  &origin_map);
}
```

- [ ] **Step 7: Run the IR tests and verify success**

Run: `ctest --test-dir build -R ProjectIrBuilderTest --output-on-failure`

Expected: the two translation units link into one deterministic module and partial generation fails.

- [ ] **Step 8: Commit private project IR construction**

```bash
git add src/analysis/pipeline/ProgramIr.* src/analysis/llvm tests/fixtures/projects/multiple_tus_flow tests/integration/analysis/ProjectIrBuilderTest.cpp
git commit -m "feat: build linked project IR in process"
```

---

### Task 3: Local LLVM Fact Extraction

**Files:**
- Create: `src/analysis/llvm/LocalFactExtractor.h`
- Create: `src/analysis/llvm/LocalFactExtractor.cpp`
- Create: `tests/fixtures/projects/local_facts/compile_commands.json`
- Create: `tests/fixtures/projects/local_facts/local_facts.cpp`
- Test: `tests/integration/analysis/LocalFactExtractorTest.cpp`

**Interfaces:**
- Consumes: live `pipeline::ProgramIr`, M2 function IDs, and M3 Summary IR fact types
- Produces: `analysis::llvm::FunctionLocalFacts`
- Produces: `StatusOr<std::vector<FunctionLocalFacts>> LocalFactExtractor::Extract(ProgramIr&)`

- [ ] **Step 1: Write failing local-fact tests**

```cpp
TEST(LocalFactExtractorTest, EmitsCallsMemoryAndLocalFlows) {
  ASSERT_OK_AND_ASSIGN(auto program_ir, BuildFixtureProgramIr("local_facts"));
  analysis::llvm::LocalFactExtractor extractor;
  ASSERT_OK_AND_ASSIGN(auto facts, extractor.Extract(program_ir));
  EXPECT_HAS_CALL(facts, "memcpy", summary::EpistemicState::kMust);
  EXPECT_HAS_PARAMETER_RETURN_FLOW(facts, "identity", 0);
  EXPECT_HAS_LOCAL_STORE_AND_LOAD(facts, "copy_value");
}

TEST(LocalFactExtractorTest, RepresentsUnresolvedIndirectCallAsUnknown) {
  ASSERT_OK_AND_ASSIGN(auto facts, ExtractFixtureLocalFacts("local_facts"));
  EXPECT_HAS_UNKNOWN(facts, summary::UnknownKind::kUnresolvedCall);
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R LocalFactExtractorTest --output-on-failure`

Expected: compilation fails because `LocalFactExtractor` is missing.

- [ ] **Step 3: Define per-function local facts**

```cpp
namespace veritas::analysis::llvm {
struct FunctionLocalFacts {
  core::StableId function_variant_id;
  std::vector<summary::CallFact> calls;
  std::vector<summary::MemoryEffectFact> memory_effects;
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::RangeFact> range_facts;
  std::vector<summary::UnknownFact> unknowns;
};

class LocalFactExtractor {
 public:
  StatusOr<std::vector<FunctionLocalFacts>> Extract(
      pipeline::ProgramIr& program_ir);
};
}
```

- [ ] **Step 4: Implement direct call, CFG, and dominator extraction**

Traverse each defined function. Emit `MUST_CALL` for direct calls, bounded local candidates when LLVM proves them, and `UNKNOWN_CALL` otherwise. Use `llvm::DominatorTree` for summary-level dominance facts; do not persist basic instruction nodes globally.

```cpp
for (auto& function : program_ir.module()) {
  if (function.isDeclaration()) continue;
  ::llvm::DominatorTree dominators(function);
  VERITAS_ASSIGN_OR_RETURN(
      auto function_variant_id,
      program_ir.origin_map().FunctionVariantId(function));
  FunctionLocalFacts facts{.function_variant_id = function_variant_id};
  for (auto& block : function) {
    for (auto& instruction : block) {
      if (auto* call = ::llvm::dyn_cast<::llvm::CallBase>(&instruction)) {
        facts.calls.push_back(MapLocalCall(*call, dominators,
                                           program_ir.origin_map()));
      }
    }
  }
  output.push_back(std::move(facts));
}
```

- [ ] **Step 5: Implement memory and value-flow extraction**

Handle loads, stores, `memcpy`/`memmove`/`memset`, parameters, returns, PHI, select, GEP field paths, and MemorySSA def/use links. Resolve every result through `ProgramIr::origin_map()` and emit a scoped unknown when a native value cannot be mapped.

```cpp
::llvm::AAResults alias_results = BuildAliasResults(function);
::llvm::DominatorTree dominators(function);
::llvm::MemorySSA memory_ssa(function, &alias_results, &dominators);
for (auto& instruction : ::llvm::instructions(function)) {
  if (auto* load = ::llvm::dyn_cast<::llvm::LoadInst>(&instruction)) {
    MapLoad(*load, memory_ssa, origin_map, &facts);
  } else if (auto* store = ::llvm::dyn_cast<::llvm::StoreInst>(&instruction)) {
    MapStore(*store, memory_ssa, origin_map, &facts);
  } else if (auto* phi = ::llvm::dyn_cast<::llvm::PHINode>(&instruction)) {
    MapPhi(*phi, origin_map, &facts);
  } else if (auto* select = ::llvm::dyn_cast<::llvm::SelectInst>(&instruction)) {
    MapSelect(*select, origin_map, &facts);
  }
}
```

- [ ] **Step 6: Run local-fact tests and verify success**

Run: `ctest --test-dir build -R LocalFactExtractorTest --output-on-failure`

Expected: direct facts pass and unresolved calls remain explicit unknowns.

- [ ] **Step 7: Commit local LLVM extraction**

```bash
git add src/analysis/llvm/LocalFactExtractor.* tests/fixtures/projects/local_facts tests/integration/analysis/LocalFactExtractorTest.cpp
git commit -m "feat: extract local LLVM facts"
```

---

### Task 4: Local Summary Drafts and M5 Handoff

**Files:**
- Create: `src/analysis/pipeline/LocalAnalysisStage.h`
- Create: `src/analysis/pipeline/LocalAnalysisStage.cpp`
- Modify: `include/veritas/summary/LocalSummaryBuilder.h`
- Modify: `src/summary/LocalSummaryBuilder.cpp`
- Create: `tests/support/SourceTreeInspection.h`
- Create: `tests/support/SourceTreeInspection.cpp`
- Test: `tests/integration/analysis/LocalAnalysisStageTest.cpp`
- Test: `tests/integration/build/VeritasBuildAnalyzeOwnershipTest.cpp`

**Interfaces:**
- Consumes: M1 `AnalysisManifest`, `ProjectAstExtractor`, `ProjectIrBuilder`, `LocalFactExtractor`, and M3 Summary IR
- Produces: `pipeline::LocalAnalysisResult`
- Produces: `StatusOr<LocalAnalysisResult> RunLocalAnalysis(const AnalysisManifest&)`
- Produces: move-only `ProgramIr` plus unpublished `FunctionSummary` drafts for M5

- [ ] **Step 1: Write failing stage and ownership tests**

```cpp
TEST(LocalAnalysisStageTest, ReturnsLiveProgramIrAndUnpublishedDrafts) {
  ASSERT_OK_AND_ASSIGN(auto manifest, LoadFixtureManifest("multiple_tus_flow"));
  ASSERT_OK_AND_ASSIGN(auto result, pipeline::RunLocalAnalysis(manifest));
  EXPECT_EQ(result.program_ir.translation_unit_count(), 2u);
  EXPECT_FALSE(result.summary_drafts.empty());
  EXPECT_FALSE(ReadSourceFile("src/analysis/pipeline/LocalAnalysisStage.cpp")
                   .contains("PublishSummary"));
}

TEST(VeritasBuildAnalyzeOwnershipTest, ProductionSourceHasNoArtifactInputMode) {
  EXPECT_FALSE(ProductionCommandParserAccepts("--manifest"));
  EXPECT_FALSE(ProductionCommandParserAccepts("--bitcode"));
  EXPECT_FALSE(ProductionCommandParserAccepts("--llvm-module"));
}
```

`ReadSourceFile` and `ProductionCommandParserAccepts` live in
`tests/support/SourceTreeInspection.*`. The latter invokes only the production
argument parser and returns true exactly when parsing the supplied flag reaches
the project request; it does not scan the test source text.

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R "LocalAnalysisStage|VeritasBuildAnalyzeOwnership" --output-on-failure`

Expected: compilation fails because the local stage and result type are missing.

- [ ] **Step 3: Define the M4-to-M5 result**

```cpp
namespace veritas::analysis::pipeline {
struct LocalAnalysisResult {
  ProgramIr program_ir;
  std::vector<summary::v1::FunctionSummary> summary_drafts;
};

StatusOr<LocalAnalysisResult> RunLocalAnalysis(
    const build::AnalysisManifest& manifest);
}
```

- [ ] **Step 4: Implement the ordered local stages**

Call `ProjectAstExtractor::ExtractProject`, `ProjectIrBuilder::BuildProjectIr`, `LocalFactExtractor::Extract`, then `BuildLocalSummary`. Return the live `ProgramIr` and drafts together. Do not call `SummaryRepository::PublishSummary`; M5 owns merge and publication.

```cpp
StatusOr<LocalAnalysisResult> RunLocalAnalysis(
    const build::AnalysisManifest& manifest) {
  frontend::clang::ProjectAstExtractor ast_extractor;
  VERITAS_ASSIGN_OR_RETURN(auto ast, ast_extractor.ExtractProject(manifest));
  analysis::llvm::ProjectIrBuilder ir_builder;
  VERITAS_ASSIGN_OR_RETURN(auto program_ir,
      ir_builder.BuildProjectIr(manifest, ast));
  analysis::llvm::LocalFactExtractor fact_extractor;
  VERITAS_ASSIGN_OR_RETURN(auto local_facts,
      fact_extractor.Extract(program_ir));
  VERITAS_ASSIGN_OR_RETURN(auto drafts,
      summary::BuildLocalSummaryDrafts(ast, local_facts, manifest.context));
  return LocalAnalysisResult{std::move(program_ir), std::move(drafts)};
}
```

- [ ] **Step 5: Run all M4 tests**

Run: `ctest --test-dir build -R "ProjectAstExtractor|ProjectIrBuilder|LocalFactExtractor|LocalAnalysisStage|VeritasBuildAnalyzeOwnership" --output-on-failure`

Expected: all M4 tests pass.

- [ ] **Step 6: Commit the M5 handoff**

```bash
git add src/analysis/pipeline/LocalAnalysisStage.* include/veritas/summary/LocalSummaryBuilder.h src/summary/LocalSummaryBuilder.cpp tests/support/SourceTreeInspection.* tests/integration/analysis/LocalAnalysisStageTest.cpp tests/integration/build/VeritasBuildAnalyzeOwnershipTest.cpp
git commit -m "feat: prepare local analysis for required SVF"
```

---

## Milestone Verification

- [ ] Build the M4 targets:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_DIR="${LLVM_DIR}"
cmake --build build --target veritas-build
```

- [ ] Run all M4 tests:

```bash
ctest --test-dir build -R "ProjectAstExtractor|ProjectIrBuilder|LocalFactExtractor|LocalAnalysisStage|VeritasBuildAnalyzeOwnership" --output-on-failure
```

- [ ] Verify native headers are private and artifact flags are absent:

```bash
if rg -n '#include <(llvm|clang)/' include/veritas; then
  exit 1
fi
if rg -n '"--(manifest|bitcode|llvm-module)"' src include; then
  exit 1
fi
```

- [ ] Verify no standalone compiler-analysis prerequisite appears in tests:

```bash
if rg -n '(^|[[:space:]])(clang\+\+|clang|llvm-link|opt)([[:space:]]|$)' tests --glob '*.cpp' --glob '*.cmake' --glob '*.sh'; then
  exit 1
fi
```

- [ ] Verify the branch diff is clean:

```bash
git diff --check
git status --short
```
