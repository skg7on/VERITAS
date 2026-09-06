# M11 Unified IR Acquisition, Reporting, and Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `veritas-build analyze` with persisted and parallel project
CodeGen, detailed run reports, separated or linked LLVM IR input, and a
reproducible large-project performance evaluation.

**Architecture:** A thin CLI constructs an input-neutral `AnalysisRequest`.
`ProjectAnalyzer::Analyze` resolves one `ProgramIrSource`, acquires canonical
input-unit and linked bitcode through an atomic artifact store, and then runs
the existing local/SVF/summary/CPG/WPA/FactStore pipeline unchanged. Project
CodeGen uses isolated LLVM contexts in a bounded worker pool; all output is
ordered and linked deterministically.

**Tech Stack:** C++20 without exceptions, Clang LibTooling and dependency
scanning, LLVM IRReader/BitcodeWriter/Linker, `std::thread`, LLVM JSON,
filesystem atomic rename, CMake/Ninja, GoogleTest, Python 3 benchmark tooling.

**Spec:** `docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md`

## Global Constraints

- Exactly one of `--project` and `--bitcode` is required.
- Project output defaults to `<project>/.veritas`; external IR requires
  `--output`, and every resolved output path is absolute and normalized.
- `--jobs` defaults to `auto`, where
  `max(1, min(input_unit_count, hardware_concurrency, 8))` is used; zero
  hardware concurrency means one.
- `--jobs` controls acquisition only. The process-wide SVF mutex remains.
- Every CodeGen or IR-loading worker owns its Clang state and
  `llvm::LLVMContext`; live `llvm::Module` objects never cross worker threads.
- Per-input-unit and linked bitcode are both persisted below `<output>/llvm`.
- A dependency-complete cache key includes normalized command, source content,
  transitive include content, target/toolchain identity, CodeGen settings, and
  schema versions. Timestamps never validate a hit.
- Input-unit object publication, cache records, snapshot selection, and latest
  run selection are atomic.
- Worker completion order cannot change linked bitcode, semantic IDs, facts,
  witnesses, unknowns, or primary-error selection.
- T0 debug IR and T1 symbol-only IR are accepted; any T2 stripped module
  rejects the complete acquisition.
- External IR skips CodeGen only. It still passes VERITAS verification,
  canonicalization, identity, SVF, summaries, CPG, WPA, and publication.
- Installed public headers expose no LLVM or Clang native types.
- Existing `ProjectAnalysisRequest` and `AnalyzeProject` callers remain source
  compatible.
- Normal reports contain no source contents, environment values, or
  unrestricted compiler argument lists.
- Large-project timings are reports, not CI thresholds, until stable
  environment-specific ceilings are approved.

---

## File and Ownership Map

| Area | Files | Responsibility |
| --- | --- | --- |
| Public analysis API | `include/veritas/analysis/AnalysisRequest.h`, `AnalysisResult.h`, `ProjectAnalyzer.h` | Input variants, job policy, result schema, compatibility API |
| Atomic files | `include/veritas/core/AtomicFile.h`, `src/core/AtomicFile.cpp` | Same-filesystem temporary write and atomic replacement |
| Reporting | `src/analysis/reporting/AnalysisEventSink.h`, `AnalysisRunReporter.h/.cpp` | JSONL events, readable log, request/result/timing JSON, latest pointer |
| IR persistence | `src/analysis/llvm/IrArtifactStore.h/.cpp` | Bitcode CAS, cache mappings, immutable snapshots, current pointer |
| IR mechanics | `src/analysis/llvm/IrModuleUtilities.h/.cpp`, `OriginMap.h/.cpp`, `ProjectIrBuilder.h/.cpp` | Identity annotation, canonical bitcode, fidelity, source anchors, deterministic link |
| Project cache inputs | `include/veritas/build/AnalysisManifest.h`, `src/build/DependencyHasher.h/.cpp`, `ProjectManifestLoader.cpp` | Transitive dependency digests and cache-valid manifest fields |
| Acquisition seam | `src/analysis/pipeline/ProgramIrSource.h`, `CodeGenIrSource.h/.cpp`, `LocalAnalysisStage.h/.cpp` | Source-neutral `ProgramIr` acquisition and local analysis |
| Parallel scheduling | `src/analysis/llvm/InputUnitScheduler.h/.cpp` | Bounded work, cancellation, draining, deterministic failure selection |
| Analyzer orchestration | `src/analysis/AnalysisInputResolver.h/.cpp`, `ProjectAnalyzer.cpp` | Single resolution, acquisition, downstream stages, report finalization |
| WPA reporting | `include/veritas/wpa/WpaRunRepository.h`, `WpaOrchestrator.h`, `include/veritas/facts/AnalysisFactBus.h`, implementations | Reuse flags, batch ID, publication receipt, counts |
| External IR | `src/analysis/ir_adapter/*.h/.cpp` | Parse/verify `.bc`/`.ll`, fidelity, anonymous context, managed sidecar round-trip |
| CLI | `src/tools/veritas-build.cpp` | Argument parsing, request construction, compact stdout |
| Performance | `tools/generate_analysis_benchmark_project.py`, `tools/benchmark_veritas_build.py`, qualification tests | Synthetic medium/large corpus, scenario runner, JSON and Markdown report |
| Documentation | architecture, M1/M4 specs, guides, indexes | Current behavior, artifact contract, examples, measured recommendations |

### Task 1: Public input, job, and result contracts

**Files:**
- Create: `include/veritas/analysis/AnalysisRequest.h`
- Create: `include/veritas/analysis/AnalysisResult.h`
- Modify: `include/veritas/analysis/ProjectAnalyzer.h:15-133`
- Create: `tests/unit/analysis/AnalysisRequestTest.cpp`
- Modify: `tests/unit/analysis/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `AnalysisConfig`, `WpaEngineMode`, `UnknownFact`, and
  `ProjectAnalysisResult` definitions from `ProjectAnalyzer.h`.
- Produces: `AnalysisRequest`, `AnalysisInput`, `JobCount`,
  `ResolveJobCount`, `AnalysisResult`, and the compatibility alias
  `ProjectAnalysisResult`.

- [ ] **Step 1: Write the failing public-contract tests**

Add tests that construct every input variant, resolve explicit and automatic
job counts, verify the auto cap, and compile the legacy result name:

```cpp
TEST(AnalysisRequestTest, ResolvesAutomaticJobsWithCapAndFallback) {
  EXPECT_EQ(*ResolveJobCount(JobCount::Auto(), 20, 32), 8u);
  EXPECT_EQ(*ResolveJobCount(JobCount::Auto(), 3, 32), 3u);
  EXPECT_EQ(*ResolveJobCount(JobCount::Auto(), 3, 0), 1u);
}

TEST(AnalysisRequestTest, RejectsZeroExplicitJobs) {
  auto jobs = ResolveJobCount(JobCount::Explicit(0), 4, 8);
  ASSERT_FALSE(jobs.ok());
  EXPECT_EQ(jobs.status().code(), StatusCode::kInvalidArgument);
}

TEST(AnalysisRequestTest, KeepsLegacyResultName) {
  static_assert(std::is_same_v<ProjectAnalysisResult, AnalysisResult>);
}
```

- [ ] **Step 2: Run the target and verify the contract is missing**

Run:

```bash
cmake --build --preset default --target AnalysisRequestTest
```

Expected: FAIL because the two new headers and target do not exist.

- [ ] **Step 3: Add the public types and preserve legacy fields**

Move `AnalysisConfig` and `WpaEngineMode` into `AnalysisRequest.h`. Define the
request around a variant, not paths with sentinel emptiness:

```cpp
struct ProjectInputSpec { std::filesystem::path project_root; };
struct BitcodeInputSpec {
  enum class Kind : std::uint8_t { kLinkedFile, kModuleDirectory };
  Kind kind;
  std::filesystem::path path;
};
using AnalysisInput = std::variant<ProjectInputSpec, BitcodeInputSpec>;

struct JobCount {
  enum class Mode : std::uint8_t { kAuto, kExplicit };
  static JobCount Auto() { return {Mode::kAuto, 0}; }
  static JobCount Explicit(std::size_t value) {
    return {Mode::kExplicit, value};
  }
  Mode mode;
  std::size_t value;
};

