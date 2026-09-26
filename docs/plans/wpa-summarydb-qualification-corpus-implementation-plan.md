# WPA and SummaryDB Qualification Corpus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete production WPA orchestration and add a mixed C/C++ qualification corpus that proves SVF normalization, SummaryDB persistence, compiled Souffle recursion, and standard analyzer behavior for memory, alias, direct/indirect/virtual dispatch, and recursive calls.

**Architecture:** Persist each immutable WPA run and component result through a new `WpaRunRepository`, then have `WpaOrchestrator` evaluate reachability and memory effects in reverse SCC topological order using the existing engine-neutral materializer and canonicalizer. Qualify every semantic boundary with focused fixtures, then exercise their interactions in the mixed C11/C++20 `semantic_zoo` project and enforce exact CTest/CI entry-gate membership.

**Tech Stack:** C11, C++20, CMake 3.23+, Ninja, LLVM/Clang 22+, pinned SVF, Protobuf, RocksDB, SQLite, vendored Souffle 2.5 at `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, GoogleTest, Python 3.

**Spec:** `docs/specs/wpa-summarydb-qualification-corpus-design-spec.md`

## Global Constraints

- Production recursive domains remain exactly `ReachableCall` and `MayWrite`; `MayRead`, recursive alias, global flow, and soundness-coverage domains remain outside this plan.
- Compiled Souffle is the default production engine; C++ is only a conformance oracle or explicitly selected `cpp-emergency` engine. Automatic fallback is forbidden.
- The supported production toolchain pins Souffle release `2.5` at full revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` and runs generated programs with exactly one evaluation thread.
- Reuse requires exact `(LogicalInputHash, EngineToolchainIdentity)` equality and revalidation of the immutable cached result.
- Dense IDs never leave one execution input; persisted summaries, facts, witnesses, and cache objects contain stable IDs only.
- `summary.v1` remains readable and immutable; native analysis publishes `summary.v2`.
- Alias kind and epistemic state remain independent, including `NO_ALIAS + MUST` and `UNKNOWN_ALIAS + UNKNOWN`.
- Alias transport is qualified through normalized SVF facts, `summary.v2`, relation-schema validation, and relation-I/O round trips; alias rows are not injected into reachability or memory logical inputs.
- A failed component publishes no replacement, never invokes another engine implicitly, and leaves already-published summaries and CPG data valid.
- Every published result has a finite, acyclic witness rooted in declared local or successor facts.
- Fixture compilation uses C11 or C++20 with `-O0 -g -fno-inline`; C++ entries also use `-fno-rtti -fno-exceptions`.
- Fixture sources include no system headers and define no `main` function.
- Every new VERITAS-authored C, C++, header, CMake, Python, and shell file begins with the repository's complete Apache-2.0 header.
- Tests never use source line numbers, raw LLVM/SVF node IDs, dense tuple IDs, or tuple insertion order as semantic oracles.
- Determinism covers seeds `0` through `64`, lifecycle covers `20` complete analyses in one process, and performance covers `5` warmed iterations.
- The checked-in performance ceilings are `30000` ms wall time, `2048` MiB peak RSS, and `1000000` output facts for `recursive_calls`.
- The exact ten M9 criterion labels and five `wpa-qualification` aggregate names in the approved specification are authoritative.
- Implementation stays on `claude/wpa-summarydb-qualification-design` in `/Users/skg7on/Workspace/Projects/VERITAS/.claude/worktrees/wpa-summarydb-qualification-design`; the primary checkout remains clean on `main`.

## File and Responsibility Map

| Area | Files | Responsibility |
| --- | --- | --- |
| Durable WPA lifecycle | `include/veritas/wpa/WpaRunRepository.h`, `src/wpa/WpaRunRepository.cpp`, `src/summarydb/schema/v2.sql` | Immutable run manifests, mutable lifecycle state, component cache objects, exact-engine reuse, failure records |
| WPA scheduling | `include/veritas/wpa/WpaOrchestrator.h`, `src/wpa/WpaOrchestrator.cpp` | Call/SCC construction, expected component set, reverse-topological execution, canonicalization, publication |
| Standard analysis | `include/veritas/analysis/ProjectAnalyzer.h`, `src/analysis/ProjectAnalyzer.cpp`, `src/tools/veritas-build.cpp` | Explicit engine selection, limits, run identity, analyzer result fields, no-fallback CLI behavior |
| Test support | `tests/support/WpaFixtureHarness.h`, `tests/support/WpaFixtureHarness.cpp` | One-copy fixture analysis, normalized SVF snapshot, SummaryDB reload, stable semantic predicates |
| Focused corpus | `tests/fixtures/projects/{pointer_alias,abstract_memory,callback_dispatch,virtual_dispatch,recursive_calls,unknown_external}/` | Narrow memory, alias, dispatch, hierarchy, recursion, and unknown oracles |
| Integrated corpus | `tests/fixtures/projects/semantic_zoo/` | Mixed C/C++, multi-TU interaction coverage |
| Boundary tests | `tests/integration/analysis/SemanticZooSvfTest.cpp`, `SemanticZooSummaryDbTest.cpp` | Source-to-SVF and source-to-summary/SummaryDB assertions |
| End-to-end tests | `tests/integration/analysis/SemanticZooEndToEndTest.cpp`, `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp` | Standard analyzer, durable WPA run, recursive derived facts, reuse, emergency mode |
| Qualification | `tests/qualification/wpa/` | Differential, determinism, failure, migration/lifecycle, performance aggregates |
| M9 handoff | `include/veritas/facts/AnalysisFactBus.h`, `src/facts/AnalysisFactBus.cpp` | Validate and deliver one complete immutable fact batch with idempotent sink state |
| Gate and CI | `tests/qualification/check_no_skips.py`, `tools/check_m9_entry.py`, `.github/workflows/ci.yml` | Exact membership, skip rejection, pinned provenance, mandatory execution |

---

### Task 1: Persist WPA Run and Component Lifecycle

**Files:**

- Create: `include/veritas/wpa/WpaRunRepository.h`
- Create: `src/wpa/WpaRunRepository.cpp`
- Create: `src/summarydb/schema/v2.sql`
- Modify: `include/veritas/summarydb/MetadataStore.h:86-149`
- Modify: `src/summarydb/MetadataStore.cpp`
- Modify: `src/summarydb/CMakeLists.txt`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/WpaRunRepositoryTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**

- Consumes: `facts::AnalysisRunManifest`, `WpaComponentResult`, `summarydb::MetadataStore`, and `summarydb::ObjectStore`.
- Produces: `WpaRunStatus`, `WpaComponentStatus`, `WpaComponentKey`, `StoredWpaComponent`, and `WpaRunRepository`.
- Produces exact methods: `Open`, `BeginRun`, `LoadReusableComponent`, `StoreSuccessfulComponent`, `RecordComponentFailure`, `CompleteRun`, `MarkIncomplete`, `LoadRun`, and `ListComponents`.
- Preserves: existing V1 `wpa_sccs` and `wpa_component_states` tables as readable historical storage.

- [ ] **Step 1: Write the failing lifecycle and cache tests**

```cpp
TEST(WpaRunRepositoryTest, CompleteRequiresExactExpectedSet) {
  auto repository = OpenRepository();
  const auto run = ValidRun();
  ASSERT_TRUE(repository->BeginRun(run, TwoExpectedComponents()).ok());
  ASSERT_TRUE(repository->StoreSuccessfulComponent(
      run, FirstKey(), ValidResult(FirstKey())).ok());
  EXPECT_EQ(repository->CompleteRun(run.run_id).code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(repository->LoadRun(run.run_id)->status,
            WpaRunStatus::kIncomplete);
}

TEST(WpaRunRepositoryTest, ReuseRequiresExactToolchainAndValidObject) {
  auto repository = OpenRepository();
  ASSERT_TRUE(StoreOneSuccess(*repository, "souffle:toolchain-a").ok());
  EXPECT_TRUE(repository->LoadReusableComponent(
      CacheLookup("souffle:toolchain-a")).ok());
  EXPECT_EQ(repository->LoadReusableComponent(
      CacheLookup("souffle:toolchain-b")).status().code(),
      StatusCode::kNotFound);
  CorruptCachedObjectBytes();
  EXPECT_EQ(repository->LoadReusableComponent(
      CacheLookup("souffle:toolchain-a")).status().code(),
      StatusCode::kFailedPrecondition);
}

TEST(WpaRunRepositoryTest, FailedRunKeepsPriorSuccessAsHistory) {
  auto repository = OpenRepository();
  const auto prior = StoreCompleteRun(*repository, Revision(1));
  const auto current = BeginEquivalentRun(*repository, Revision(2));
  ASSERT_TRUE(repository->RecordComponentFailure(
      current.run_id, FirstKey(), Status::Internal("worker crashed")).ok());
  EXPECT_EQ(repository->LoadRun(current.run_id)->status,
            WpaRunStatus::kIncomplete);
  EXPECT_EQ(repository->LoadRun(prior.run_id)->status,
            WpaRunStatus::kComplete);
  EXPECT_EQ(repository->ListComponents(prior.run_id)->front().result,
            PriorResult());
}
```

- [ ] **Step 2: Build the focused test and verify it fails**

Run:

```bash
cmake --build --preset default --target WpaRunRepositoryTest
```

Expected: compilation fails because `WpaRunRepository.h` and its lifecycle types do not exist.

- [ ] **Step 3: Add schema V2 and the repository contract**

```cpp
enum class WpaRunStatus : std::uint8_t { kRunning, kComplete, kIncomplete };
enum class WpaComponentStatus : std::uint8_t {
  kPending, kRunning, kComplete, kFailed
};

struct WpaComponentKey {
  core::StableId scc_id;
  WpaComponentKind component;
  auto operator<=>(const WpaComponentKey&) const = default;
};

struct WpaReuseLookup {
  std::string logical_input_hash;
  std::string engine_toolchain_identity;
};

struct StoredWpaComponent {
  WpaComponentKey key;
  std::string result_object_key;
  WpaComponentResult result;
};

struct StoredWpaRun {
  facts::AnalysisRunManifest run;
  WpaRunStatus status;
  std::vector<WpaComponentKey> expected_components;
  std::vector<std::string> diagnostics;
};

class WpaRunRepository {
 public:
  static StatusOr<std::unique_ptr<WpaRunRepository>> Open(
      const std::filesystem::path& db_root);
  Status BeginRun(const facts::AnalysisRunManifest& run,
                  std::span<const WpaComponentKey> expected);
  StatusOr<StoredWpaComponent> LoadReusableComponent(
      const WpaReuseLookup& lookup) const;
  StatusOr<StoredWpaComponent> StoreSuccessfulComponent(
      const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
      const WpaComponentResult& result);
  Status RecordComponentFailure(core::StableId run_id,
                                const WpaComponentKey& key,
                                const Status& failure);
  Status CompleteRun(core::StableId run_id);
  Status MarkIncomplete(core::StableId run_id, std::string diagnostic);
  StatusOr<StoredWpaRun> LoadRun(core::StableId run_id) const;
  StatusOr<std::vector<StoredWpaComponent>> ListComponents(
      core::StableId run_id) const;
};
```