struct AnalysisRequest {
  AnalysisInput input;
  std::filesystem::path output_root;
  JobCount jobs = JobCount::Auto();
  AnalysisConfig config = AnalysisConfig::Default();
};
```

Define the exact fidelity vocabulary, then define `AnalysisBoundary`,
`IrArtifactReference`,
`InputUnitAnalysisResult`, `AnalysisCounts`, `StageTiming`, and
`AnalysisResult`. Copy every current `ProjectAnalysisResult` field verbatim
into `AnalysisResult`, then add `input_kind`, `input_fidelity`,
`ir_snapshot_id`, `linked_bitcode_path`, `report_path`, `fact_batch_id`,
`publication_receipt_id`, `input_units`, `counts`, and `timings`. End with:

```cpp
using ProjectAnalysisResult = AnalysisResult;
```

```cpp
enum class InputFidelity : std::uint8_t {
  kSource,
  kDebugInfo,
  kSymbolsOnly,
  kStripped,
};
```

Implement `ResolveJobCount` inline with explicit validation and the approved
cap. Add `ProjectAnalyzer::Analyze(const AnalysisRequest&)` while retaining
`AnalyzeProject(const ProjectAnalysisRequest&, const AnalysisConfig&)`.

- [ ] **Step 4: Build and run the focused tests**

Run:

```bash
cmake --build --preset default --target AnalysisRequestTest project_analyzer_integration_test
ctest --test-dir build -R "AnalysisRequestTest|ProjectAnalyzerTest" --output-on-failure
```

Expected: PASS; existing project callers compile against the alias.

- [ ] **Step 5: Commit the API contract**

```bash
git add include/veritas/analysis tests/unit/analysis/CMakeLists.txt tests/unit/analysis/AnalysisRequestTest.cpp
git commit -m "feat: define input-neutral analysis contracts"
```

### Task 2: Atomic files and run reporting

**Files:**
- Create: `include/veritas/core/AtomicFile.h`
- Create: `src/core/AtomicFile.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `src/analysis/reporting/AnalysisEventSink.h`
- Create: `src/analysis/reporting/AnalysisRunReporter.h`
- Create: `src/analysis/reporting/AnalysisRunReporter.cpp`
- Modify: `src/analysis/CMakeLists.txt:14-49`
- Create: `tests/unit/core/AtomicFileTest.cpp`
- Modify: `tests/unit/core/CMakeLists.txt`
- Create: `tests/unit/analysis/AnalysisRunReporterTest.cpp`
- Modify: `tests/unit/analysis/CMakeLists.txt`

**Interfaces:**
- Consumes: `AnalysisRequest`, `AnalysisResult`, `AnalysisBoundary`, and
  `Status` from Task 1.
- Produces: `core::WriteFileAtomically`, `AnalysisEventSink`, and
  `AnalysisRunReporter::{Open, Emit, FinishSuccess, FinishFailure}`.

- [ ] **Step 1: Write atomicity and report-schema tests**

Use a fixed invocation ID in tests. Assert a success creates all five files,
a failure still creates `result.json`, and `runs/latest.json` changes only
after finalization:

```cpp
auto reporter = AnalysisRunReporter::Open(
    output, request, AnalysisRunReporterOptions{.invocation_id = "run-fixed"});
ASSERT_TRUE(reporter.ok());
ASSERT_FALSE(fs::exists(output / "runs" / "latest.json"));
ASSERT_TRUE((*reporter)->Emit({.boundary = AnalysisBoundary::kAcquisition,
                               .severity = EventSeverity::kInfo,
                               .code = "tu.generated",
                               .message = "generated tu:sha256:01"}).ok());
ASSERT_TRUE((*reporter)->FinishSuccess(result).ok());
EXPECT_TRUE(fs::is_regular_file(output / "runs/run-fixed/result.json"));
EXPECT_TRUE(fs::is_regular_file(output / "runs/run-fixed/timings.json"));
EXPECT_TRUE(fs::is_regular_file(output / "runs/run-fixed/diagnostics.jsonl"));
EXPECT_TRUE(fs::is_regular_file(output / "runs/run-fixed/analysis.log"));
EXPECT_TRUE(fs::is_regular_file(output / "runs/latest.json"));
```

The atomic-file test first writes `old`, replaces it with `new`, and verifies
that no filename containing `.tmp.` remains in the destination directory.

- [ ] **Step 2: Run the focused targets and observe the missing APIs**

```bash
cmake --build --preset default --target AtomicFileTest AnalysisRunReporterTest
```

Expected: FAIL because the atomic writer and reporter are not defined.

- [ ] **Step 3: Implement same-filesystem atomic replacement**

Expose this exact core API:

```cpp
Status WriteFileAtomically(const std::filesystem::path& destination,
                           std::span<const std::byte> bytes);
Status WriteTextFileAtomically(const std::filesystem::path& destination,
                               std::string_view text);
StatusOr<std::string> ReadTextFile(const std::filesystem::path& path);
```

Create the temporary file in `destination.parent_path()`, write in binary mode,
flush and close it, then call `std::filesystem::rename(temp, destination,
error)`. Remove the temporary file on every failure path. Use a process ID plus
an atomic counter for collision-free temporary names; do not use timestamps as
semantic data.

- [ ] **Step 4: Implement the thread-safe reporter**

Define typed events:

```cpp
struct AnalysisEvent {
  AnalysisBoundary boundary;
  EventSeverity severity;
  std::string code;
  std::string message;
  std::optional<std::size_t> input_ordinal;
  std::map<std::string, std::string> fields;
};

class AnalysisEventSink {
 public:
  virtual ~AnalysisEventSink() = default;
  virtual Status Emit(AnalysisEvent event) = 0;
};
```

`AnalysisRunReporter` creates `request.json` at open, serializes events under a
mutex to JSONL and readable text, writes canonical result and timing objects,
and atomically publishes `runs/latest.json` only from `FinishSuccess` or
`FinishFailure`. Sanitize event fields by an allowlist (`input_unit_id`,
`artifact_digest`, `cache_state`, `stage`, `status_code`, `count`) before
writing them.

- [ ] **Step 5: Run reporter tests**

```bash
cmake --build --preset default --target AtomicFileTest AnalysisRunReporterTest
ctest --test-dir build -R "AtomicFileTest|AnalysisRunReporterTest" --output-on-failure
```

Expected: PASS, including deterministic key order and failure retention.

- [ ] **Step 6: Commit reporting infrastructure**

```bash
git add include/veritas/core/AtomicFile.h src/core src/analysis/reporting src/analysis/CMakeLists.txt tests/unit/core tests/unit/analysis
git commit -m "feat: persist structured analysis run reports"
```

### Task 3: Content-addressed IR objects, cache records, and snapshots

**Files:**
- Create: `src/analysis/llvm/IrArtifactStore.h`
- Create: `src/analysis/llvm/IrArtifactStore.cpp`
- Modify: `src/analysis/llvm/CMakeLists.txt:14-36`
- Create: `tests/unit/analysis/llvm/IrArtifactStoreTest.cpp`
- Modify: `tests/unit/analysis/llvm/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::WriteFileAtomically` and `InputFidelity`.
- Produces: `IrObjectDescriptor`, `InputUnitArtifact`, `IrSnapshotDescriptor`,
  and `IrArtifactStore` CAS/cache/snapshot operations.

- [ ] **Step 1: Write failing CAS, cache, and snapshot tests**

Cover deduplication, racing identical writes, corrupt existing content,
cache-record validation, and snapshot pointer atomicity:

```cpp
auto store = IrArtifactStore::Open(output);
ASSERT_TRUE(store.ok());
auto first = (*store)->PutBitcode(Bytes("same-bitcode"));
auto second = (*store)->PutBitcode(Bytes("same-bitcode"));
ASSERT_TRUE(first.ok());
ASSERT_TRUE(second.ok());
EXPECT_EQ(first->digest, second->digest);
EXPECT_EQ(first->path, second->path);

ASSERT_TRUE((*store)->StoreCacheRecord("cache-key", unit).ok());
auto hit = (*store)->LookupCacheRecord("cache-key");
ASSERT_TRUE(hit.ok());
ASSERT_TRUE(hit->has_value());
EXPECT_EQ((*hit)->object.digest, unit.object.digest);
```

Publish snapshot A, force snapshot B to fail before index publication, and
assert `llvm/current.json` still points to A.

- [ ] **Step 2: Run the test target and verify the store is missing**

```bash
cmake --build --preset default --target IrArtifactStoreTest
```

Expected: FAIL because `IrArtifactStore.h` does not exist.

- [ ] **Step 3: Implement immutable object and cache operations**

Use these signatures:

```cpp
class IrArtifactStore {
 public:
  static StatusOr<std::unique_ptr<IrArtifactStore>> Open(
      const std::filesystem::path& output_root);
  StatusOr<IrObjectDescriptor> PutBitcode(
      std::span<const std::byte> canonical_bitcode);
  StatusOr<std::optional<InputUnitArtifact>> LookupCacheRecord(
      std::string_view cache_key) const;
  Status StoreCacheRecord(std::string_view cache_key,
                          const InputUnitArtifact& artifact);
  StatusOr<IrSnapshotDescriptor> PublishSnapshot(
      const build::AnalysisManifest& manifest,
      std::span<const InputUnitArtifact> units,
      const IrObjectDescriptor& linked_object,
      std::string_view module_hash);
};
```

Hash canonical bytes with `core::ComputeSHA256`; object paths are
`llvm/objects/<lowercase-hex>.bc`. Write with exclusive put-if-absent semantics:
an existing file is re-read and re-hashed before success. Cache records live at
`llvm/cache/input-units/<cache-key>.json` and are valid only when schema,
cache key, object size, object digest, and object file all agree.

- [ ] **Step 4: Implement immutable snapshots and pointer documents**

Derive `acquisition_id` from the ordered unit IDs/object digests, linked object
digest, context IDs, fidelity, and `ir-acquisition.v1`. Materialize
`snapshots/<id>/tus`, `project.bc`, and canonical `index.json`; hard-link each
object and fall back to `copy_file` only for unsupported link errors. Atomically
replace `llvm/current.json` after `index.json` and every artifact are verified.

- [ ] **Step 5: Run focused tests including a repeat loop**

```bash
cmake --build --preset default --target IrArtifactStoreTest
ctest --test-dir build -R IrArtifactStoreTest --repeat until-fail:10 --output-on-failure
```

Expected: PASS with one object for identical concurrent inputs and no pointer
advance after the injected failure.

- [ ] **Step 6: Commit IR persistence**

```bash
git add src/analysis/llvm/IrArtifactStore.* src/analysis/llvm/CMakeLists.txt tests/unit/analysis/llvm
git commit -m "feat: add atomic LLVM IR artifact store"
```

### Task 4: Reusable IR normalization, identity, fidelity, and origin mapping

**Files:**
- Create: `src/analysis/llvm/IrModuleUtilities.h`
- Create: `src/analysis/llvm/IrModuleUtilities.cpp`
- Modify: `src/analysis/llvm/ProjectIrBuilder.cpp:32-199`
- Modify: `src/analysis/llvm/ProjectIrBuilder.h:23-51`
- Modify: `src/analysis/llvm/OriginMap.h:18-76`
- Modify: `src/analysis/llvm/OriginMap.cpp:17-72`
- Modify: `src/analysis/llvm/CMakeLists.txt`
- Create: `tests/unit/analysis/llvm/IrModuleUtilitiesTest.cpp`
- Modify: `tests/unit/analysis/llvm/CMakeLists.txt`

**Interfaces:**
- Consumes: `build::TranslationUnitCommand`, `build::ProgramContext`, LLVM
  modules, and `InputFidelity`.
- Produces: canonical bitcode helpers, shared function-identity annotation,
  fidelity detection, and function source anchors in `OriginMap`.

- [ ] **Step 1: Write failing normalization and fidelity tests**

Construct equivalent modules with different module identifiers and source
filenames, then require identical bytes. Add T0, T1, and T2 modules and verify
their classifications. For T0, attach a `DISubprogram` and assert the final
origin map retains normalized file/line information.

```cpp
NormalizeModuleForVeritas(first);
NormalizeModuleForVeritas(second);
EXPECT_EQ(*CanonicalBitcode(first), *CanonicalBitcode(second));
EXPECT_EQ(DetectInputFidelity(debug_module), InputFidelity::kDebugInfo);
EXPECT_EQ(DetectInputFidelity(symbol_module), InputFidelity::kSymbolsOnly);
EXPECT_EQ(DetectInputFidelity(stripped_module), InputFidelity::kStripped);
```

- [ ] **Step 2: Run the target and verify the helpers are absent**

```bash
cmake --build --preset default --target IrModuleUtilitiesTest
```

Expected: FAIL because the utility header is missing.

- [ ] **Step 3: Move identity logic out of `ProjectIrBuilder.cpp`**

Expose the existing behavior without changing its domains:

```cpp
Status AnnotateFunctionIdentities(
    llvm::Module& module,
    const build::TranslationUnitCommand& command,
    const build::ProgramContext& context);
Status PopulateOriginMap(llvm::Module& linked, OriginMap& origins,
                         InputFidelity fidelity);
Status NormalizeModuleForVeritas(llvm::Module& module);
StatusOr<std::vector<std::byte>> CanonicalBitcode(const llvm::Module& module);
std::string CanonicalModuleHash(const llvm::Module& module);
InputFidelity DetectInputFidelity(const llvm::Module& module);
```

Move `kFunctionVariantAttribute`, `MakeIdentity`, `FunctionSignature`, and
`AnnotateFunctionIdentities` intact. `NormalizeModuleForVeritas` sets the
module identifier to `veritas.project`, clears source filename, and preserves
value names. `CanonicalBitcode` uses `llvm::WriteBitcodeToFile` into a byte
vector and hashes through `veritas::core`, not a second SHA implementation.

- [ ] **Step 4: Extend `OriginMap` for T0 anchors**

Add this path-free record:

```cpp
struct DebugSourceAnchor {
  std::string logical_file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};
```

Store it by function pointer beside the symbol map. For T0, obtain
`DISubprogram::getFile()`, normalize separators, remove compile-directory
prefixes, and retain the subprogram line. For T1, record only stable symbols.
Clear and remove anchor entries with their corresponding function mappings.

- [ ] **Step 5: Run unit and existing local-analysis tests**

```bash
cmake --build --preset default --target IrModuleUtilitiesTest local_analysis_stage_integration_test
ctest --test-dir build -R "IrModuleUtilitiesTest|LocalAnalysisStageTest" --output-on-failure
```

Expected: PASS with current function-variant identities unchanged.

- [ ] **Step 6: Commit reusable IR mechanics**

```bash
git add src/analysis/llvm tests/unit/analysis/llvm
git commit -m "refactor: share canonical LLVM IR mechanics"
```

### Task 5: Dependency-complete manifest hashing

**Files:**
- Create: `src/build/DependencyHasher.h`
- Create: `src/build/DependencyHasher.cpp`
- Modify: `src/build/CMakeLists.txt:22-53`
- Modify: `include/veritas/build/AnalysisManifest.h:33-80`
- Modify: `src/build/AnalysisManifest.cpp:90-244`
- Modify: `src/build/ProjectManifestLoader.cpp:298-413`
- Create: `tests/integration/build/DependencyHasherTest.cpp`
- Modify: `tests/integration/build/CMakeLists.txt`
- Modify: `tests/integration/build/ProjectManifestLoaderTest.cpp`

**Interfaces:**
- Consumes: normalized compile commands and Clang's in-process dependency
  scanner.
- Produces: `DependencyHasher::HashTranslationUnit`, populated
  `preprocessor_hash`, aggregate `include_closure_hash`, and header-sensitive
  revision identity.

- [ ] **Step 1: Write header-change and path-stability tests**

Create a fixture copy whose source includes `public.h`. Load its manifest,
change only `public.h`, load again, and assert the TU preprocessor hash,
aggregate include-closure hash, revision ID, and build variant react correctly.
Materialize the unchanged fixture under two roots and assert identical hashes.

```cpp
EXPECT_NE(before.translation_units[0].preprocessor_hash,
          after.translation_units[0].preprocessor_hash);
EXPECT_NE(before.context.include_closure_hash,
          after.context.include_closure_hash);
EXPECT_NE(before.context.revision_id, after.context.revision_id);
EXPECT_EQ(first_root.context.include_closure_hash,
          second_root.context.include_closure_hash);
```

- [ ] **Step 2: Run focused tests and observe the empty hashes**

```bash
cmake --build --preset default --target DependencyHasherTest ProjectManifestLoaderTest
ctest --test-dir build -R "DependencyHasherTest|ProjectManifestLoaderTest" --output-on-failure
```

Expected: FAIL because current `preprocessor_hash` and
`include_closure_hash` are empty.

- [ ] **Step 3: Implement in-process dependency discovery**