Create V2 tables `wpa_analysis_runs`, `wpa_expected_components_v2`,
`wpa_component_states_v2`, and `wpa_component_result_cache_v2`. Key the cache
only by the canonical bytes of `(logical_input_hash,
engine_toolchain_identity)`; the logical hash already commits to the SCC,
component, schemas, rules, models, mappings, roots, and EDB. Store the canonical
component bytes under their SHA-256 object key with `PutIfAbsent`. On every
cache load, verify the object digest, deserialize all stable IDs, confirm the
embedded key/hash against the current logical input, call `ValidateSemanticRow`
for every fact, and re-run witness closure before returning it. `CompleteRun`
executes one SQLite transaction that compares expected and completed key sets
for exact equality before updating the run status.

- [ ] **Step 4: Run the repository tests**

Run:

```bash
cmake --build --preset default --target WpaRunRepositoryTest
./build/bin/WpaRunRepositoryTest
```

Expected: idempotent begin/store, exact completion, exact-engine reuse,
corruption rejection, and prior-success retention all pass.

- [ ] **Step 5: Commit the lifecycle repository**

```bash
git add include/veritas/wpa/WpaRunRepository.h src/wpa/WpaRunRepository.cpp src/summarydb/schema/v2.sql include/veritas/summarydb/MetadataStore.h src/summarydb/MetadataStore.cpp src/summarydb/CMakeLists.txt src/wpa/CMakeLists.txt tests/unit/wpa/WpaRunRepositoryTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat: persist WPA run lifecycle"
```

### Task 2: Execute Components in Reverse SCC Order

**Files:**

- Create: `include/veritas/wpa/WpaOrchestrator.h`
- Create: `src/wpa/WpaOrchestrator.cpp`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/WpaOrchestratorTest.cpp`
- Modify: `tests/unit/wpa/WpaCoordinatorTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**

- Consumes: `CallGraph::FromSummaries`, `SccGraph::Build`, `SccGraph::ReverseTopologicalOrder`, `WpaInputMaterializer::Build`, `WpaExecutor::Execute`, `facts::ResultCanonicalizer::Canonicalize`, and Task 1's repository.
- Produces: `WpaRunRequest`, `WpaComponentCompletion`, `WpaRunResult`, and `WpaOrchestrator::Run`.
- Guarantees: the frozen expected set is `|SCCs| * 2`, successor support is component-matched, and any non-OK execution leaves the current run incomplete.

- [ ] **Step 1: Write the failing scheduling, reuse, and failure tests**

```cpp
TEST(WpaOrchestratorTest, ExecutesSuccessorsBeforePredecessorsForBothDomains) {
  RecordingExecutor executor;
  auto result = MakeOrchestrator(executor).Run(ThreeSccRequest());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(executor.keys(), ExpectedReverseTopologicalKeys());
  EXPECT_EQ(result->expected_components.size(), 6u);
  EXPECT_EQ(result->completed_components.size(), 6u);
}

TEST(WpaOrchestratorTest, SuccessorFactsAreSupportNotLocalOwnership) {
  RecordingMaterializer observer;
  ASSERT_TRUE(MakeOrchestrator(observer).Run(CallerCalleeRequest()).ok());
  EXPECT_TRUE(observer.PredecessorHas(RelationId::kSupportReachableCall));
  EXPECT_TRUE(observer.PredecessorHas(RelationId::kSupportMayWrite));
  EXPECT_FALSE(observer.PredecessorClaimsSuccessorRootsAsLocal());
}

TEST(WpaOrchestratorTest, CacheHitSkipsExecutionButRevalidatesResult) {
  CountingExecutor executor;
  ASSERT_TRUE(MakeOrchestrator(executor).Run(RevisionRequest(1)).ok());
  ASSERT_TRUE(MakeOrchestrator(executor).Run(RevisionRequest(2)).ok());
  EXPECT_EQ(executor.invocations(), ExpectedComponentCount());
  EXPECT_NE(RunId(1), RunId(2));
  EXPECT_EQ(ResultObjectKeys(1), ResultObjectKeys(2));
}

TEST(WpaOrchestratorTest, ExecutorFailurePublishesNoCurrentComponent) {
  FailOnKeyExecutor executor(FailingKey());
  auto result = MakeOrchestrator(executor).Run(ThreeSccRequest());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(CurrentRunStatus(), WpaRunStatus::kIncomplete);
  EXPECT_FALSE(CurrentComponent(FailingKey()).has_value());
  EXPECT_EQ(executor.fallback_invocations(), 0u);
}
```

- [ ] **Step 2: Build and verify the test fails**

Run:

```bash
cmake --build --preset default --target WpaOrchestratorTest WpaCoordinatorTest
```

Expected: compilation fails because the orchestrator contract is absent.

- [ ] **Step 3: Implement the orchestration contract**

```cpp
struct WpaRunRequest {
  facts::AnalysisRunManifest run;
  std::span<const summary::SummaryArtifact> summaries;
  const analysis::semantic::ModelBundle* models = nullptr;
  std::array<WpaComponentKind, 2> components{
      WpaComponentKind::kReachability,
      WpaComponentKind::kMemoryEffects};
  WpaExecutionLimits limits;
};

struct WpaComponentCompletion {
  WpaComponentKey key;
  std::string result_object_key;
  WpaComponentResult result;
};

struct WpaRunResult {
  facts::AnalysisRunManifest run;
  std::vector<WpaComponentKey> expected_components;
  std::vector<WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
};

class WpaOrchestrator {
 public:
  WpaOrchestrator(WpaExecutor& executor, WpaRunRepository& repository);
  StatusOr<WpaRunResult> Run(const WpaRunRequest& request);
};
```

Build the call graph once, build the SCC graph once, freeze keys sorted by
component then reverse-topological SCC order, and call `BeginRun`. For one key,
collect only already-completed results of `SccGraph::Successors(scc_id)` for
the same component, materialize once, try exact reuse, execute on a miss, and
canonicalize with `local_roots` and `successor_roots`. Construct
`WpaComponentResult` from the canonical facts, witnesses, hashes, and
diagnostics; publish it only through `StoreSuccessfulComponent`. On any
failure, call `RecordComponentFailure`, call `MarkIncomplete`, and return the
original status without invoking another executor.

- [ ] **Step 4: Run focused orchestration tests**

Run:

```bash
cmake --build --preset default --target WpaOrchestratorTest WpaCoordinatorTest
./build/bin/WpaOrchestratorTest
./build/bin/WpaCoordinatorTest
```

Expected: reverse ordering, support ownership, reuse, no fallback, canonical
witnesses, and incomplete-run behavior pass.

- [ ] **Step 5: Commit orchestration**

```bash
git add include/veritas/wpa/WpaOrchestrator.h src/wpa/WpaOrchestrator.cpp src/wpa/CMakeLists.txt tests/unit/wpa/WpaOrchestratorTest.cpp tests/unit/wpa/WpaCoordinatorTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat: orchestrate recursive WPA components"
```

### Task 3: Integrate WPA into Standard Analysis and CLI

**Files:**

- Modify: `include/veritas/analysis/ProjectAnalyzer.h:35-70`
- Modify: `src/analysis/ProjectAnalyzer.cpp:47-176`
- Modify: `src/analysis/ProjectAnalyzerInternal.h`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `src/tools/veritas-build.cpp:50-200`
- Modify: `src/tools/CMakeLists.txt`
- Create: `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp`
- Create: `tests/integration/analysis/WpaEmergencyModeTest.cpp`
- Modify: `tests/integration/analysis/ProjectAnalyzerTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`

**Interfaces:**

- Adds: `WpaEngineMode::{kSouffle,kCppEmergency}` and WPA fields to `AnalysisConfig`.
- Adds: `wpa_run_id`, `wpa_engine`, `wpa_expected_component_count`, `wpa_completed_component_count`, and `wpa_diagnostics` to `ProjectAnalysisResult`.
- Adds CLI: `--wpa-engine=souffle|cpp-emergency`; there is no `auto` value.
- Executes: WPA only after summary and thin-CPG publication succeeds.

- [ ] **Step 1: Write the failing default, failure, and emergency tests**

```cpp
TEST(ProjectAnalyzerWpaTest, DefaultPublishesCompleteSouffleRun) {
  EXPECT_EQ(AnalysisConfig::Default().wpa_engine, WpaEngineMode::kSouffle);
  auto result = AnalyzeFixture("multiple_tus", AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->wpa_run_id.empty());
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kSouffle);
  EXPECT_GT(result->wpa_expected_component_count, 0u);
  EXPECT_EQ(result->wpa_completed_component_count,
            result->wpa_expected_component_count);
  EXPECT_TRUE(result->wpa_diagnostics.empty());
}

TEST(ProjectAnalyzerWpaTest, SouffleFailureNeverInvokesCpp) {
  RecordingExecutor cpp;
  auto analyzer = AnalyzerWithExecutors(FailingSouffle(), cpp);
  auto result = analyzer.AnalyzeProject(FixtureRequest("multiple_tus"),
                                        AnalysisConfig::Default());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(cpp.invocation_count(), 0u);
  EXPECT_TRUE(SummariesRemainReadable());
  EXPECT_EQ(PersistedRunStatus(), WpaRunStatus::kIncomplete);
}

TEST(ProjectAnalyzerWpaTest, ConformanceOracleUsesASeparateNonProductionRun) {
  auto config = AnalysisConfig::Default();
  config.run_cpp_conformance_oracle = true;
  auto result = AnalyzeFixture("multiple_tus", config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kSouffle);
  EXPECT_NE(result->wpa_run_id, ConformanceRunIdForFixture());
  EXPECT_EQ(ProductionFacts(), ConformanceFacts());
  EXPECT_EQ(ProductionLogicalInputBytes(), ConformanceLogicalInputBytes());
}

TEST(WpaEmergencyModeTest, ExplicitCppModeIsDistinctAndDegraded) {
  auto config = AnalysisConfig::Default();
  config.wpa_engine = WpaEngineMode::kCppEmergency;
  auto result = AnalyzeFixture("multiple_tus", config);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kCppEmergency);
  EXPECT_NE(result->wpa_run_id, ProductionRunIdForFixture());
  EXPECT_THAT(result->wpa_diagnostics,
              Contains("degraded-operation:cpp-emergency"));
}
```

- [ ] **Step 2: Build and verify the tests fail**

Run:

```bash
cmake --build --preset default --target ProjectAnalyzerWpaTest WpaEmergencyModeTest ProjectAnalyzerTest
```

Expected: compilation fails because analyzer WPA fields and test injection do
not exist.