Use `clang::dependencies::DependencyScanningService` in
`DependencyDirectivesScan` mode and one `clang::tooling::DependencyScanningTool`
per calling thread. Call `getDependencyFile` with the normalized command and
working directory. Parse the make dependency output with a state machine that
handles escaped spaces, escaped backslashes, and backslash-newline
continuations; reject malformed output instead of returning an incomplete set.

Expose:

```cpp
struct TranslationUnitDependencyHash {
  std::vector<std::filesystem::path> files;
  std::string digest;
};

class DependencyHasher {
 public:
  StatusOr<TranslationUnitDependencyHash> HashTranslationUnit(
      const TranslationUnitCommand& command,
      const std::filesystem::path& project_root) const;
};
```

Canonicalize each dependency as `(PathRootKind, root_id, relative_path,
content_digest)`, sort and deduplicate, then domain-hash with
`veritas.preprocessor-closure.v1`. Never hash an mtime or checkout root.

- [ ] **Step 4: Populate manifest identity before TU IDs are derived**

Scan every normalized command, set each `preprocessor_hash`, and derive
`context.include_closure_hash` from sorted `(relative source,
preprocessor_hash)` pairs. Derive revision IDs with the new domain
`veritas.revision.v2` over source-tree plus include-closure hashes. Mark the
manifest input kind as `project` and fidelity as `source` in diagnostic JSON.

- [ ] **Step 5: Run build-manifest tests**

```bash
cmake --build --preset default --target AnalysisManifestTest DependencyHasherTest ProjectManifestLoaderTest
ctest --test-dir build -R "AnalysisManifestTest|DependencyHasherTest|ProjectManifestLoaderTest" --output-on-failure
```

Expected: PASS; header-only changes invalidate the revision and affected TU.

- [ ] **Step 6: Commit dependency-complete identities**

```bash
git add include/veritas/build src/build tests/integration/build tests/unit/build
git commit -m "feat: hash translation unit include closures"
```

### Task 6: Serial persisted project acquisition seam

**Files:**
- Create: `src/analysis/pipeline/ProgramIrSource.h`
- Create: `src/analysis/pipeline/CodeGenIrSource.h`
- Create: `src/analysis/pipeline/CodeGenIrSource.cpp`
- Modify: `src/analysis/pipeline/CMakeLists.txt:14-29`
- Modify: `src/analysis/pipeline/LocalAnalysisStage.h:24-42`
- Modify: `src/analysis/pipeline/LocalAnalysisStage.cpp:25-50`
- Modify: `src/analysis/llvm/ProjectIrBuilder.h:35-51`
- Modify: `src/analysis/llvm/ProjectIrBuilder.cpp:144-303`
- Create: `tests/integration/analysis/CodeGenIrSourceTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/integration/analysis/LocalAnalysisStageTest.cpp`

**Interfaces:**
- Consumes: manifest hashes, `IrArtifactStore`, `IrModuleUtilities`, and
  `AnalysisEventSink`.
- Produces: `AcquiredProgramIr`, `ProgramIrSource`, serial `CodeGenIrSource`,
  persisted project bitcode, and the source-neutral local-analysis overload.

- [ ] **Step 1: Write a failing persisted-acquisition integration test**

Acquire the `multiple_tus` fixture with one job and assert two input units, one
linked module, the expected snapshot layout, and successful local extraction:

```cpp
CodeGenIrSource source(*manifest, 1);
auto acquired = source.Acquire(**store, events);
ASSERT_TRUE(acquired.ok()) << acquired.status().message();
EXPECT_EQ(acquired->units.size(), 2u);
EXPECT_TRUE(fs::is_regular_file(acquired->snapshot.path / "project.bc"));
EXPECT_TRUE(fs::is_directory(acquired->snapshot.path / "tus"));
auto local = RunLocalAnalysis(std::move(acquired->program_ir),
                              acquired->manifest.context);
ASSERT_TRUE(local.ok()) << local.status().message();
EXPECT_EQ(local->summary_drafts.size(), 2u);
```

- [ ] **Step 2: Run the target and verify the acquisition seam is absent**

```bash
cmake --build --preset default --target CodeGenIrSourceTest
```

Expected: FAIL because `ProgramIrSource` and `CodeGenIrSource` do not exist.

- [ ] **Step 3: Define the source-neutral handoff**

```cpp
struct AcquiredProgramIr {
  build::AnalysisManifest manifest;
  ProgramIr program_ir;
  llvm::IrSnapshotDescriptor snapshot;
  InputFidelity fidelity;
  std::vector<llvm::InputUnitArtifact> units;
};

class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual StatusOr<AcquiredProgramIr> Acquire(
      llvm::IrArtifactStore& artifacts,
      AnalysisEventSink& events) = 0;
};
```

Add `RunLocalAnalysis(ProgramIr, const build::ProgramContext&)`. It runs only
`LocalFactExtractor::ExtractV2` and `BuildLocalSummaryV2`. Retain the manifest
overload as a compatibility path until all tests migrate.

- [ ] **Step 4: Split per-TU generation from deterministic linking**

Add to `ProjectIrBuilder`:

```cpp
StatusOr<GeneratedTranslationUnit> GenerateTranslationUnit(
    const build::TranslationUnitCommand& command,
    const build::ProgramContext& context);
StatusOr<pipeline::ProgramIr> LinkPersistedModules(
    const build::AnalysisManifest& manifest,
    std::span<const InputUnitArtifact> units);
```

`GenerateTranslationUnit` creates a private LLVM context, runs the existing
Clang action, annotates identities, verifies, normalizes the input-unit module
without erasing its logical unit identity, serializes canonical bitcode, and
reports whether it defines `main`. `LinkPersistedModules` sorts by ordinal,
rejects multiple definitions of `main` with the existing actionable message,
parses and links one persisted module at a time into `ProgramIr::GetContext`,
normalizes the final module, populates origins, hashes it, and releases each
source module after linking.

- [ ] **Step 5: Implement serial `CodeGenIrSource`**

For each manifest entry in ordinal order, call `GenerateTranslationUnit`, put
its bytes into the artifact store, emit `tu.generated`, and collect an
`InputUnitArtifact`. Link the persisted objects, put final canonical bitcode,
publish the snapshot, and return `AcquiredProgramIr`. Do not add cache reuse or
threads in this task.

- [ ] **Step 6: Run acquisition and compatibility tests**

```bash
cmake --build --preset default --target CodeGenIrSourceTest local_analysis_stage_integration_test SemanticZooFixtureTest
ctest --test-dir build -R "CodeGenIrSourceTest|LocalAnalysisStageTest|SemanticZooFixtureTest" --output-on-failure
```

Expected: PASS; the serial source persists two TU objects and deterministic
`project.bc` while old local-analysis callers still work.

- [ ] **Step 7: Commit the serial acquisition seam**

```bash
git add src/analysis/pipeline src/analysis/llvm/ProjectIrBuilder.* tests/integration/analysis
git commit -m "feat: persist serial project LLVM acquisition"
```

### Task 7: Safe project input-unit cache reuse

**Files:**
- Modify: `src/analysis/pipeline/CodeGenIrSource.h`
- Modify: `src/analysis/pipeline/CodeGenIrSource.cpp`
- Modify: `src/analysis/llvm/IrArtifactStore.h`
- Modify: `src/analysis/llvm/IrArtifactStore.cpp`
- Modify: `tests/integration/analysis/CodeGenIrSourceTest.cpp`
- Modify: `tests/unit/analysis/llvm/IrArtifactStoreTest.cpp`

**Interfaces:**
- Consumes: dependency-complete manifest fields and cache-record operations.
- Produces: `ComputeInputUnitCacheKey` and exact cache hit/miss reporting.

- [ ] **Step 1: Write cache hit and invalidation tests**

Run acquisition twice into the same output and assert the second run has two
hits. Then change one source, one included header, one compile argument, and
the schema version in separate cases; assert only valid units are reused.

```cpp
auto warm = AcquireFixture(project, output, 1);
ASSERT_TRUE(warm.ok());
EXPECT_EQ(CountCacheState(*warm, CacheState::kHit), 2u);

RewriteFile(project / "shared.h", "#define VALUE 2\n");
auto changed = AcquireFixture(project, output, 1);
ASSERT_TRUE(changed.ok());
EXPECT_EQ(CountCacheState(*changed, CacheState::kMiss), 2u);
```

- [ ] **Step 2: Run the test and verify every acquisition regenerates**

```bash
cmake --build --preset default --target CodeGenIrSourceTest
ctest --test-dir build -R CodeGenIrSourceTest --output-on-failure
```

Expected: FAIL because no unit reports `CacheState::kHit`.

- [ ] **Step 3: Implement the versioned cache key**

```cpp
std::string ComputeInputUnitCacheKey(
    const build::TranslationUnitCommand& unit,
    const build::ProgramContext& context,
    std::string_view codegen_schema_version);
```

Length-prefix and domain-hash `veritas.codegen-cache.v1`, normalized arguments,
`command_hash`, `preprocessor_hash`, compiler ID/version, target triple,
type-layout hash, macro-set hash, build variant, and identity annotation schema.
Return lowercase SHA-256 hex for the cache-record filename.

- [ ] **Step 4: Reuse only fully validated records**

Before CodeGen, call `LookupCacheRecord`. A hit must validate schema, cache key,
unit identity, object digest/size, and object bytes. Emit `tu.cache_hit` and use
the descriptor without parsing it until deterministic link. A miss generates,
stores the object, then atomically stores the key mapping and emits
`tu.cache_miss`. Treat malformed records as misses and emit a warning event.

- [ ] **Step 5: Run cache and artifact tests**

```bash
cmake --build --preset default --target CodeGenIrSourceTest IrArtifactStoreTest
ctest --test-dir build -R "CodeGenIrSourceTest|IrArtifactStoreTest" --output-on-failure
```

Expected: PASS for exact reuse, header invalidation, and corrupt-record miss.

- [ ] **Step 6: Commit cache reuse**

```bash
git add src/analysis/pipeline/CodeGenIrSource.* src/analysis/llvm/IrArtifactStore.* tests/integration/analysis/CodeGenIrSourceTest.cpp tests/unit/analysis/llvm/IrArtifactStoreTest.cpp
git commit -m "feat: reuse dependency-complete TU bitcode cache"
```

### Task 8: Bounded parallel input-unit scheduling

**Files:**
- Create: `src/analysis/llvm/InputUnitScheduler.h`
- Create: `src/analysis/llvm/InputUnitScheduler.cpp`
- Modify: `src/analysis/llvm/CMakeLists.txt`
- Modify: `src/analysis/pipeline/CodeGenIrSource.cpp`
- Create: `tests/unit/analysis/llvm/InputUnitSchedulerTest.cpp`
- Modify: `tests/unit/analysis/llvm/CMakeLists.txt`
- Modify: `tests/integration/analysis/CodeGenIrSourceTest.cpp`

**Interfaces:**
- Consumes: resolved positive job count and the serial per-unit acquisition
  callback.
- Produces: `InputUnitScheduler::Run`, deterministic ordered completions, complete
  observed diagnostics, and lowest-ordinal primary failure.

- [ ] **Step 1: Write scheduling, cancellation, and ordering tests**

Inject a worker callback that blocks selected ordinals so completion order is
`3, 1, 2, 0`, and assert returned results are `0, 1, 2, 3`. Inject failures at
ordinals three and one, ensure unscheduled work stops, active work drains, and
the returned status cites ordinal one.

```cpp
auto result = scheduler.Run(4, 4, worker);
EXPECT_EQ(result.completed_ordinals,
          (std::vector<std::size_t>{0, 1, 2, 3}));
EXPECT_TRUE(result.failures.empty());
EXPECT_LE(maximum_simultaneous_workers.load(), 4u);
```

For the failing run, assert `failures` is sorted by ordinal and
`PrimaryFailure()` returns ordinal one even though ordinal three failed first.

- [ ] **Step 2: Run the target and verify the scheduler is missing**

```bash
cmake --build --preset default --target InputUnitSchedulerTest
```

Expected: FAIL because the scheduler header does not exist.

- [ ] **Step 3: Implement bounded work with deterministic failure selection**

```cpp
using InputUnitWork = std::function<Status(std::size_t)>;

struct InputUnitScheduleResult {
  std::vector<std::size_t> completed_ordinals;
  std::vector<InputUnitFailure> failures;

  const InputUnitFailure* PrimaryFailure() const;
};

InputUnitScheduleResult Run(std::size_t unit_count,
                            std::size_t jobs,
                            InputUnitWork work);
```

Use `min(unit_count, jobs)` `std::thread` workers, an atomic next ordinal, an
atomic cancellation flag, and caller-owned fixed-size result slots indexed by
ordinal. Each callback writes only its own preallocated slot. After any
failure, prevent new ordinal claims but join every active thread. Sort observed
failures and completed ordinals numerically; `PrimaryFailure` selects the first
failure. This keeps every observed diagnostic available to the caller before
it returns the primary status. Never detach a worker.

- [ ] **Step 4: Integrate the scheduler into `CodeGenIrSource`**

Make the existing serial cache-aware callback the scheduler work function; it
writes the acquired artifact into the preallocated slot for its ordinal.
Each callback creates and destroys its own `ProjectIrBuilder` and LLVM context.
After scheduling, emit all observed diagnostics in ordinal order, link the
ordered descriptors serially, and publish the snapshot. `jobs == 1` uses the
same scheduler path.

- [ ] **Step 5: Verify parallel determinism and stress the scheduler**

```bash
cmake --build --preset default --target InputUnitSchedulerTest CodeGenIrSourceTest
ctest --test-dir build -R InputUnitSchedulerTest --repeat until-fail:50 --output-on-failure
ctest --test-dir build -R CodeGenIrSourceTest --repeat until-fail:10 --output-on-failure
```

Expected: PASS; job counts one, two, and auto yield the same module and snapshot
content IDs.

- [ ] **Step 6: Commit parallel acquisition**

```bash
git add src/analysis/llvm/InputUnitScheduler.* src/analysis/llvm/CMakeLists.txt src/analysis/pipeline/CodeGenIrSource.cpp tests/unit/analysis/llvm tests/integration/analysis/CodeGenIrSourceTest.cpp
git commit -m "feat: parallelize deterministic project CodeGen"
```

### Task 9: Route project analysis through input-neutral orchestration

**Files:**
- Create: `src/analysis/AnalysisInputResolver.h`
- Create: `src/analysis/AnalysisInputResolver.cpp`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `tests/integration/analysis/ProjectAnalyzerTest.cpp`
- Modify: `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp`

**Interfaces:**
- Consumes: `AnalysisRequest`, `ProgramIrSource`, `IrArtifactStore`,
  `AnalysisRunReporter`, and the existing downstream analysis stages.
- Produces: `ResolvedAnalysisInput`, `AnalysisInputResolver::Resolve`, and the
  canonical `ProjectAnalyzer::Analyze` orchestration path.

- [ ] **Step 1: Write resolver and orchestration tests**

Add a project-mode test that records acquisition events and verifies the
manifest is resolved once, one immutable snapshot is selected, and the new
entry point preserves the legacy semantic IDs:

```cpp
auto modern = analyzer.Analyze(AnalysisRequest{
    .input = ProjectInputSpec{project_root},
    .output_root = output_root,
    .jobs = JobCount::Explicit(1),
    .config = config,
});
ASSERT_TRUE(modern.ok()) << modern.status();
ASSERT_EQ(modern->project_id, legacy->project_id);
ASSERT_EQ(modern->build_variant_id, legacy->build_variant_id);
EXPECT_TRUE(fs::exists(modern->linked_bitcode_path));
EXPECT_TRUE(fs::exists(modern->report_path));
EXPECT_EQ(ReadEvents(modern->report_path, "manifest.loaded").size(), 1u);
```

Add a failure case after acquisition and assert `result.json`,
`diagnostics.jsonl`, and `analysis.log` retain the last successful boundary and
the downstream failure status.

- [ ] **Step 2: Run the focused tests and observe the missing entry point**

```bash
cmake --build --preset default --target project_analyzer_integration_test project_analyzer_wpa_integration_test
ctest --test-dir build -R "ProjectAnalyzerTest|ProjectAnalyzerWpaTest" --output-on-failure
```

Expected: FAIL because project analysis does not yet route through an
input-neutral resolver or reporter.

- [ ] **Step 3: Implement one resolution path**

Define the resolver result without exposing LLVM types:

```cpp
struct ResolvedAnalysisInput {
  std::filesystem::path output_root;
  std::unique_ptr<ProgramIrSource> source;
};

class AnalysisInputResolver {
 public:
  StatusOr<ResolvedAnalysisInput> Resolve(const AnalysisRequest& request,
                                          AnalysisEventSink& events) const;
};
```

In the project branch, call `ResolveProjectInput` and `LoadProjectManifest`
exactly once, resolve the job count from the translation-unit count, and
construct `CodeGenIrSource` with the resolved manifest and jobs. Keep the
bitcode branch returning `kUnimplemented` until Task 12.