- [ ] **Step 3: Add exact engine configuration and result fields**

```cpp
enum class WpaEngineMode : std::uint8_t { kSouffle, kCppEmergency };

struct AnalysisConfig {
  std::chrono::seconds svf_soft_analysis_budget;
  std::size_t svf_max_graph_nodes;
  std::size_t svf_max_emitted_facts;
  WpaEngineMode wpa_engine = WpaEngineMode::kSouffle;
  std::chrono::milliseconds wpa_component_timeout{30000};
  std::uint64_t wpa_component_memory_mb = 2048;
  std::uint32_t wpa_threads = 1;
  bool run_cpp_conformance_oracle = false;
  static AnalysisConfig Default();
};
```

After `ProjectPublicationCoordinator::Publish` returns, reopen the exact output
root, call `ListCurrentSummaryArtifacts(revision_id, build_variant_id)`, build
an engine-specific `AnalysisRunManifest`, and call `WpaOrchestrator::Run`.
Souffle uses the generated worker path and the generated provenance manifest;
`cpp-emergency` constructs `CppConformanceExecutor::Create(kCppEmergency)` and
does not read Souffle provenance. Reject `wpa_threads != 1`, an unknown CLI
engine, or conformance-oracle mode combined with emergency mode. Do not catch a
Souffle status to retry with C++. When `run_cpp_conformance_oracle` is true,
run the C++ executor under a distinct `kCppConformance` manifest using the
already-materialized logical bytes, persist that run separately, compare its
canonical facts with production, and keep `ProjectAnalysisResult::wpa_run_id`
bound to the production Souffle run.

- [ ] **Step 4: Run analyzer and CLI tests**

Run:

```bash
cmake --build --preset default --target ProjectAnalyzerWpaTest WpaEmergencyModeTest ProjectAnalyzerTest veritas-build
./build/bin/ProjectAnalyzerWpaTest
./build/bin/WpaEmergencyModeTest
./build/bin/ProjectAnalyzerTest
./build/bin/veritas-build analyze --help
```

Expected: default production, injected failure, explicit emergency, invalid
CLI value, component counts, and post-publication SummaryDB retention pass.

- [ ] **Step 5: Commit standard analyzer integration**

```bash
git add include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp src/analysis/ProjectAnalyzerInternal.h src/analysis/CMakeLists.txt src/tools/veritas-build.cpp src/tools/CMakeLists.txt tests/integration/analysis/ProjectAnalyzerWpaTest.cpp tests/integration/analysis/WpaEmergencyModeTest.cpp tests/integration/analysis/ProjectAnalyzerTest.cpp tests/integration/analysis/CMakeLists.txt
git commit -m "feat: run WPA in standard project analysis"
```

### Task 4: Add a Shared Semantic Fixture Harness

**Files:**

- Create: `tests/support/WpaFixtureHarness.h`
- Create: `tests/support/WpaFixtureHarness.cpp`
- Modify: `tests/support/CMakeLists.txt`
- Create: `tests/support/WpaFixtureHarnessTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Consumes: existing `testing::FixtureProject`, local-analysis/SVF stages, `ProjectAnalyzer`, and `SummaryRepository`.
- Produces: `MapFixtureWithSvf`, `AnalyzeAndLoadFixture`, and stable predicate helpers used by Tasks 5-11.
- Ensures: a fixture is copied exactly once per snapshot; analyzer and repository paths always refer to that same copy.

- [ ] **Step 1: Write the failing harness self-test**

```cpp
TEST(WpaFixtureHarnessTest, AnalysisAndReloadShareOneFixtureCopy) {
  auto snapshot = AnalyzeAndLoadFixture("function_pointer",
                                        AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_EQ(snapshot->project_root / ".veritas", snapshot->output_root);
  EXPECT_FALSE(snapshot->analysis.published_summary_ids.empty());
  EXPECT_EQ(snapshot->summaries.size(),
            snapshot->analysis.published_summary_ids.size());
  EXPECT_TRUE(AllArtifactsAreV2(snapshot->summaries));
}
```

- [ ] **Step 2: Build and verify the test fails**

Run:

```bash
cmake --build --preset default --target WpaFixtureHarnessTest
```

Expected: compilation fails because the harness types are absent.

- [ ] **Step 3: Implement the harness contract**

```cpp
struct SvfFixtureSnapshot {
  std::filesystem::path project_root;
  analysis::svf::SvfMappingResult mapping;
};

struct AnalyzedFixtureSnapshot {
  std::filesystem::path project_root;
  std::filesystem::path output_root;
  analysis::ProjectAnalysisResult analysis;
  std::vector<summary::SummaryArtifact> summaries;
};

StatusOr<SvfFixtureSnapshot> MapFixtureWithSvf(
    std::string_view name,
    const analysis::svf::SvfConfig& config =
        analysis::svf::SvfConfig::Default());

StatusOr<AnalyzedFixtureSnapshot> AnalyzeAndLoadFixture(
    std::string_view name, const analysis::AnalysisConfig& config);

bool AllArtifactsAreV2(std::span<const summary::SummaryArtifact> artifacts);
std::vector<const summary::v2::Call*> CallsWithDispatch(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::DispatchKind dispatch);
std::vector<const summary::v2::MemoryEffect*> MemoryEffectsWithObjectKind(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::AbstractObjectKind kind);
```

`MapFixtureWithSvf` performs `ResolveProjectInput`, `LoadProjectManifest`,
`RunLocalAnalysis`, and one `SvfAnalysisStage::Analyze` with a provenance
context derived from the manifest and module hash. `AnalyzeAndLoadFixture`
copies once, runs `ProjectAnalyzer`, opens the same `.veritas` root, and lists
artifacts under the returned revision/build coordinates. Predicates inspect
semantic enums and stable-ID prefixes; diagnostic names may select an expected
source scenario but never establish identity equality.

Add a dedicated `veritas_wpa_fixture_support` static library containing the
two new source files. Give it private include access to `${PROJECT_SOURCE_DIR}/src`
and link `veritas_analysis`, `veritas_svf_analysis`,
`veritas_analysis_pipeline`, `veritas_build`, `veritas_summarydb`, and
`veritas_test_support`. Link the self-test and all later semantic/qualification
targets to this library so the base `veritas_test_support` target remains
lightweight.

- [ ] **Step 4: Run the harness test**

Run:

```bash
cmake --build --preset default --target WpaFixtureHarnessTest
./build/bin/WpaFixtureHarnessTest
```

Expected: one-copy analysis, V2 reload, and helper predicates pass.

- [ ] **Step 5: Commit the harness**

```bash
git add tests/support/WpaFixtureHarness.h tests/support/WpaFixtureHarness.cpp tests/support/WpaFixtureHarnessTest.cpp tests/support/CMakeLists.txt tests/CMakeLists.txt
git commit -m "test: add WPA fixture harness"
```

### Task 5: Add Focused Memory and Alias Fixtures

**Files:**

- Create: `tests/fixtures/projects/pointer_alias/compile_commands.json`
- Create: `tests/fixtures/projects/pointer_alias/pointer_alias.c`
- Create: `tests/fixtures/projects/abstract_memory/compile_commands.json`
- Create: `tests/fixtures/projects/abstract_memory/abstract_memory.c`
- Create: `tests/integration/analysis/SemanticMemorySvfTest.cpp`
- Create: `tests/integration/analysis/SemanticMemorySummaryDbTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/unit/facts/RelationSchemaTest.cpp`
- Modify: `tests/unit/wpa/RelationIoTest.cpp`

**Interfaces:**

- Produces C symbols: `alias_copy`, `alias_disjoint`, `alias_parameters`, `alias_indirect`, `memory_global`, `memory_nested`, `memory_constant_index`, `memory_variable_index`, `memory_overlap`, and `memory_zero_range`.
- Qualifies source-owned `MustAlias`, `MayAlias`, and `NoAlias`; qualifies `UnknownAlias` by synthetic normalized-fact/summary/relation round trip.
- Qualifies global, stack, argument, subobject, array, overlap, unknown, and known-zero memory representations.

- [ ] **Step 1: Add tests that fail because fixtures are missing**

```cpp
TEST(SemanticMemorySvfTest, SourceCoversMustMayAndNoAlias) {
  auto snapshot = MapFixtureWithSvf("pointer_alias");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, AliasKind::kMustAlias,
                       EpistemicState::kMust));
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, AliasKind::kMayAlias,
                       EpistemicState::kMay));
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, AliasKind::kNoAlias,
                       EpistemicState::kMust));
}

TEST(SemanticMemorySvfTest, FactBudgetCreatesScopedExplicitUnknown) {
  auto config = analysis::svf::SvfConfig::Default();
  config.max_emitted_facts = 1;
  auto snapshot = MapFixtureWithSvf("pointer_alias", config);
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(snapshot->mapping.completion,
            SvfMappingCompletion::kCompleteWithUnknowns);
  EXPECT_TRUE(HasScopedBudgetUnknown(snapshot->mapping.facts,
                                     "max_emitted_facts"));
}