- [ ] **Step 4: Make `Analyze` the canonical pipeline**

Open `AnalysisRunReporter` before resolution. Resolve the source and artifact
store, acquire `ProgramIr`, then call a source-neutral local-analysis function:

```cpp
StatusOr<LocalAnalysisOutput> RunLocalAnalysis(
    const ProgramIr& program_ir,
    const AnalysisManifest& manifest,
    const AnalysisConfig& config,
    AnalysisEventSink& events);
```

Keep the established order after acquisition: local facts, SVF, model merge,
Thin CPG, summary and CPG publication, WPA, fact publication, and FactStore
persistence. Emit a start and completion event at every boundary. Finalize the
report exactly once on success or failure.

- [ ] **Step 5: Preserve the compatibility wrapper**

Implement `AnalyzeProject` by constructing a project `AnalysisRequest` with
`JobCount::Auto()` and delegating to `Analyze`. Preserve all existing error
codes and semantic result fields.

- [ ] **Step 6: Verify project behavior and commit**

```bash
cmake --build --preset default --target project_analyzer_integration_test project_analyzer_wpa_integration_test veritas-build
ctest --test-dir build -R "ProjectAnalyzerTest|ProjectAnalyzerWpaTest" --output-on-failure
git add src/analysis/AnalysisInputResolver.* src/analysis/ProjectAnalyzer.cpp src/analysis/CMakeLists.txt tests/integration/analysis/ProjectAnalyzerTest.cpp tests/integration/analysis/ProjectAnalyzerWpaTest.cpp
git commit -m "refactor: unify project analysis orchestration"
```

### Task 10: Report WPA execution and fact-publication details

**Files:**
- Modify: `include/veritas/wpa/WpaRunRepository.h`
- Modify: `include/veritas/wpa/WpaOrchestrator.h`
- Modify: `src/wpa/WpaOrchestrator.cpp`
- Modify: `include/veritas/facts/AnalysisFactBus.h`
- Modify: `src/facts/AnalysisFactBus.cpp`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Modify: `tests/unit/wpa/WpaOrchestratorTest.cpp`
- Modify: `tests/unit/facts/AnalysisFactBusTest.cpp`
- Modify: `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp`

**Interfaces:**
- Consumes: existing WPA component completions, analysis fact batches, and
  configured fact sinks.
- Produces: operational cache-reuse flags, a stable publication receipt, and
  `WpaPublicationResult` for the run report.

- [ ] **Step 1: Write cache-reuse, receipt, and aggregate-count tests**

Run the same WPA request twice and assert the second completion is marked
reused while its semantic batch ID is unchanged. Publish the same batch to the
same ordered sink set twice and assert the receipt ID is stable:

```cpp
ASSERT_FALSE(first.completed.front().reused);
ASSERT_TRUE(second.completed.front().reused);
EXPECT_EQ(first.batch_id, second.batch_id);

auto receipt_a = bus.PublishWithReceipt(batch);
auto receipt_b = bus.PublishWithReceipt(batch);
ASSERT_TRUE(receipt_a.ok() && receipt_b.ok());
EXPECT_EQ(receipt_a->receipt_id, receipt_b->receipt_id);
EXPECT_EQ(receipt_a->delivered_sinks, expected_sorted_sink_ids);
```

Extend the integration assertion to cover expected, completed, reused,
fact-count, witness-count, batch-ID, and receipt-ID fields in `result.json`.

- [ ] **Step 2: Run the tests and verify the details are absent**

```bash
cmake --build --preset default --target WpaOrchestratorTest AnalysisFactBusTest project_analyzer_wpa_integration_test
ctest --test-dir build -R "WpaOrchestratorTest|AnalysisFactBusTest|ProjectAnalyzerWpaTest" --output-on-failure
```

Expected: FAIL because completions have no reuse flag and publication returns
only `Status`.

- [ ] **Step 3: Add the operational reuse flag without changing semantic IDs**

Add `bool reused = false` to `WpaComponentCompletion`. Set it to true on the
repository reuse branch and false on execution. Do not serialize it into any
semantic batch hash or stable component-result identity.

- [ ] **Step 4: Add an idempotent publication receipt**

```cpp
struct AnalysisFactPublicationReceipt {
  std::string receipt_id;
  StableId run_id;
  StableId batch_id;
  std::vector<std::string> delivered_sinks;
};

StatusOr<AnalysisFactPublicationReceipt> PublishWithReceipt(
    const AnalysisFactBatch& batch);
Status Publish(const AnalysisFactBatch& batch);
```

Compute `receipt_id` as `receipt:sha256:<hex>` in domain
`veritas.fact-publication-receipt.v1` over the run ID, batch ID, and sorted
sink identifiers. Keep `Publish` as a compatibility wrapper around
`PublishWithReceipt`.

- [ ] **Step 5: Return a structured WPA publication result**

Refactor the analyzer helper to return:

```cpp
struct WpaPublicationResult {
  StableId wpa_run_id;
  StableId batch_id;
  std::string receipt_id;
  std::size_t expected_components;
  std::size_t completed_components;
  std::size_t reused_components;
  std::size_t fact_count;
  std::size_t witness_count;
  std::vector<Diagnostic> diagnostics;
};
```

Populate `AnalysisResult` and the run reporter from this value instead of
mutating optional output pointers.

- [ ] **Step 6: Verify and commit the reporting extension**

```bash
cmake --build --preset default --target WpaOrchestratorTest AnalysisFactBusTest project_analyzer_wpa_integration_test
ctest --test-dir build -R "WpaOrchestratorTest|AnalysisFactBusTest|ProjectAnalyzerWpaTest" --output-on-failure
git add include/veritas/wpa/WpaRunRepository.h include/veritas/wpa/WpaOrchestrator.h src/wpa/WpaOrchestrator.cpp include/veritas/facts/AnalysisFactBus.h src/facts/AnalysisFactBus.cpp src/analysis/ProjectAnalyzer.cpp tests/unit/wpa/WpaOrchestratorTest.cpp tests/unit/facts/AnalysisFactBusTest.cpp tests/integration/analysis/ProjectAnalyzerWpaTest.cpp
git commit -m "feat: report WPA and fact publication details"
```

### Task 11: Parse, verify, canonicalize, and classify external LLVM IR

**Files:**
- Create: `src/analysis/ir_adapter/CMakeLists.txt`
- Create: `src/analysis/ir_adapter/BitcodeModuleLoader.h`
- Create: `src/analysis/ir_adapter/BitcodeModuleLoader.cpp`
- Modify: `src/analysis/CMakeLists.txt`
- Create: `tests/integration/analysis/ir_adapter/CMakeLists.txt`
- Create: `tests/integration/analysis/ir_adapter/BitcodeModuleLoaderTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`

**Interfaces:**
- Consumes: `.bc` or `.ll` files, `InputFidelity`, and the canonical-bitcode
  utility from Task 4.
- Produces: verified canonical module bytes, deterministic directory
  enumeration, main-definition detection, and T0/T1/T2 classification.

- [ ] **Step 1: Write parser, equivalence, enumeration, and fidelity tests**

Build a fixture module through the LLVM API, write it once as textual IR and
once as bitcode, then assert both inputs produce the same canonical digest.
Cover lexical direct-child ordering, ignored unrelated files, malformed IR,
incompatible LLVM bitcode, duplicate logical names, main detection, and these
fidelity cases:

```cpp
EXPECT_EQ(Load(debug_ir)->fidelity, InputFidelity::kDebugInfo);
EXPECT_EQ(Load(symbol_ir)->fidelity, InputFidelity::kSymbolsOnly);
EXPECT_EQ(Load(stripped_ir)->fidelity, InputFidelity::kStripped);
```

- [ ] **Step 2: Run the target and observe the missing adapter**

```bash
cmake --build --preset default --target BitcodeModuleLoaderTest
```

Expected: FAIL because the adapter target and loader do not exist.

- [ ] **Step 3: Implement deterministic input discovery and parsing**

```cpp
struct LoadedBitcodeModule {
  std::size_t ordinal;
  std::string logical_name;
  InputFidelity fidelity;
  std::vector<std::byte> canonical_input_bitcode;
  bool defines_main;
};

class BitcodeModuleLoader {
 public:
  StatusOr<LoadedBitcodeModule> Load(
      const std::filesystem::path& path,
      std::size_t ordinal,
      std::string logical_name) const;
  StatusOr<std::vector<std::filesystem::path>> EnumerateDirectory(
      const std::filesystem::path& path) const;
};
```

Use `llvm::parseIRFile` so both `.bc` and `.ll` are accepted, then
`llvm::verifyModule` and the shared normalization and canonical-bitcode path.
Directory enumeration accepts direct regular files ending in `.bc` or `.ll`,
normalizes paths, sorts by filename then full normalized path, and rejects an
empty set.

- [ ] **Step 4: Implement conservative fidelity classification**

Classify T0 only when usable compile-unit and source-location metadata can
anchor definitions, T1 when stable named definitions or declarations remain,
and T2 otherwise. Preserve source-bearing metadata long enough to classify and
construct T0 anchors; path normalization must not remove semantic line or
column information.

- [ ] **Step 5: Verify malformed and valid inputs, then commit**

```bash
cmake --build --preset default --target BitcodeModuleLoaderTest
ctest --test-dir build -R BitcodeModuleLoaderTest --output-on-failure
git add src/analysis/ir_adapter src/analysis/CMakeLists.txt tests/integration/analysis/ir_adapter tests/integration/analysis/CMakeLists.txt
git commit -m "feat: load and classify external LLVM IR"
```

### Task 12: Acquire linked or separated IR with stable external contexts

**Files:**
- Create: `src/analysis/ir_adapter/ExternalIrManifest.h`
- Create: `src/analysis/ir_adapter/ExternalIrManifest.cpp`
- Create: `src/analysis/ir_adapter/BitcodeIrSource.h`
- Create: `src/analysis/ir_adapter/BitcodeIrSource.cpp`
- Modify: `src/analysis/ir_adapter/CMakeLists.txt`
- Modify: `src/analysis/llvm/IrArtifactStore.h`
- Modify: `src/analysis/llvm/IrArtifactStore.cpp`
- Modify: `src/analysis/AnalysisInputResolver.cpp`
- Create: `tests/integration/analysis/ir_adapter/BitcodeIrSourceTest.cpp`
- Modify: `tests/integration/analysis/ir_adapter/CMakeLists.txt`
- Modify: `tests/integration/analysis/ProjectAnalyzerTest.cpp`

**Interfaces:**
- Consumes: loaded external modules, the acquisition scheduler, managed
  snapshot sidecars, and the artifact store.
- Produces: `BitcodeSetIrSource`, `LinkedBitcodeIrSource`, verified managed
  snapshot loading, and anonymous content-addressed `AnalysisManifest` values.

- [ ] **Step 1: Write managed-round-trip and anonymous-input tests**

Add cases for one linked `.bc`, one textual `.ll`, and a directory containing
two translation-unit modules. Assert separated input is parsed in parallel but
linked in lexical order. Acquire a project snapshot, feed both its
`project.bc` and `tus/` directory back through `--bitcode`, and assert the
original program context and semantic IDs are recovered exactly.

Add rejection tests for a sidecar with an escaped path, a changed object
digest, a missing input-unit file, mixed T0/T2 input, and a wholly stripped
module.

- [ ] **Step 2: Run the focused tests and observe unsupported bitcode input**

```bash
cmake --build --preset default --target BitcodeIrSourceTest project_analyzer_integration_test
ctest --test-dir build -R "BitcodeIrSourceTest|ProjectAnalyzerTest" --output-on-failure
```

Expected: FAIL because managed sidecars and bitcode sources are not yet
resolved.

- [ ] **Step 3: Define anonymous external identity before annotation**

```cpp
StatusOr<AnalysisManifest> BuildAnonymousExternalManifest(
    std::span<const LoadedBitcodeModule> modules);
```

Hash each module's raw canonical bytes first. Derive repository, revision, and
build-variant identities through existing core ID constructors using the
ordered digest set, adapter schema, LLVM version, and aggregate fidelity.
Derive synthetic translation-unit identities from ordinal, logical name, and
raw module digest. Exclude host paths, mtimes, environment values, and
invocation IDs. Apply context-dependent VERITAS annotations only after this
manifest exists, then persist the annotated canonical object.

- [ ] **Step 4: Load and validate managed snapshot sidecars**

Add `IrArtifactStore::LoadManagedSnapshot(path)`. Discover `index.json` beside
`project.bc`, or in the parent of `tus/`. Validate schema version, output-root
confinement, unique ordinals and unit IDs, every referenced file digest, linked
digest, and manifest identity. Return `kFailedPrecondition` on any mismatch;
never silently fall back to anonymous identity when a sidecar is present but
invalid.

- [ ] **Step 5: Implement source variants and bitcode resolution**

```cpp
class BitcodeSetIrSource final : public ProgramIrSource {
 public:
  StatusOr<ProgramIr> Acquire(AnalysisEventSink& events) override;
};

class LinkedBitcodeIrSource final : public ProgramIrSource {
 public:
  StatusOr<ProgramIr> Acquire(AnalysisEventSink& events) override;
};
```

Use the bounded scheduler for separated-module parsing, order results by
ordinal, reject the complete acquisition if any unit is T2, detect multiple
strong `main` definitions, link serially, normalize, and publish the same
artifact layout used by project CodeGen. A linked file is one input unit but
still receives an input-unit object and snapshot descriptor. Aggregate
fidelity is the weakest accepted unit.

Replace the resolver's bitcode `kUnimplemented` branch with the appropriate
source, requiring an explicit output path and resolving jobs from discovered
module count.

- [ ] **Step 6: Verify identity round-trip and commit**

```bash
cmake --build --preset default --target BitcodeIrSourceTest project_analyzer_integration_test
ctest --test-dir build -R "BitcodeIrSourceTest|ProjectAnalyzerTest" --output-on-failure
git add src/analysis/ir_adapter src/analysis/llvm/IrArtifactStore.* src/analysis/AnalysisInputResolver.cpp tests/integration/analysis/ir_adapter tests/integration/analysis/ProjectAnalyzerTest.cpp
git commit -m "feat: analyze separated and linked LLVM IR"
```

### Task 13: Expose unified inputs, jobs, artifacts, and reports in the CLI

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Modify: `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`

**Interfaces:**
- Consumes: `AnalysisRequest`, project or bitcode input, output path, jobs,
  analysis configuration, and `ProjectAnalyzer::Analyze`.
- Produces: the two approved command forms, compact stdout, and actionable
  validation errors.

- [ ] **Step 1: Write end-to-end CLI contract tests**

Cover project mode with omitted output, project mode with explicit output and
`--jobs 2`, linked `.bc`, textual `.ll`, and a module directory. Add errors for
both input flags, neither input flag, bitcode without output, `--jobs 0`, a
non-numeric job count, missing paths, and an empty bitcode directory.

On success assert stdout and the filesystem expose the linked bitcode, report,
program ID, run ID, batch ID, and receipt ID. On forced downstream failure,
assert the run directory still contains all report files and ordinary logs do
not include source contents, environment values, or unrestricted compiler
arguments.

- [ ] **Step 2: Run the CLI target and observe the old parser behavior**

```bash
cmake --build --preset default --target VeritasBuildAnalyzeCliTest
ctest --test-dir build -R VeritasBuildAnalyzeCliTest --output-on-failure
```

Expected: FAIL because `--bitcode` and `--jobs` are not accepted and detailed
artifacts are not printed.

- [ ] **Step 3: Replace duplicate project setup with request construction**

Implement these usage forms:

```text
veritas-build analyze --project <dir> [--output <absolute-dir>] [--jobs auto|N]
veritas-build analyze --bitcode <file.bc|file.ll|directory> --output <absolute-dir> [--jobs auto|N]
```

Require exactly one input flag. Preserve existing analysis configuration
flags, reject retired unsupported modes, and validate output and job syntax
before analysis. Remove CLI calls to `ResolveProjectInput`,
`LoadProjectManifest`, and `WriteDiagnosticManifest`; construct one
`AnalysisRequest` and call `ProjectAnalyzer::Analyze`.

- [ ] **Step 4: Print a compact machine-readable completion summary**

Print one JSON object containing `status`, `input_kind`, `program_id`,
`analysis_run_id`, `ir_snapshot_id`, `linked_bitcode`, `report`,
`fact_batch_id`, and `publication_receipt_id`. Send human-actionable errors to
stderr and include the failed run report path when reporter creation
succeeded.

- [ ] **Step 5: Verify all CLI modes and commit**

```bash
cmake --build --preset default --target VeritasBuildAnalyzeCliTest veritas-build
ctest --test-dir build -R VeritasBuildAnalyzeCliTest --output-on-failure
git add src/tools/veritas-build.cpp tests/integration/build/VeritasBuildAnalyzeCliTest.cpp
git commit -m "feat: expose unified veritas-build inputs and reports"
```