TEST(SemanticMemorySummaryDbTest, StructuredMemorySurvivesV2Persistence) {
  auto snapshot = AnalyzeAndLoadFixture("abstract_memory",
                                        AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_TRUE(HasObjectKinds(snapshot->summaries,
      {ABSTRACT_OBJECT_KIND_GLOBAL, ABSTRACT_OBJECT_KIND_STACK,
       ABSTRACT_OBJECT_KIND_ARGUMENT}));
  EXPECT_TRUE(HasFieldAndArrayAccessPaths(snapshot->summaries));
  EXPECT_TRUE(HasKnownZeroRange(snapshot->summaries));
  EXPECT_TRUE(HasCanonicalUnknownRange(snapshot->summaries));
}

TEST(RelationSchemaTest, UnknownAliasRoundTripsWithoutChangingEpistemic) {
  const auto row = SyntheticAliasRow(AliasKind::kUnknownAlias,
                                     EpistemicState::kUnknown);
  ASSERT_TRUE(ValidateSemanticRow(row).ok());
  EXPECT_EQ(RoundTripAliasThroughRelationIo(row), row);
}
```

- [ ] **Step 2: Build and verify the fixture tests fail**

Run:

```bash
cmake --build --preset default --target SemanticMemorySvfTest SemanticMemorySummaryDbTest RelationSchemaTest RelationIoTest
```

Expected: the fixture harness reports unknown projects `pointer_alias` and
`abstract_memory`.

- [ ] **Step 3: Add the exact C fixture operations**

```c
int alias_copy(int *p) { int *q = p; *q = 7; return *p; }
int alias_disjoint(void) { int a = 1; int b = 2; return a + b; }
int alias_parameters(int *left, int *right) {
  *left = 3;
  return *right;
}
int alias_indirect(int **slot, int *value) {
  *slot = value;
  **slot = 11;
  return *value;
}
```

```c
int memory_global_value;
static int memory_static_value;
union MemoryOverlap { int whole; unsigned char bytes[4]; };
struct MemoryInner { int values[4]; };
struct MemoryOuter { int tag; struct MemoryInner inner; };

void memory_global(void) {
  memory_global_value = 1;
  memory_static_value = 2;
}
int memory_nested(struct MemoryOuter *p) { return p->inner.values[2]; }
int memory_constant_index(int *p) { return p[3]; }
int memory_variable_index(int *p, int index) { return p[index]; }
int memory_overlap(union MemoryOverlap *p) {
  p->whole = 0;
  return p->bytes[0];
}
int memory_zero_range(struct MemoryOuter *p) { return p->tag; }
```

Each `compile_commands.json` has one C11 entry with directory
`@PROJECT_ROOT@` and command
`clang -std=c11 -O0 -g -fno-inline -c @PROJECT_ROOT@/<file>.c -o <file>.o`.

- [ ] **Step 4: Add stable semantic assertions and run them**

The tests must compare distinct `memory_location_id` and
`abstract_object_id` values, inspect structural path segment kinds, prove the
byte range is excluded from memory identity by rebuilding the same location
with two ranges, and prove a known `(offset=0,size=0)` differs from both-known-
false. For may-alias parameters, accept a stronger result only if both pointers
resolve to one proven object; never convert an unknown result to a positive
alias.

Run:

```bash
cmake --build --preset default --target SemanticMemorySvfTest SemanticMemorySummaryDbTest RelationSchemaTest RelationIoTest
./build/bin/SemanticMemorySvfTest
./build/bin/SemanticMemorySummaryDbTest
./build/bin/RelationSchemaTest
./build/bin/RelationIoTest
```

Expected: all memory kinds, access paths, range states, three source alias
kinds, synthetic unknown alias, and stable-ID distinctness pass.

- [ ] **Step 5: Commit focused memory qualification**

```bash
git add tests/fixtures/projects/pointer_alias tests/fixtures/projects/abstract_memory tests/integration/analysis/SemanticMemorySvfTest.cpp tests/integration/analysis/SemanticMemorySummaryDbTest.cpp tests/integration/analysis/CMakeLists.txt tests/unit/facts/RelationSchemaTest.cpp tests/unit/wpa/RelationIoTest.cpp
git commit -m "test: qualify memory and alias semantics"
```

### Task 6: Add Focused Dispatch, Hierarchy, Recursion, and Unknown Fixtures

**Files:**

- Create: `tests/fixtures/projects/callback_dispatch/compile_commands.json`
- Create: `tests/fixtures/projects/callback_dispatch/callback_dispatch.c`
- Create: `tests/fixtures/projects/virtual_dispatch/compile_commands.json`
- Create: `tests/fixtures/projects/virtual_dispatch/virtual_dispatch.cpp`
- Create: `tests/fixtures/projects/recursive_calls/compile_commands.json`
- Create: `tests/fixtures/projects/recursive_calls/recursive_calls.cpp`
- Create: `tests/fixtures/projects/unknown_external/compile_commands.json`
- Create: `tests/fixtures/projects/unknown_external/unknown_external.c`
- Create: `tests/integration/analysis/SemanticDispatchSvfTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/unit/wpa/CallGraphTest.cpp`
- Modify: `tests/unit/wpa/SccGraphTest.cpp`

**Interfaces:**

- Produces C callback paths through one global pointer, one formal callback parameter, and one two-entry table.
- Produces a C++ hierarchy with single inheritance, multiple inheritance, two overrides, a direct nonvirtual method, and a base-pointer virtual selector.
- Produces one self-recursive SCC, one two-member recursive SCC, and a recursive path to a writing leaf.
- Produces one unresolved external call that remains scoped and does not fan out.

- [ ] **Step 1: Add failing dispatch and SCC tests**

```cpp
TEST(SemanticDispatchSvfTest, DistinguishesDirectIndirectCallbackAndVirtual) {
  auto callbacks = MapFixtureWithSvf("callback_dispatch");
  auto virtuals = MapFixtureWithSvf("virtual_dispatch");
  ASSERT_TRUE(callbacks.ok());
  ASSERT_TRUE(virtuals.ok());
  EXPECT_TRUE(HasDispatch(callbacks->mapping.facts, DispatchKind::kDirect));
  EXPECT_TRUE(HasDispatch(callbacks->mapping.facts, DispatchKind::kIndirect));
  EXPECT_TRUE(HasDispatch(callbacks->mapping.facts, DispatchKind::kCallback));
  EXPECT_TRUE(HasTwoStableMayTargetsAtOneSite(callbacks->mapping.facts));
  EXPECT_TRUE(HasDispatch(virtuals->mapping.facts, DispatchKind::kVirtual));
  EXPECT_TRUE(HasTwoStableMayTargetsAtOneSite(virtuals->mapping.facts));
}

TEST(SccGraphTest, FocusedRecursionHasExpectedSccShapes) {
  auto artifacts = AnalyzeAndLoadFixture("recursive_calls",
                                         AnalysisConfig::Default())->summaries;
  auto graph = CallGraph::FromSummaries(artifacts);
  ASSERT_TRUE(graph.ok());
  auto sccs = SccGraph::Build(*graph);
  ASSERT_TRUE(sccs.ok());
  EXPECT_TRUE(ContainsRecursiveSingleton(*graph, *sccs));
  EXPECT_TRUE(ContainsSccWithMemberCount(*sccs, 2));
}

TEST(CallGraphTest, UnknownExternalDoesNotFanOut) {
  auto graph = GraphForFixture("unknown_external");
  ASSERT_TRUE(graph.ok());
  EXPECT_EQ(UnknownCallCount(*graph), 1u);
  EXPECT_EQ(ResolvedEdgesAtUnknownSite(*graph), 0u);
}
```

- [ ] **Step 2: Build and verify the tests fail**

Run:

```bash
cmake --build --preset default --target SemanticDispatchSvfTest CallGraphTest SccGraphTest
```

Expected: fixture lookup fails for the four new project names.

- [ ] **Step 3: Add callback, recursion, and unknown C/C++ sources**

```c
typedef void (*DispatchCallback)(int *);
static void callback_left(int *p) { p[0] = 1; }
static void callback_right(int *p) { p[1] = 2; }
DispatchCallback callback_global = callback_left;
DispatchCallback callback_table[2] = {callback_left, callback_right};
void callback_direct(int *p) { callback_left(p); }
void callback_indirect(int *p) { callback_global(p); }
void callback_parameter(DispatchCallback cb, int *p) { cb(p); }
void callback_select(int choose, int *p) { callback_table[choose != 0](p); }
```

```cpp
struct DispatchBase { virtual void write(int*) = 0; };
struct DispatchTag { virtual int tag() const = 0; };
struct DispatchLeft final : DispatchBase {
  void write(int* p) override { p[0] = 1; }
};
struct DispatchRight final : DispatchBase, DispatchTag {
  void write(int* p) override { p[1] = 2; }
  int tag() const override { return 2; }
  void direct(int* p) { p[2] = 3; }
};
void virtual_one(DispatchBase* value, int* p) { value->write(p); }
void virtual_two(bool choose, int* p) {
  DispatchLeft left;
  DispatchRight right;
  DispatchBase* value = choose ? static_cast<DispatchBase*>(&left)
                               : static_cast<DispatchBase*>(&right);
  value->write(p);
}
void nonvirtual(DispatchRight* value, int* p) { value->direct(p); }
```

```cpp
void recursive_leaf(int* p) { p[0] = 9; }
int recursive_self(int n, int* p) {
  if (n == 0) { recursive_leaf(p); return 0; }
  return recursive_self(n - 1, p);
}
int recursive_even(int n, int* p);
int recursive_odd(int n, int* p) {
  return n == 0 ? 0 : recursive_even(n - 1, p);
}
int recursive_even(int n, int* p) {
  if (n == 0) { recursive_leaf(p); return 1; }
  return recursive_odd(n - 1, p);
}
```

```c
extern int unresolved_vendor_call(int *);
int unknown_external_entry(int *p) { return unresolved_vendor_call(p); }
```

Use C11 for the C files and C++20 plus `-fno-rtti -fno-exceptions` for C++.
All entries also carry `-O0 -g -fno-inline` and replace `@PROJECT_ROOT@`.

- [ ] **Step 4: Implement callback classification at the stable origin boundary**

Modify `src/analysis/svf/SvfFactMapper.cpp` only if the failing test confirms
the current mapper reports a formal callback call as ordinary indirect. The
classification rule is exact: a call operand whose stable LLVM origin is a
function-pointer formal argument maps to `kCallback`; global variables, local
selected pointers, and tables remain `kIndirect`; `CallICFGNode::isVirtualCall`
remains `kVirtual`. Do not inspect diagnostic display names.

Run:

```bash
cmake --build --preset default --target SemanticDispatchSvfTest CallGraphTest SccGraphTest
./build/bin/SemanticDispatchSvfTest
./build/bin/CallGraphTest
./build/bin/SccGraphTest
```

Expected: dispatch kinds, stable MAY target sets, recursive SCC shapes, and
scoped unknown behavior pass.

- [ ] **Step 5: Commit focused call qualification**

```bash
git add tests/fixtures/projects/callback_dispatch tests/fixtures/projects/virtual_dispatch tests/fixtures/projects/recursive_calls tests/fixtures/projects/unknown_external tests/integration/analysis/SemanticDispatchSvfTest.cpp tests/integration/analysis/CMakeLists.txt tests/unit/wpa/CallGraphTest.cpp tests/unit/wpa/SccGraphTest.cpp src/analysis/svf/SvfFactMapper.cpp
git commit -m "test: qualify dispatch and recursive calls"
```

If callback classification already passes, omit `src/analysis/svf/SvfFactMapper.cpp`
from the commit rather than creating a no-op change.

### Task 7: Add the Mixed C/C++ `semantic_zoo` Source Project

**Files:**

- Create: `tests/fixtures/projects/semantic_zoo/compile_commands.json`
- Create: `tests/fixtures/projects/semantic_zoo/semantic_zoo.h`
- Create: `tests/fixtures/projects/semantic_zoo/c_memory.c`
- Create: `tests/fixtures/projects/semantic_zoo/c_callbacks.c`
- Create: `tests/fixtures/projects/semantic_zoo/cpp_memory.cpp`
- Create: `tests/fixtures/projects/semantic_zoo/cpp_dispatch.cpp`
- Create: `tests/fixtures/projects/semantic_zoo/recursion.cpp`
- Create: `tests/fixtures/projects/semantic_zoo/driver.cpp`
- Create: `tests/integration/analysis/SemanticZooFixtureTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`

**Interfaces:**

- Produces the C ABI `ZooPayload`, `ZooEnvelope`, `ZooBuffer`, `ZooCallback`, and wrapper entry points declared in `semantic_zoo.h`.
- Produces six linked translation units: two C11 and four C++20.
- Integrates: structured memory, callbacks, allocation/model effects, virtual dispatch, recursion, and cross-TU calls under `zoo_driver`.

- [ ] **Step 1: Write the failing mixed-language fixture test**

```cpp
TEST(SemanticZooFixtureTest, LinksTwoCAndFourCppTranslationUnits) {
  const auto root = testing::FixtureProject("semantic_zoo");
  auto manifest = LoadFixtureManifest(root);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  EXPECT_EQ(manifest->translation_units.size(), 6u);
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  EXPECT_NE(local->program_ir.GetFunction("zoo_driver"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_callback_parameter"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_virtual_select"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_recursive_entry"), nullptr);
}
```

- [ ] **Step 2: Build and verify the test fails**

Run:

```bash
cmake --build --preset default --target SemanticZooFixtureTest
```

Expected: fixture lookup fails because `semantic_zoo` does not exist.

- [ ] **Step 3: Add the shared ABI and translation-unit responsibilities**

```c
typedef unsigned long ZooSize;
typedef struct ZooPayload { unsigned char bytes[16]; ZooSize length; } ZooPayload;
typedef struct ZooEnvelope { int tag; ZooPayload payload; } ZooEnvelope;
typedef struct ZooBuffer { unsigned char data[32]; ZooSize capacity; ZooSize length; } ZooBuffer;
typedef void (*ZooCallback)(ZooBuffer*, int);

void zoo_callback_direct(ZooBuffer*, int);
void zoo_callback_indirect(ZooBuffer*, int);
void zoo_callback_parameter(ZooCallback, ZooBuffer*, int);
void zoo_callback_select(int, ZooBuffer*, int);
void zoo_modeled_copy(ZooBuffer*, const ZooEnvelope*);
void zoo_virtual_select(int, ZooBuffer*);
void zoo_recursive_entry(int, ZooBuffer*);
void zoo_driver(int, ZooBuffer*, const ZooEnvelope*);
```

`c_memory.c` implements global/static writes, copied pointers, two disjoint
stack objects, may-alias pointer parameters, nested fields, constant/variable
arrays, `int **`, union overlap, and an access to fixture-owned declaration
`extern int zoo_external_state` so external-object abstraction is observable.
`c_callbacks.c` implements two callbacks,
a global pointer, a two-entry table, and the four ABI wrappers. `cpp_memory.cpp`
declares `malloc`, `free`, and `memcpy` with `extern "C"`, implements two
different allocation sites plus one reused allocation helper, writes heap
fields, models copy/deallocation, and exposes `zoo_modeled_copy`.
`cpp_dispatch.cpp` keeps the abstract/single/multiple-inheritance hierarchy
private and exposes direct, one-target virtual, and two-target virtual C
wrappers. `recursion.cpp` implements self and mutual recursion that reach a
writing leaf and a C callback. `driver.cpp` calls every wrapper from
`zoo_driver`.

- [ ] **Step 4: Add exact compilation database entries and run local analysis**

The JSON contains these exact file/language pairs:

```text
c_memory.c       clang   -std=c11
c_callbacks.c    clang   -std=c11
cpp_memory.cpp   clang++ -std=c++20 -fno-rtti -fno-exceptions
cpp_dispatch.cpp clang++ -std=c++20 -fno-rtti -fno-exceptions
recursion.cpp    clang++ -std=c++20 -fno-rtti -fno-exceptions
driver.cpp       clang++ -std=c++20 -fno-rtti -fno-exceptions
```

Every command also includes `-O0 -g -fno-inline -I@PROJECT_ROOT@ -c`, writes a
unique `.o`, and uses both `directory` and `file` fields rooted at
`@PROJECT_ROOT@`.

Run:

```bash
cmake --build --preset default --target SemanticZooFixtureTest
./build/bin/SemanticZooFixtureTest
```

Expected: the six units link into one `ProgramIr` and all four cross-language
entry points resolve.

- [ ] **Step 5: Commit the integrated source project**

```bash
git add tests/fixtures/projects/semantic_zoo tests/integration/analysis/SemanticZooFixtureTest.cpp tests/integration/analysis/CMakeLists.txt
git commit -m "test: add mixed C and C++ semantic zoo"
```

### Task 8: Qualify `semantic_zoo` at the SVF and SummaryDB Boundaries

**Files:**

- Create: `tests/integration/analysis/SemanticZooSvfTest.cpp`
- Create: `tests/integration/analysis/SemanticZooSummaryDbTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/unit/analysis/llvm/StableValueMapperTest.cpp`
- Modify: `tests/unit/analysis/llvm/AbstractMemoryBuilderTest.cpp`
- Modify: `tests/unit/summarydb/SummaryRepositoryTest.cpp`
- Modify: `tests/unit/summarydb/CMakeLists.txt`

**Interfaces:**

- Consumes: Task 7 fixture plus Task 4 harness.
- Proves: stable unnamed values and allocation sites, memory shapes, alias kinds, dispatch kinds, V2 serialization, immutability, current/historical bindings, and insertion-order independence.
- Does not require: durable class-hierarchy edges in thin CPG; hierarchy is observed through SVF's virtual target set.

- [ ] **Step 1: Write failing layered semantic assertions**

```cpp
TEST(SemanticZooSvfTest, MapsMemoryAliasAndDispatchWithoutIdentityCollisions) {
  auto snapshot = MapFixtureWithSvf("semantic_zoo");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  const auto& facts = snapshot->mapping.facts;
  EXPECT_TRUE(HasAllObjectKinds(facts));
  EXPECT_TRUE(HasAlias(facts, AliasKind::kMustAlias, EpistemicState::kMust));
  EXPECT_TRUE(HasAlias(facts, AliasKind::kNoAlias, EpistemicState::kMust));
  EXPECT_TRUE(HasDispatch(facts, DispatchKind::kDirect));
  EXPECT_TRUE(HasDispatch(facts, DispatchKind::kIndirect));
  EXPECT_TRUE(HasDispatch(facts, DispatchKind::kCallback));
  EXPECT_TRUE(HasDispatch(facts, DispatchKind::kVirtual));
  EXPECT_TRUE(AllStableIdsAreUniqueWithinTheirSemanticAnchors(facts));
}

TEST(SemanticZooSummaryDbTest, NativeV2RoundTripPreservesEveryCell) {
  auto snapshot = AnalyzeAndLoadFixture("semantic_zoo",
                                        AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_TRUE(AllArtifactsAreV2(snapshot->summaries));
  EXPECT_TRUE(ContainsNestedAndArrayPaths(snapshot->summaries));
  EXPECT_TRUE(ContainsModeledAllocationCopyAndFree(snapshot->summaries));
  EXPECT_TRUE(ContainsAllDispatchKinds(snapshot->summaries));
  EXPECT_TRUE(ContainsNoIdentityCollision(snapshot->summaries));
}

TEST(SummaryRepositoryVersionTest, ReanalysisMovesBindingWithoutMutatingCas) {
  const auto first = PublishSemanticZooRevision(1);
  const auto old_bytes = ReadSummaryBytes(first.summary_id);
  const auto second = PublishSemanticZooRevision(2);
  EXPECT_NE(first.summary_id, second.summary_id);
  EXPECT_EQ(ReadSummaryBytes(first.summary_id), old_bytes);
  EXPECT_EQ(CurrentSummaryId(first.function_variant_id), second.summary_id);
}
```

- [ ] **Step 2: Build and verify semantic failures are visible**

Run:

```bash
cmake --build --preset default --target SemanticZooSvfTest SemanticZooSummaryDbTest StableValueMapperTest AbstractMemoryBuilderTest SummaryRepositoryTest
```

Expected: tests compile; any missing semantic feature fails with the named
object/alias/dispatch/path assertion rather than an empty-result comparison.

- [ ] **Step 3: Add complete stable boundary oracles**

For calls, group `NormalizedCallTarget` by stable `call_site`, require every
resolved indirect/callback/virtual target to be a stable function-variant ID
with `kMay`, and require exactly two candidates for the selector sites. For
memory, group by `(abstract_object_id, access_path)` and require distinct heap
allocation anchors at distinct source operations while repeated callers of one
helper retain one allocation-site abstraction. Prove known-zero and unknown
range cells separately. For SummaryDB, compare deterministic serialized bytes,
component hashes, and summary IDs after reversing artifact/item insertion
order; repeat publication and require idempotent current bindings.

- [ ] **Step 4: Run all layered tests**

Run:

```bash
cmake --build --preset default --target SemanticZooSvfTest SemanticZooSummaryDbTest StableValueMapperTest AbstractMemoryBuilderTest SummaryRepositoryTest
./build/bin/SemanticZooSvfTest
./build/bin/SemanticZooSummaryDbTest
./build/bin/StableValueMapperTest
./build/bin/AbstractMemoryBuilderTest
./build/bin/SummaryRepositoryTest
```

Expected: source-to-SVF and source-to-SummaryDB oracles pass with non-empty
facts and collision-free stable identities.

- [ ] **Step 5: Commit layered semantic qualification**

```bash
git add tests/integration/analysis/SemanticZooSvfTest.cpp tests/integration/analysis/SemanticZooSummaryDbTest.cpp tests/integration/analysis/CMakeLists.txt tests/unit/analysis/llvm/StableValueMapperTest.cpp tests/unit/analysis/llvm/AbstractMemoryBuilderTest.cpp tests/unit/summarydb/SummaryRepositoryTest.cpp tests/unit/summarydb/CMakeLists.txt
git commit -m "test: qualify semantic zoo boundaries"
```

### Task 9: Qualify Standard Analyzer End to End

**Files:**

- Create: `tests/integration/analysis/SemanticZooEndToEndTest.cpp`
- Modify: `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp`
- Modify: `tests/integration/wpa/CMakeLists.txt`
- Modify: `tests/unit/wpa/WpaInputMaterializerTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**

- Consumes: standard `ProjectAnalyzer`, durable `WpaRunRepository`, and Task 7 fixture.
- Proves: direct/transitive reachability, direct/transitive may-write, epistemic weakening, recursive convergence, exact completion, reuse references, and closed witnesses.
- Returns: one production `RunId` in `ProjectAnalysisResult`; conformance runs remain separate.

- [ ] **Step 1: Write the failing end-to-end result assertions**

```cpp
TEST(SemanticZooEndToEndTest, DriverReachesEveryDispatchAndWritePath) {
  auto snapshot = AnalyzeAndLoadFixture("semantic_zoo",
                                        AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  auto repository = WpaRunRepository::Open(snapshot->output_root);
  ASSERT_TRUE(repository.ok());
  auto run = (*repository)->LoadRun(ParseId(snapshot->analysis.wpa_run_id));
  ASSERT_TRUE(run.ok());
  EXPECT_EQ(run->status, WpaRunStatus::kComplete);
  auto components = (*repository)->ListComponents(run->run.run_id);
  ASSERT_TRUE(components.ok());
  EXPECT_EQ(components->size(),
            snapshot->analysis.wpa_expected_component_count);
  EXPECT_TRUE(ContainsTransitiveReachability(*components));
  EXPECT_TRUE(ContainsTransitiveMayWrite(*components));
  EXPECT_TRUE(EveryFactHasClosedWitness(*components));
}

TEST(SemanticZooEndToEndTest, PossibleCallsWeakenEffectsToMay) {
  auto components = AnalyzeZooComponents();
  EXPECT_TRUE(HasMustDirectWrite(components));
  EXPECT_TRUE(HasMayWriteThroughCallback(components));
  EXPECT_TRUE(HasMayWriteThroughVirtualSelector(components));
}

TEST(ProjectAnalyzerWpaTest, ExactEngineReuseCreatesNewRunReference) {
  const auto first = AnalyzeStableFixtureRevision(1);
  const auto second = AnalyzeStableFixtureRevision(2);
  EXPECT_NE(first.wpa_run_id, second.wpa_run_id);
  EXPECT_EQ(ResultObjectKeys(first.wpa_run_id),
            ResultObjectKeys(second.wpa_run_id));
  EXPECT_EQ(SouffleExecutionDelta(second.wpa_run_id), 0u);
}

TEST(WpaInputMaterializerTest, SemanticZooOwnsAllCurrentInputRelations) {
  auto reach = MaterializeZoo(WpaComponentKind::kReachability);
  auto memory = MaterializeZoo(WpaComponentKind::kMemoryEffects);
  ASSERT_TRUE(reach.ok());
  ASSERT_TRUE(memory.ok());
  EXPECT_TRUE(HasRelation(*reach, RelationId::kDirectCall));
  EXPECT_TRUE(HasRelation(*reach, RelationId::kUnknownCall));
  EXPECT_TRUE(HasRelation(*memory, RelationId::kDirectRead));
  EXPECT_TRUE(HasRelation(*memory, RelationId::kDirectWrite));
  EXPECT_TRUE(HasRelation(*memory, RelationId::kModeledEffect));
  EXPECT_TRUE(AllDenseCellsMapToExactlyOneStableId(*reach));
  EXPECT_TRUE(AllDenseCellsMapToExactlyOneStableId(*memory));
}
```

- [ ] **Step 2: Build and verify missing integration behavior fails**

Run:

```bash
cmake --build --preset default --target SemanticZooEndToEndTest ProjectAnalyzerWpaTest WpaEndToEndTest
```

Expected: any missing analyzer-to-orchestrator or repository query behavior
fails with the corresponding run/component/fact assertion.

- [ ] **Step 3: Add stable end-to-end fact selectors**

Selectors decode relation rows through their stable mappings and identify the
driver, callback, virtual, recursion, and modeled-copy roots by stable summary
relationships. Require at least one expected `ReachableCall` and `MayWrite`
before comparing larger sets. For every returned fact, walk witness edges to a
declared `local_roots` or `successor_roots` fact ID, reject a repeated fact ID
on the current path, and assert no orphan leaf.

Extend `WpaInputMaterializerTest` with one rejection case each for a missing
mapping, cross-domain dense ID, conflicting stable ID, noncanonical unknown
range, and conflicting duplicate tuple. Reverse summary, root, and tuple
insertion order and require the same logical hash. Keep Alias schema/RelationIo
coverage in Task 5; do not insert Alias into either zoo component.

- [ ] **Step 4: Run all end-to-end tests**

Run:

```bash
cmake --build --preset default --target SemanticZooEndToEndTest ProjectAnalyzerWpaTest WpaEndToEndTest WpaInputMaterializerTest
./build/bin/SemanticZooEndToEndTest
./build/bin/ProjectAnalyzerWpaTest
./build/bin/WpaEndToEndTest
./build/bin/WpaInputMaterializerTest
```

Expected: standard analyzer persistence, transitive results, weakening,
recursive convergence, reuse, and witness closure pass.

- [ ] **Step 5: Commit end-to-end qualification**

```bash
git add tests/integration/analysis/SemanticZooEndToEndTest.cpp tests/integration/analysis/ProjectAnalyzerWpaTest.cpp tests/integration/analysis/CMakeLists.txt tests/integration/wpa/WpaEndToEndTest.cpp tests/integration/wpa/CMakeLists.txt tests/unit/wpa/WpaInputMaterializerTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "test: qualify WPA end to end"
```

### Task 10: Add Differential and Determinism Qualification

**Files:**

- Create: `tests/qualification/CMakeLists.txt`
- Create: `tests/qualification/wpa/CMakeLists.txt`
- Create: `tests/qualification/wpa/WpaQualificationSupport.h`
- Create: `tests/qualification/wpa/WpaQualificationSupport.cpp`
- Create: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaDeterminismQualificationTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**

- Consumes: one serialized `WpaLogicalComponentInput` under two valid execution envelopes.
- Produces aggregate tests: `WpaDifferentialQualificationTest` and `WpaDeterminismQualificationTest`.
- Qualifies fixtures: `multiple_tus`, `function_pointer`, `callback_dispatch`, `virtual_dispatch`, `recursive_calls`, `abstract_memory`, and `semantic_zoo`.

The support file defines these exact test-only contracts:

```cpp
struct QualificationCase {
  std::string fixture;
  WpaComponentKind component;
  std::vector<facts::RelationId> required_nonempty_relations;
};

struct DeterministicRunSnapshot {
  std::vector<std::string> logical_hashes;
  std::vector<facts::AnalysisFact> facts;
  std::vector<facts::WitnessEdge> witnesses;
  std::vector<std::string> fixpoint_hashes;
};

StatusOr<WpaLogicalComponentInput> MaterializeCase(
    const QualificationCase& qualification);
StatusOr<facts::CanonicalizedResult> ExecuteAndCanonicalize(
    const WpaExecutor& executor, const WpaExecutionEnvelope& envelope);
StatusOr<DeterministicRunSnapshot> RunPermutation(std::uint32_t seed);
```

- [ ] **Step 1: Write differential tests with non-empty semantic guards**

```cpp
TEST_P(WpaDifferentialQualificationTest, SouffleEqualsCppForOneLogicalInput) {
  auto logical = MaterializeCase(GetParam());
  ASSERT_TRUE(logical.ok()) << logical.status().message();
  const auto bytes = SerializeCanonical(*logical);
  auto souffle = ExecuteAndCanonicalize(SouffleEnvelope(*logical));
  auto cpp = ExecuteAndCanonicalize(CppConformanceEnvelope(*logical));
  ASSERT_TRUE(souffle.ok());
  ASSERT_TRUE(cpp.ok());
  EXPECT_EQ(SerializeCanonical(SouffleEnvelope(*logical).logical), bytes);
  EXPECT_EQ(SerializeCanonical(CppConformanceEnvelope(*logical).logical), bytes);
  EXPECT_FALSE(ExpectedFactsFor(GetParam(), *souffle).empty());
  EXPECT_EQ(souffle->facts, cpp->facts);
  EXPECT_EQ(souffle->external_hash, cpp->external_hash);
  EXPECT_EQ(souffle->witnesses, cpp->witnesses);
}
```

Instantiate both domains where applicable. The reachability matrix contains
`multiple_tus`, `function_pointer`, `callback_dispatch`, `virtual_dispatch`,
`recursive_calls`, and `semantic_zoo`. The memory matrix contains
`abstract_memory`, `recursive_calls`, and `semantic_zoo`.

- [ ] **Step 2: Write seed and compilation-database permutation tests**

```cpp
TEST(WpaDeterminismQualificationTest, SeedsZeroThroughSixtyFourAreIdentical) {
  auto baseline = RunPermutation(0);
  ASSERT_TRUE(baseline.ok());
  for (std::uint32_t seed = 1; seed <= 64; ++seed) {
    auto candidate = RunPermutation(seed);
    ASSERT_TRUE(candidate.ok()) << "seed=" << seed;
    EXPECT_EQ(candidate->logical_hashes, baseline->logical_hashes);
    EXPECT_EQ(candidate->facts, baseline->facts);
    EXPECT_EQ(candidate->witnesses, baseline->witnesses);
    EXPECT_EQ(candidate->fixpoint_hashes, baseline->fixpoint_hashes);
  }
}

TEST(WpaDeterminismQualificationTest, CompileDatabaseOrderDoesNotMatter) {
  auto forward = AnalyzeZooWithCompileDatabaseOrder(false);
  auto reverse = AnalyzeZooWithCompileDatabaseOrder(true);
  ASSERT_TRUE(forward.ok());
  ASSERT_TRUE(reverse.ok());
  EXPECT_EQ(forward->summary_ids, reverse->summary_ids);
  EXPECT_EQ(forward->wpa_external_hashes, reverse->wpa_external_hashes);
}
```

- [ ] **Step 3: Build and run the two qualification aggregates**

Run:

```bash
cmake --build --preset default --target WpaDifferentialQualificationTest WpaDeterminismQualificationTest
./build/bin/WpaDifferentialQualificationTest
./build/bin/WpaDeterminismQualificationTest
```

Expected: every case has a non-empty expected subset, both engines consume
byte-identical input, canonical output and proof selection agree, and all 65
seeds plus compile-database reversal are identical.

- [ ] **Step 4: Register ordinary qualification labels**

Register exactly one aggregate CTest entry per executable with labels
`wpa-qualification;engine-conformance` and
`wpa-qualification;relations-v2`, respectively. Use `add_test`, not only
`gtest_discover_tests`, so Task 13 can enforce exact aggregate names.

- [ ] **Step 5: Commit differential and determinism qualification**

```bash
git add tests/qualification/CMakeLists.txt tests/qualification/wpa tests/CMakeLists.txt
git commit -m "test: add WPA differential qualification"
```

### Task 11: Add Failure, Migration, Lifecycle, and Performance Qualification

**Files:**

- Create: `tests/qualification/wpa/WpaFailureQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaMigrationQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaPerformanceQualificationTest.cpp`
- Create: `tests/qualification/wpa/performance-ceilings.json`
- Create: `src/wpa/SouffleProcessState.h`
- Modify: `src/wpa/SouffleWpaExecutor.cpp`
- Modify: `src/wpa/CMakeLists.txt`
- Modify: `tests/qualification/wpa/WpaQualificationSupport.h`
- Modify: `tests/qualification/wpa/WpaQualificationSupport.cpp`
- Modify: `tests/qualification/wpa/CMakeLists.txt`

**Interfaces:**

- Produces aggregate tests: `WpaFailureQualificationTest`, `WpaMigrationQualificationTest`, and `WpaPerformanceQualificationTest`.
- Injects: missing/incompatible worker, provenance mismatch, timeout, crash/signal, resource exhaustion, schema mismatch, missing/conflicting/cross-domain mappings, duplicate conflict, unsupported epistemic value, and malformed/orphaned/cyclic/unclosed witnesses.
- Measures: five warmed `recursive_calls` runs and 20 complete same-process analyses.

The support file adds the exact failure contract:

```cpp
enum class FailureKind : std::uint8_t {
  kMissingWorker, kIncompatibleBundle, kProvenanceMismatch, kTimeout,
  kCrashOrSignal, kResourceExhaustion, kSchemaMismatch, kMissingMapping,
  kConflictingMapping, kCrossDomainMapping, kConflictingDuplicate,
  kUnsupportedEpistemic, kMalformedWitness, kOrphanedWitness,
  kCyclicWitness, kUnclosedWitness,
};

struct FailureObservation {
  bool observed_failure;
  WpaRunStatus new_run_status;
  bool new_component_published;
  std::vector<std::byte> prior_component_bytes;
  std::vector<std::byte> prior_component_bytes_after;
  bool summaries_readable;
  std::size_t cpp_fallback_invocations;
};

std::array<FailureKind, 16> AllRequiredFailureKinds();
StatusOr<FailureObservation> InjectFailureAfterPriorSuccess(FailureKind kind);
```

- [ ] **Step 1: Write the complete table-driven failure test**

```cpp
TEST(WpaFailureQualificationTest, EveryFailureRetainsPriorSuccess) {
  for (FailureKind failure : AllRequiredFailureKinds()) {
    SCOPED_TRACE(FailureKindName(failure));
    auto result = InjectFailureAfterPriorSuccess(failure);
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_TRUE(result->observed_failure);
    EXPECT_EQ(result->new_run_status, WpaRunStatus::kIncomplete);
    EXPECT_FALSE(result->new_component_published);
    EXPECT_EQ(result->prior_component_bytes,
              result->prior_component_bytes_after);
    EXPECT_TRUE(result->summaries_readable);
    EXPECT_EQ(result->cpp_fallback_invocations, 0u);
  }
}
```

`AllRequiredFailureKinds()` returns exactly 16 distinct enum values covering
the conditions listed in the Interfaces block. Each case injects at the
narrowest boundary: executor process, provenance reader, relation I/O,
stable/dense mapper, raw evaluation, or witness canonicalizer.

- [ ] **Step 2: Write migration and lifecycle tests**

```cpp
TEST(WpaMigrationQualificationTest, V1RemainsImmutableAndV2BecomesCurrent) {
  const auto old = LoadHistoricalV1Fixture();
  const auto old_bytes = old.bytes;
  auto reanalysis = ReanalyzeHistoricalProjectAsV2();
  ASSERT_TRUE(reanalysis.ok());
  EXPECT_EQ(ReadObject(old.id), old_bytes);
  EXPECT_TRUE(CurrentBindingIsV2(reanalysis->revision_id));
  EXPECT_TRUE(OldWpaRunIsHistoricalNotCurrent());
}

TEST(WpaMigrationQualificationTest, TwentyRunsCleanAllProcessState) {
  for (int run = 0; run < 20; ++run) {
    ASSERT_TRUE(AnalyzeFixtureInCurrentProcess("multiple_tus").ok())
        << "run=" << run;
    EXPECT_EQ(ActiveSouffleWorkerCount(), 0u);
    EXPECT_TRUE(SvfSessionStateIsCleanForTest());
  }
}
```

- [ ] **Step 3: Add fixed performance ceilings and warmed measurements**

```json
{
  "schema_version": "wpa-performance.v1",
  "fixture": "recursive_calls",
  "maximum_wall_time_ms": 30000,
  "maximum_peak_rss_mb": 2048,
  "maximum_output_facts": 1000000
}
```

The executable performs one unmeasured warm-up and five measured iterations,
reports the median wall time and maximum RSS, asserts all five semantic hashes
are identical, and fails if any checked-in ceiling is exceeded. Read process
RSS through a small platform adapter: `getrusage(RUSAGE_SELF)` on macOS/Linux,
normalizing macOS bytes and Linux KiB to MiB. Never rewrite the JSON from the
test.

Change the private worker launcher to accept the complete
`WpaExecutionLimits`. In the child, before `execv`, apply `setrlimit(RLIMIT_AS)`
when `memory_mb != 0`; return `Status::Internal` with the distinct diagnostic
prefix `Souffle worker resource exhaustion:` when the limited worker cannot
complete. Track forked-but-not-yet-reaped children with
one process-local atomic counter in `SouffleProcessState.h`, decrementing on
normal exit, signal, timeout, and launcher error. Expose only
`ActiveSouffleWorkerCountForTest()` from this private header and include it in
qualification through the target's existing private source include path.

- [ ] **Step 4: Build and run all three aggregates**

Run:

```bash
cmake --build --preset default --target WpaFailureQualificationTest WpaMigrationQualificationTest WpaPerformanceQualificationTest
./build/bin/WpaFailureQualificationTest
./build/bin/WpaMigrationQualificationTest
./build/bin/WpaPerformanceQualificationTest
```

Expected: all failure cases are atomic, historical bytes remain immutable, 20
runs clean process state, and five warmed runs satisfy fixed ceilings with one
semantic result.

- [ ] **Step 5: Commit robustness qualification**

```bash
git add tests/qualification/wpa/WpaFailureQualificationTest.cpp tests/qualification/wpa/WpaMigrationQualificationTest.cpp tests/qualification/wpa/WpaPerformanceQualificationTest.cpp tests/qualification/wpa/performance-ceilings.json src/wpa/SouffleProcessState.h src/wpa/SouffleWpaExecutor.cpp src/wpa/CMakeLists.txt tests/qualification/wpa/WpaQualificationSupport.h tests/qualification/wpa/WpaQualificationSupport.cpp tests/qualification/wpa/CMakeLists.txt
git commit -m "test: qualify WPA robustness and performance"
```

### Task 12: Add the Complete Analysis Fact Bus Handoff

**Files:**

- Create: `include/veritas/facts/AnalysisFactBus.h`
- Create: `src/facts/AnalysisFactBus.cpp`
- Modify: `include/veritas/wpa/WpaOrchestrator.h`
- Modify: `src/wpa/WpaOrchestrator.cpp`
- Modify: `include/veritas/wpa/WpaRunRepository.h`
- Modify: `src/wpa/WpaRunRepository.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Create: `tests/unit/facts/AnalysisFactBusTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp`

**Interfaces:**

- Consumes: only a complete `WpaRunResult`.
- Produces: `AnalysisFactBatch`, `AnalysisFactSink`, `AnalysisFactBus::AddSink`, and `AnalysisFactBus::Publish`.
- Delivers: idempotent at least once per `(RunId, BatchId, SinkId)` with durable pending/completed sink state.

- [ ] **Step 1: Write failing validation and retry tests**

```cpp
TEST(AnalysisFactBusTest, RejectsMissingComponentOrUndeclaredRoot) {
  auto incomplete = SuccessfulBatch();
  incomplete.completed_components.pop_back();
  EXPECT_EQ(Bus().Publish(std::move(incomplete)).code(),
            StatusCode::kFailedPrecondition);
  EXPECT_EQ(Bus().Publish(BatchWithUndeclaredWitnessLeaf()).code(),
            StatusCode::kFailedPrecondition);
}

TEST(AnalysisFactBusTest, RetryVisitsOnlyPendingSinks) {
  RecordingSink first;
  FailOnceSink second;
  auto bus = BusWithSinks(first, second);
  const auto batch = SuccessfulBatch();
  EXPECT_FALSE(bus.Publish(batch).ok());
  EXPECT_TRUE(bus.Publish(batch).ok());
  EXPECT_EQ(first.logical_publication_count(batch.batch_id), 1u);
  EXPECT_EQ(second.logical_publication_count(batch.batch_id), 1u);
}
```

- [ ] **Step 2: Build and verify the test fails**

Run:

```bash
cmake --build --preset default --target AnalysisFactBusTest WpaEndToEndTest
```

Expected: compilation fails because the bus contract is absent.

- [ ] **Step 3: Implement the exact handoff types**

```cpp
struct AnalysisFactBatch {
  core::StableId batch_id;
  AnalysisRunManifest run;
  std::vector<wpa::WpaComponentKey> expected_components;
  std::vector<wpa::WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  std::vector<AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

class AnalysisFactSink {
 public:
  virtual ~AnalysisFactSink() = default;
  virtual Status Publish(const AnalysisFactBatch& batch) = 0;
};

class AnalysisFactBus {
 public:
  explicit AnalysisFactBus(wpa::WpaRunRepository& delivery_state);
  Status AddSink(std::string sink_id, AnalysisFactSink& sink);
  Status Publish(AnalysisFactBatch batch) const;
};
```

Canonicalize component, root, fact, and witness order; derive `batch_id` from
that content. Require exact expected/completed key equality, one run manifest,
unique fact IDs with nonconflicting rows, component hashes matching the stored
objects, and complete witness closure. Store pending/completed delivery rows in
the V2 metadata schema. A sink error does not change WPA component or run
success; retry addresses pending sinks only.

- [ ] **Step 4: Run bus and end-to-end tests**

Run:

```bash
cmake --build --preset default --target AnalysisFactBusTest WpaEndToEndTest
./build/bin/AnalysisFactBusTest
./build/bin/WpaEndToEndTest
```

Expected: incomplete or mixed batches fail, valid batches deliver once, and
partial fan-out retries safely.

- [ ] **Step 5: Commit the M9-neutral handoff**

```bash
git add include/veritas/facts/AnalysisFactBus.h src/facts/AnalysisFactBus.cpp include/veritas/wpa/WpaOrchestrator.h src/wpa/WpaOrchestrator.cpp include/veritas/wpa/WpaRunRepository.h src/wpa/WpaRunRepository.cpp src/facts/CMakeLists.txt tests/unit/facts/AnalysisFactBusTest.cpp tests/unit/facts/CMakeLists.txt tests/integration/wpa/WpaEndToEndTest.cpp
git commit -m "feat: add complete WPA fact handoff"
```

### Task 13: Enforce Exact Qualification and M9 Entry Gates

**Files:**

- Create: `tests/qualification/check_no_skips.py`
- Create: `tools/check_m9_entry.py`
- Create: `tests/qualification/M9EntryGateTest.py`
- Create: `tests/qualification/M9DocumentationConsistencyTest.py`
- Modify: `tests/qualification/CMakeLists.txt`
- Modify: `tests/qualification/wpa/CMakeLists.txt`
- Modify: `tests/unit/facts/CMakeLists.txt`
- Modify: `tests/unit/wpa/CMakeLists.txt`
- Modify: `tests/unit/analysis/llvm/CMakeLists.txt`
- Modify: `tests/unit/summary/CMakeLists.txt`
- Modify: `tests/unit/summarydb/CMakeLists.txt`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/integration/analysis/svf/CMakeLists.txt`
- Modify: `tests/integration/wpa/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `README.md`
- Modify: `CLAUDE.md`
- Modify: `docs/README.md`
- Modify: `docs/plans/README.md`
- Modify: `docs/specs/README.md`
- Modify: `docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md`
- Modify: `docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md`

**Interfaces:**

- Produces: `python3 tools/check_m9_entry.py --build-dir build`.
- Enforces: exact names, no missing/extra/duplicate/disabled/skipped/failed/errored aggregates, production Souffle configuration, pinned provenance, generated workers, and documentation consistency.
- Registers exactly five `wpa-qualification` aggregate tests and the ten criterion memberships from the approved specification.

- [ ] **Step 1: Write gate unit tests before the checker**

```python
class M9EntryGateTest(unittest.TestCase):
    def test_rejects_missing_extra_duplicate_and_skipped_members(self):
        for fixture in (
            "missing-member", "extra-member", "duplicate-member",
            "disabled-member", "skipped-member", "failed-member",
            "errored-member",
        ):
            with self.subTest(fixture=fixture):
                result = run_gate(fixture_build(fixture))
                self.assertNotEqual(result.returncode, 0)

    def test_rejects_emergency_engine_and_wrong_provenance(self):
        self.assertIn("production engine is not souffle",
                      run_gate(fixture_build("cpp-emergency")).stderr)
        self.assertIn("Souffle provenance mismatch",
                      run_gate(fixture_build("wrong-provenance")).stderr)
```

- [ ] **Step 2: Implement strict JUnit parsing**

```python
EXPECTED_BY_LABEL = {
    "summary-v2": {"FunctionSummaryV2Test", "SummaryRepositoryVersionTest"},
    "indirect-calls": {"SvfFactMapperV2Test", "CallGraphTest"},
    "stable-identity": {"StableValueIdentityTest", "AbstractMemoryBuilderTest", "DenseIdMapTest"},
    "relations-v2": {"RelationSchemaTest", "WpaInputMaterializerTest", "WpaDeterminismQualificationTest"},
    "souffle-production": {"SouffleWpaExecutorTest", "ProjectAnalyzerWpaTest", "WpaPerformanceQualificationTest"},
    "engine-conformance": {"WpaExecutorConformanceTest", "WpaDifferentialQualificationTest"},
    "witness-closure": {"WitnessCanonicalizerTest", "AnalysisFactBusTest"},
    "failure-atomicity": {"WpaFailureQualificationTest", "WpaOrchestratorTest"},
    "run-identity": {"AnalysisRunTest", "WpaMigrationQualificationTest"},
    "documentation-consistency": {"M9DocumentationConsistencyTest"},
}

WPA_QUALIFICATION = {
    "WpaDifferentialQualificationTest",
    "WpaDeterminismQualificationTest",
    "WpaFailureQualificationTest",
    "WpaMigrationQualificationTest",
    "WpaPerformanceQualificationTest",
}
```

`check_no_skips.py` parses every `<testcase>`, rejects a duplicate before
converting names to a set, compares exact expected membership, and rejects any
suite/case failure, error, disabled count, or `<skipped>` element.
`check_m9_entry.py` runs CTest once per label with `--no-tests=error` and JUnit,
then delegates XML validation to the same parser. It also checks
`VERITAS_WPA_ENGINE:STRING=souffle`, the exact pinned revision, the actual
Souffle executable SHA-256, generated `v2_reach`/`v2_maywrite` programs, and
the worker path.

- [ ] **Step 3: Register exact aggregate ownership**

Use `add_test(NAME <aggregate> COMMAND $<TARGET_FILE:<target>>)` for every C++
aggregate and the Python interpreter for documentation consistency. Assign
`m9-entry;<criterion>` exactly as listed above. Assign
`wpa-qualification` only to the five qualification executables. Discovered
GoogleTest cases keep ordinary labels but are not members of either exact
registry.

Use these exact aggregate-to-command mappings:

| Aggregate name | Command target/script |
| --- | --- |
| `FunctionSummaryV2Test` | `SummaryV2BuilderTest` |
| `SummaryRepositoryVersionTest` | `SummaryRepositoryTest` |
| `SvfFactMapperV2Test` | `svf_fact_mapper_integration_test` |
| `CallGraphTest` | `CallGraphTest` |
| `StableValueIdentityTest` | `StableValueMapperTest` |
| `AbstractMemoryBuilderTest` | `AbstractMemoryBuilderTest` |
| `DenseIdMapTest` | `DenseIdMapTest` |
| `RelationSchemaTest` | `RelationSchemaTest` |
| `WpaInputMaterializerTest` | `WpaInputMaterializerTest` |
| `WpaDeterminismQualificationTest` | same-named target |
| `SouffleWpaExecutorTest` | `SouffleWpaExecutorTest` |
| `ProjectAnalyzerWpaTest` | same-named target |
| `WpaPerformanceQualificationTest` | same-named target |
| `WpaExecutorConformanceTest` | `WpaExecutorConformanceTest` |
| `WpaDifferentialQualificationTest` | same-named target |
| `WitnessCanonicalizerTest` | `ResultCanonicalizerTest` |
| `AnalysisFactBusTest` | `AnalysisFactBusTest` |
| `WpaFailureQualificationTest` | same-named target |
| `WpaOrchestratorTest` | `WpaOrchestratorTest` |
| `AnalysisRunTest` | `AnalysisRunTest` |
| `WpaMigrationQualificationTest` | same-named target |
| `M9DocumentationConsistencyTest` | `tests/qualification/M9DocumentationConsistencyTest.py` |

- [ ] **Step 4: Update CI and canonical documentation**

CI builds the vendored Souffle revision, verifies
`build/souffle-provenance.json`, builds all qualification targets, runs:

```bash
ctest --test-dir build -L wpa-qualification --no-tests=error --output-on-failure --output-junit build/wpa-qualification.xml
python3 tests/qualification/check_no_skips.py build/wpa-qualification.xml --label wpa-qualification
python3 tools/check_m9_entry.py --build-dir build
```

Install Bison, Flex, and the existing Souffle build prerequisites in the CI
dependency step before configuring VERITAS; do not fetch or install a second
Souffle copy.

Update current-state documentation to state that standard project analysis
runs durable compiled-Souffle WPA, M8R.3-M8R.5 are delivered, and M9 is
unblocked only when the executable gate passes. Preserve immutable historical
M8 descriptions and link them forward to the remediation documents.

- [ ] **Step 5: Run the gate unit tests and live gate**

Run:

```bash
python3 tests/qualification/M9EntryGateTest.py
cmake --build --preset default
ctest --test-dir build -L wpa-qualification --no-tests=error --output-on-failure --output-junit build/wpa-qualification.xml
python3 tests/qualification/check_no_skips.py build/wpa-qualification.xml --label wpa-qualification
python3 tools/check_m9_entry.py --build-dir build
```

Expected: synthetic bad reports are rejected, all five live qualification
aggregates pass without skips, and all ten exact M9 criteria pass.

- [ ] **Step 6: Run full repository verification**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
ctest --test-dir build --output-on-failure
python3 tools/check_m9_entry.py --build-dir build
git diff --check
```

Expected: configure and build exit zero, the complete CTest suite reports zero
failures, all M9 aggregates execute without skips, the gate passes ten
criteria, and `git diff --check` is silent.

- [ ] **Step 7: Commit the executable gate and documentation**

```bash
git add tests/qualification/check_no_skips.py tools/check_m9_entry.py tests/qualification/M9EntryGateTest.py tests/qualification/M9DocumentationConsistencyTest.py tests/qualification/CMakeLists.txt tests/qualification/wpa/CMakeLists.txt tests/unit/facts/CMakeLists.txt tests/unit/wpa/CMakeLists.txt tests/unit/analysis/llvm/CMakeLists.txt tests/unit/summary/CMakeLists.txt tests/unit/summarydb/CMakeLists.txt tests/integration/analysis/CMakeLists.txt tests/integration/analysis/svf/CMakeLists.txt tests/integration/wpa/CMakeLists.txt .github/workflows/ci.yml README.md CLAUDE.md docs/README.md docs/plans/README.md docs/specs/README.md docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md
git commit -m "test: enforce WPA qualification gate"
```

---

## Spec Coverage Matrix

| Approved requirement | Tasks |
| --- | --- |
| Durable run/component lifecycle, exact-engine reuse | 1-3, 9 |
| Reverse-topological SCC orchestration and support ownership | 2, 6, 9 |
| Standard analyzer defaults to Souffle; explicit emergency; no fallback | 3, 9, 11 |
| Global/stack/heap/argument/function/external/subobject/array/overlap/unknown memory | 5, 7, 8 |
| Must/May/No/Unknown alias transport | 5, 8 |
| Direct, indirect, callback, virtual, recursive, cross-TU, modeled, unknown calls | 6-9 |
| Single and multiple inheritance with multiple virtual targets | 6-8 |
| Stable unnamed-value and allocation-site identities | 5, 7, 8 |
| SummaryDB V2 serialization, immutability, current/history bindings | 5, 8, 11 |
| Relation schema/mapping/ownership validation | 5, 9, 10 |
| Byte-identical Souffle/C++ inputs and canonical output equality | 10 |
| Finite deterministic rooted witnesses | 2, 9, 10, 12 |
| Failure atomicity and prior-success retention | 1-3, 11 |
| Seeds 0-64, 20 same-process runs, five warmed measurements | 10-11 |
| Exact five qualification aggregates and ten M9 criteria | 13 |

## Final Verification Checklist

- [ ] Primary checkout is clean on `main`; task worktree is on `claude/wpa-summarydb-qualification-design`.
- [ ] Every new source/config/script file has the full Apache-2.0 header.
- [ ] `summary.v1` fixtures remain readable and byte-identical.
- [ ] Native project analysis publishes only `summary.v2` artifacts.
- [ ] Production analyzer reports one complete Souffle run with exact component counts.
- [ ] C++ is never invoked after a Souffle failure.
- [ ] Every memory, alias, dispatch, hierarchy, recursion, and unknown scenario has a named non-empty oracle.
- [ ] Every dense execution ID maps to exactly one stable ID of the correct domain.
- [ ] Souffle and C++ facts, witnesses, and external hashes agree on overlapping domains.
- [ ] Cache reuse validates exact engine/toolchain provenance and immutable object bytes.
- [ ] Every failure injection leaves prior success and published summaries readable.
- [ ] All 65 permutation seeds, 20 lifecycle analyses, and five warmed measurements pass.
- [ ] Exact `wpa-qualification` and `m9-entry` membership passes with no skips.
- [ ] Full configure, build, CTest, M9 gate, and `git diff --check` pass.

## Execution Handoff

Execute Tasks 1-13 in order. Each task is a review checkpoint and ends with a
focused test command plus a commit; do not begin the next task until the
current task's focused checks pass. Run the complete repository verification
only after Task 13 has made the qualification and M9 gates executable.