### Task 14: Measure medium and large project acquisition performance

**Files:**
- Create: `tools/generate_analysis_benchmark_project.py`
- Create: `tools/benchmark_veritas_build.py`
- Create: `tests/qualification/VeritasBuildBenchmarkToolTest.py`
- Modify: `tests/qualification/CMakeLists.txt`
- Create: `docs/guides/veritas-build-performance-evaluation.md`

**Interfaces:**
- Consumes: `veritas-build`, generated compilation databases, emitted run
  reports, and managed LLVM snapshots.
- Produces: reproducible synthetic projects, `analysis-performance.v1` JSON,
  a checked-in Markdown evaluation, and evidence-backed speedup priorities.

- [ ] **Step 1: Write a qualification test for the tooling contract**

Generate an eight-unit project with one `main`, a shared header chain, and a
valid absolute-path `compile_commands.json`. Run one iteration of serial cold,
automatic cold, automatic warm, linked-bitcode, and TU-directory scenarios.
Assert the JSON schema, scenario names, measurement units, artifact paths, and
equal semantic digests across all five scenarios.

- [ ] **Step 2: Run the qualification test and observe missing scripts**

```bash
cmake --preset default
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL because the generator and benchmark runner do not exist.

- [ ] **Step 3: Implement the deterministic benchmark project generator**

Accept an output directory, translation-unit count, header-depth, and fixed
seed. Emit one main translation unit, `N-1` library translation units, bounded
cross-unit calls, a shared include chain, and normalized compile commands.
Write a corpus manifest with generator version and source-tree digest so runs
can prove they used the same inputs.

- [ ] **Step 4: Implement the scenario runner and report renderer**

For each scenario and iteration, allocate a distinct output directory and
capture wall time, process CPU time, peak resident set size, acquisition and
analysis stage timings, cache hits and misses, input and linked byte counts,
artifact-store growth, input-unit count, job count, and final semantic digest.
Read authoritative stage and cache values through `runs/latest.json` and the run
result files rather than parsing human logs.

Write `analysis-performance.v1` JSON first, then render Markdown solely from
that JSON. Record host CPU count, OS, compiler and LLVM versions, VERITAS
commit, scenario command templates, corpus digests, iteration count, median,
p95, and min/max. Do not encode pass/fail speed thresholds.

- [ ] **Step 5: Verify the tooling on the qualification fixture**

```bash
cmake --build --preset default --target veritas-build
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: PASS and all acquisition modes produce the same semantic digest.

- [ ] **Step 6: Run the approved performance matrix**

```bash
python3 tools/generate_analysis_benchmark_project.py --output build/benchmarks/m11-medium --translation-units 256 --header-depth 8 --seed 1101
python3 tools/generate_analysis_benchmark_project.py --output build/benchmarks/m11-large --translation-units 1024 --header-depth 16 --seed 1101
python3 tools/benchmark_veritas_build.py --binary build/bin/veritas-build --medium build/benchmarks/m11-medium --large build/benchmarks/m11-large --iterations 5 --json build/benchmarks/m11-results.json --markdown docs/guides/veritas-build-performance-evaluation.md
```

Review the generated report for cold serial versus cold auto scaling, warm
cache benefit, external-input acquisition cost, the serial SVF share, peak
memory, and artifact-store growth. Rank speedup proposals by measured share:
dependency-scan reuse, preamble reuse, process workers, incremental linking,
summary/WPA parallelism, or SVF isolation. State which proposals preserve the
current determinism and compatibility contract and which require a later
schema or architecture change.

- [ ] **Step 7: Commit the reproducible evaluation**

```bash
git add tools/generate_analysis_benchmark_project.py tools/benchmark_veritas_build.py tests/qualification/VeritasBuildBenchmarkToolTest.py tests/qualification/CMakeLists.txt docs/guides/veritas-build-performance-evaluation.md
git commit -m "perf: qualify unified analysis acquisition"
```

### Task 15: Publish the artifact contract and current user workflow

**Files:**
- Modify: `docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md`
- Modify: `docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md`
- Modify: `docs/architecture/01-platform-architecture.md`
- Modify: `docs/architecture/02-whole-program-analysis-architecture.md`
- Modify: `docs/architecture/03-summarydb-storage-architecture.md`
- Modify: `docs/guides/tutorial-build-summarydb-analysis-tool.md`
- Modify: `docs/guides/summarydb-generation-manual.md`
- Modify: `docs/guides/README.md`
- Modify: `docs/README.md`
- Modify: `docs/specs/milestones/README.md`
- Modify: `docs/plans/README.md`
- Modify: `README.md`
- Modify: `tests/integration/analysis/svf/RequiredSvfBoundaryTest.cpp`

**Interfaces:**
- Consumes: the implemented CLI, artifact schema, performance JSON, and
  compatibility behavior.
- Produces: one consistent description of project and external IR analysis,
  copy-pasteable examples, explicit fidelity rules, and verified public-header
  boundaries.

- [ ] **Step 1: Write documentation assertions and scan current claims**

Extend `RequiredSvfBoundaryTest` to recursively inspect installed public
headers and reject direct LLVM or Clang includes and native pointer/reference
types. Record every existing guide or milestone claim that says project input
is the only supported mode, bitcode is rejected, CodeGen is memory-only, or
analysis produces only SummaryDB output.

- [ ] **Step 2: Run the boundary test before documentation edits**

```bash
cmake --build --preset default --target required_svf_boundary_test
ctest --test-dir build -R RequiredSvfBoundaryTest --output-on-failure
```

Expected: PASS for the implementation, or FAIL with the exact public header
that must be made LLVM-neutral before publishing the feature.

- [ ] **Step 3: Update architecture and milestone contracts**

Document the `AnalysisRequest -> ProgramIrSource -> IrArtifactStore -> existing
analysis stages` flow, worker ownership, deterministic serial link, SVF mutex,
artifact tree, atomic selection rules, cache key, report schemas, and managed
sidecar round-trip. Reconcile M1 and M4 acceptance criteria with both input
modes while keeping repository-relative links and canonical ownership clear.

- [ ] **Step 4: Rewrite the user guides around actual commands and outputs**

Add complete examples for:

```bash
veritas-build analyze --project /absolute/path/to/project --output /absolute/path/to/summarydb --jobs auto
veritas-build analyze --bitcode /absolute/path/to/project.bc --output /absolute/path/to/summarydb --jobs 1
veritas-build analyze --bitcode /absolute/path/to/tu-bitcode --output /absolute/path/to/summarydb --jobs 8
```

Explain output discovery through `llvm/current.json` and `runs/latest.json`,
where detailed results, timings, diagnostics, and readable logs live, T0/T1/T2
behavior, safe cache reuse, failure reports, and how to run and interpret the
performance evaluation. Link the measured performance guide instead of
copying its values into multiple documents.

- [ ] **Step 5: Update navigation and status only after acceptance passes**

Update guide, documentation, milestone-spec, and milestone-plan indexes. Mark
M11 implemented only when all functional, determinism, fidelity, reporting,
performance-tooling, and documentation checks below pass; otherwise retain
the current status and state the remaining acceptance item.

- [ ] **Step 6: Run targeted and full verification**

```bash
cmake --build --preset default --target veritas-build
ctest --test-dir build -R "AnalysisRequestTest|AtomicFileTest|AnalysisRunReporterTest|IrArtifactStoreTest|IrModuleUtilitiesTest|DependencyHasherTest|CodeGenIrSourceTest|InputUnitSchedulerTest|ProjectAnalyzerTest|ProjectAnalyzerWpaTest|WpaOrchestratorTest|AnalysisFactBusTest|BitcodeModuleLoaderTest|BitcodeIrSourceTest|VeritasBuildAnalyzeCliTest|VeritasBuildBenchmarkToolTest|RequiredSvfBoundaryTest" --output-on-failure
cmake --build --preset default
ctest --preset default --output-on-failure
git diff --check
git status --short
```

Expected: all focused and full tests pass, documentation contains no stale
single-input claims, generated performance Markdown matches its JSON source,
and only the intended implementation and documentation files are changed.

- [ ] **Step 7: Commit the final documentation and status update**

```bash
git add README.md docs tests/integration/analysis/svf/RequiredSvfBoundaryTest.cpp
git commit -m "docs: publish M11 acquisition and performance guide"
```
