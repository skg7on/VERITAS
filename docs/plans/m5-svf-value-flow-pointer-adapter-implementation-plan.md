# M5 Required In-Process SVF Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the required pinned SVF library on VERITAS-owned in-memory project IR, map its value-flow/pointer results into Summary IR, and publish completed project summaries from one `analyze --project` invocation.

**Architecture:** M0 supplies `third_party/SVF` and a required private CMake wrapper; M4 supplies a live move-only `ProgramIr` plus local summary drafts. M5 creates one serialized RAII SVF session directly from `ProgramIr::module()`, maps SVF data through M4 origin maps, releases all SVF singleton state, merges facts conservatively, and publishes only VERITAS-native summaries and provenance.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22.x, SVF commit `18fb5650600530a54f0afc22f4df1a10b03d3c02`, SVF `SvfCore`/`SvfLLVM`, Z3 as required by SVF, Protobuf Summary IR, GoogleTest.

**Spec:** `docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md`

## Global Constraints

- SVF is required in the standard build and standard full-analysis pipeline; there is no `VERITAS_ENABLE_SVF` switch or disabled build.
- The Git submodule path is `third_party/SVF`, upstream is `https://github.com/SVF-tools/SVF.git`, and the exact revision is `18fb5650600530a54f0afc22f4df1a10b03d3c02`.
- VERITAS and SVF use the same LLVM/Clang 22.x installation and compatible RTTI, exception, target, and ABI settings.
- M5 receives a live private `ProgramIr`; it never accepts a `.bc`, `.ll`, LLVM-module pathname, serialized SVF graph, or standalone SVF output.
- SVF headers remain under `src/analysis/svf`; installed VERITAS headers expose no SVF or LLVM native types.
- SVF node IDs are transient and are never persisted as VERITAS identity.
- V1 runs `AndersenWaveDiff`; another pointer-analysis mode requires a new design decision and analyzer identity.
- The adapter is serialized until the pinned SVF revision is proven safe for concurrent independent contexts.
- Validated partial results carry scoped unknown/truncation facts; fatal SVF construction or cleanup failure fails project analysis.
- M4 local `MUST` facts and Clang source anchors are never silently erased by SVF mapping.

---

### Task 1: Required SVF Build Contract and Configuration

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/Dependencies.cmake`
- Create: `src/analysis/svf/CMakeLists.txt`
- Create: `src/analysis/svf/SvfConfig.h`
- Create: `src/analysis/svf/SvfConfig.cpp`
- Create: `cmake/tests/RequiredSvfContract.cmake`
- Test: `tests/unit/analysis/svf/SvfConfigTest.cpp`

**Interfaces:**
- Consumes: M0 `veritas_third_party_svf`, `SvfCore`, and `SvfLLVM` CMake targets
- Produces: private `veritas_svf_analysis` target
- Produces: `analysis::svf::SvfConfig::Default()`
- Produces: analyzer configuration bytes for `AnalyzerRunID`

- [ ] **Step 1: Write failing required-build contract tests**

Create `cmake/tests/RequiredSvfContract.cmake` to fail unless all of these conditions hold:

```cmake
if(DEFINED VERITAS_ENABLE_SVF)
  message(FATAL_ERROR "VERITAS_ENABLE_SVF must not exist")
endif()
if(NOT TARGET SvfCore OR NOT TARGET SvfLLVM)
  message(FATAL_ERROR "required pinned SVF library targets are missing")
endif()
if(NOT TARGET veritas_third_party_svf)
  message(FATAL_ERROR "VERITAS third-party SVF wrapper is missing")
endif()
```

Write the config test:

```cpp
TEST(SvfConfigTest, DefaultIsRequiredBoundedAndersenWaveDiff) {
  const auto config = analysis::svf::SvfConfig::Default();
  EXPECT_EQ(config.pointer_analysis,
            analysis::svf::PointerAnalysisKind::kAndersenWaveDiff);
  EXPECT_GT(config.soft_analysis_budget.count(), 0);
  EXPECT_GT(config.max_graph_nodes, 0u);
  EXPECT_GT(config.max_emitted_facts, 0u);
  EXPECT_TRUE(config.field_sensitive);
}
```

- [ ] **Step 2: Run the focused checks and verify failure**

Run:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
ctest --test-dir build -R "RequiredSvfContract|SvfConfig" --output-on-failure
```

Expected: the target/config tests fail because `veritas_svf_analysis` and `SvfConfig` are missing.

- [ ] **Step 3: Define the private V1 configuration**

```cpp
namespace veritas::analysis::svf {
enum class PointerAnalysisKind { kAndersenWaveDiff };

struct SvfConfig {
  PointerAnalysisKind pointer_analysis;
  std::chrono::seconds soft_analysis_budget;
  std::size_t max_graph_nodes;
  std::size_t max_emitted_facts;
  bool field_sensitive;

  static SvfConfig Default();
  std::string CanonicalAnalyzerConfig() const;
};
}
```

Use deterministic defaults of `300s`, `2'000'000` graph nodes, `5'000'000` emitted facts, and field sensitivity enabled. Serialize the enum name and every limit in fixed field order for analyzer identity.

```cpp
SvfConfig SvfConfig::Default() {
  return SvfConfig{
      .pointer_analysis = PointerAnalysisKind::kAndersenWaveDiff,
      .soft_analysis_budget = std::chrono::seconds(300),
      .max_graph_nodes = 2'000'000,
      .max_emitted_facts = 5'000'000,
      .field_sensitive = true,
  };
}

std::string SvfConfig::CanonicalAnalyzerConfig() const {
  return "pointer_analysis=andersen_wave_diff\n"
         "soft_analysis_budget_seconds=" +
         std::to_string(soft_analysis_budget.count()) + "\n" +
         "max_graph_nodes=" + std::to_string(max_graph_nodes) + "\n" +
         "max_emitted_facts=" + std::to_string(max_emitted_facts) + "\n" +
         "field_sensitive=" + (field_sensitive ? "true\n" : "false\n");
}
```

- [ ] **Step 4: Link the private SVF analysis target**

```cmake
add_library(veritas_svf_analysis STATIC
  SvfConfig.cpp)
target_link_libraries(veritas_svf_analysis
  PRIVATE veritas_third_party_svf SvfCore SvfLLVM veritas_summary)
target_include_directories(veritas_svf_analysis PRIVATE
  "${PROJECT_SOURCE_DIR}/src")
```

Do not create `FindSVF.cmake`, `find_package(SVF)`, or an enable/disable option.

- [ ] **Step 5: Run the build contract and config tests**

Run: `ctest --test-dir build -R "RequiredSvfContract|SvfConfig" --output-on-failure`

Expected: both tests pass.

- [ ] **Step 6: Commit required M5 build configuration**

```bash
git add CMakeLists.txt cmake/Dependencies.cmake cmake/tests/RequiredSvfContract.cmake src/analysis/svf/CMakeLists.txt src/analysis/svf/SvfConfig.* tests/unit/analysis/svf/SvfConfigTest.cpp
git commit -m "build: require SVF analysis target"
```

---

### Task 2: Serialized RAII SVF Session over Live Program IR

**Files:**
- Modify: `src/analysis/svf/CMakeLists.txt`
- Create: `src/analysis/svf/SvfSession.h`
- Create: `src/analysis/svf/SvfSession.cpp`
- Test: `tests/integration/analysis/svf/SvfSessionTest.cpp`

**Interfaces:**
- Consumes: mutable `pipeline::ProgramIr&`, `SvfConfig`, and the pinned SVF direct-module API
- Produces: `analysis::svf::SvfSessionView` valid only inside the session callback
- Produces: `Status RunWithSvfSession(ProgramIr&, const SvfConfig&, SvfSessionCallback)`

- [ ] **Step 1: Write failing lifecycle tests**

```cpp
TEST(SvfSessionTest, BuildsDirectlyFromLiveLlvmModule) {
  ASSERT_OK_AND_ASSIGN(auto program_ir, BuildFixtureProgramIr("parameter_return"));
  int callback_count = 0;
  ASSERT_OK(analysis::svf::RunWithSvfSession(
      program_ir, SvfConfig::Default(),
      [&](const SvfSessionView& view) {
        ++callback_count;
        EXPECT_NE(view.svf_ir, nullptr);
        EXPECT_NE(view.andersen, nullptr);
        EXPECT_NE(view.svfg, nullptr);
        return Status::Ok();
      }));
  EXPECT_EQ(callback_count, 1);
}

TEST(SvfSessionTest, ReleasesSingletonStateBetweenRuns) {
  for (int run = 0; run < 2; ++run) {
    ASSERT_OK_AND_ASSIGN(auto program_ir,
                         BuildFixtureProgramIr("parameter_return"));
    ASSERT_OK(RunNoopSvfSession(program_ir));
    EXPECT_TRUE(SvfGlobalStateIsCleanForTest());
  }
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R SvfSessionTest --output-on-failure`

Expected: compilation fails because `SvfSession` does not exist.

- [ ] **Step 3: Define callback-scoped native state**

```cpp
namespace veritas::analysis::svf {
struct SvfSessionView {
  SVF::SVFIR* svf_ir;
  SVF::AndersenWaveDiff* andersen;
  SVF::SVFG* svfg;
};

using SvfSessionCallback =
    std::function<Status(const SvfSessionView&)>;

Status RunWithSvfSession(
    pipeline::ProgramIr& program_ir,
    const SvfConfig& config,
    SvfSessionCallback callback);

// Declared only in the integration-test build of this private header.
bool SvfGlobalStateIsCleanForTest();
}
```

This header is private under `src/analysis/svf`; native pointers cannot cross an installed API.

- [ ] **Step 4: Implement direct in-process construction**

Inside a process-wide mutex, call the pinned APIs in this order:

```cpp
SVF::LLVMModuleSet::buildSVFModule(program_ir.module());
SVF::SVFIRBuilder builder;
SVF::SVFIR* svf_ir = builder.build();
SVF::AndersenWaveDiff* andersen =
    SVF::AndersenWaveDiff::createAndersenWaveDiff(svf_ir);
SVF::SVFGBuilder svfg_builder;
SVF::SVFG* svfg = svfg_builder.buildFullSVFG(andersen);
```

Invoke the callback synchronously while `ProgramIr` is alive. Do not use the overload that accepts bitcode filenames and do not call an SVF executable.

- [ ] **Step 5: Implement unconditional reverse-order cleanup**

Install a state-tracking guard immediately after entering the mutex. Set each
flag only after the corresponding singleton was successfully created:

```cpp
struct SvfCleanup final {
  bool module_set_built = false;
  bool svf_ir_built = false;
  bool andersen_built = false;

  ~SvfCleanup() {
    if (andersen_built) {
      SVF::AndersenWaveDiff::releaseAndersenWaveDiff();
    }
    if (svf_ir_built) {
      SVF::SVFIR::releaseSVFIR();
    }
    if (module_set_built) {
      SVF::LLVMModuleSet::releaseLLVMModuleSet();
    }
  }
};

std::scoped_lock lock(ProcessWideSvfMutex());
SvfCleanup cleanup;
SVF::LLVMModuleSet::buildSVFModule(program_ir.module());
cleanup.module_set_built = true;
SVF::SVFIRBuilder builder;
SVF::SVFIR* svf_ir = builder.build();
if (svf_ir == nullptr) return Status::Internal("SVFIR construction failed");
cleanup.svf_ir_built = true;
SVF::AndersenWaveDiff* andersen =
    SVF::AndersenWaveDiff::createAndersenWaveDiff(svf_ir);
if (andersen == nullptr) return Status::Internal("SVF Andersen failed");
cleanup.andersen_built = true;
SVF::SVFGBuilder svfg_builder;
SVF::SVFG* svfg = svfg_builder.buildFullSVFG(andersen);
if (svfg == nullptr) return Status::Internal("SVFG construction failed");
return callback(SvfSessionView{svf_ir, andersen, svfg});
```

Callback errors and validation failures therefore run the same cleanup. Implement
`SvfGlobalStateIsCleanForTest` with private, test-only lifecycle counters rather
than recreating SVF singletons to inspect them.

- [ ] **Step 6: Add the session source to the private target**

```cmake
target_sources(veritas_svf_analysis PRIVATE SvfSession.cpp)
```

- [ ] **Step 7: Run lifecycle tests twice**

Run:

```bash
ctest --test-dir build -R SvfSessionTest --repeat until-fail:2 --output-on-failure
```

Expected: both repeated runs pass without singleton reuse, stale-module, or double-release failures.

- [ ] **Step 8: Commit the in-process SVF lifecycle**

```bash
git add src/analysis/svf/CMakeLists.txt src/analysis/svf/SvfSession.* tests/integration/analysis/svf/SvfSessionTest.cpp
git commit -m "feat: run SVF on live project IR"
```

---

### Task 3: SVF Origin Resolution and Fact Mapping

**Files:**
- Modify: `src/analysis/svf/CMakeLists.txt`
- Create: `src/analysis/svf/SvfFactMapper.h`
- Create: `src/analysis/svf/SvfFactMapper.cpp`
- Create: `tests/fixtures/projects/parameter_return/compile_commands.json`
- Create: `tests/fixtures/projects/parameter_return/parameter_return.cpp`
- Create: `tests/fixtures/projects/store_load/compile_commands.json`
- Create: `tests/fixtures/projects/store_load/store_load.cpp`
- Create: `tests/fixtures/projects/field_access/compile_commands.json`
- Create: `tests/fixtures/projects/field_access/field_access.cpp`
- Create: `tests/fixtures/projects/function_pointer/compile_commands.json`
- Create: `tests/fixtures/projects/function_pointer/function_pointer.cpp`
- Test: `tests/integration/analysis/svf/SvfFactMapperTest.cpp`

**Interfaces:**
- Consumes: callback-scoped `SvfSessionView`, M4 `ProgramIr::origin_map()`, and `AnalyzerRunContext`
- Produces: VERITAS-only `SvfFacts`
- Produces: `StatusOr<SvfFacts> MapSvfFacts(const ProgramIr&, const SvfSessionView&, const AnalyzerRunContext&, const SvfConfig&)`

- [ ] **Step 1: Write failing mapping tests**

```cpp
TEST(SvfFactMapperTest, MapsParameterReturnAndStoreLoadFlows) {
  ASSERT_OK_AND_ASSIGN(auto parameter_facts,
                       AnalyzeFixtureWithSvf("parameter_return"));
  EXPECT_HAS_VALUE_FLOW(parameter_facts.value_flows, "arg0", "return");

  ASSERT_OK_AND_ASSIGN(auto store_facts, AnalyzeFixtureWithSvf("store_load"));
  EXPECT_HAS_STORE_LOAD_FLOW_WITH_ALIAS_PROVENANCE(store_facts);
}

TEST(SvfFactMapperTest, MapsFieldPathsAndIndirectCalls) {
  ASSERT_OK_AND_ASSIGN(auto field_facts, AnalyzeFixtureWithSvf("field_access"));
  EXPECT_HAS_FIELD_PATH(field_facts.value_flows, "record.payload");

  ASSERT_OK_AND_ASSIGN(auto call_facts,
                       AnalyzeFixtureWithSvf("function_pointer"));
  EXPECT_HAS_BOUNDED_MAY_CALL(call_facts.refined_calls, "callback");
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run: `ctest --test-dir build -R SvfFactMapperTest --output-on-failure`

Expected: compilation fails because `SvfFactMapper` and `SvfFacts` are missing.

- [ ] **Step 3: Define VERITAS-only mapped output**

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
}
```

- [ ] **Step 4: Resolve SVF values through LLVM and M4 origins**

For each mapped `SVFValue`, require `LLVMModuleSet::hasLLVMValue(value)`, call `LLVMModuleSet::getLLVMValue(value)`, then resolve the resulting `const ::llvm::Value*` through `ProgramIr::origin_map()`. If either lookup fails, add a scoped `UnknownFact` with analyzer provenance and do not persist the SVF node ID.

```cpp
std::optional<summary::ValueRef> ResolveValue(
    const SVF::SVFValue* value,
    const pipeline::ProgramIr& program_ir) {
  auto* modules = SVF::LLVMModuleSet::getLLVMModuleSet();
  if (value == nullptr || !modules->hasLLVMValue(value)) {
    return std::nullopt;
  }
  const ::llvm::Value* llvm_value = modules->getLLVMValue(value);
  if (llvm_value == nullptr) return std::nullopt;
  return program_ir.origin_map().FindValue(llvm_value);
}
```

- [ ] **Step 5: Map value-flow, alias, memory, and call results**

Traverse the SVFG in deterministic source/destination order. Map edges to `ValueFlowFact`; query `AndersenWaveDiff::alias` for mapped pointer pairs; preserve GEP field paths; map singleton justified targets or bounded target sets to call facts. Translate SVF `MustAlias`, `MayAlias`, `NoAlias`, and unresolvable cases to the corresponding VERITAS epistemic states.

Collect SVFG edges first, map their endpoints to VERITAS references, discard no
unmapped edge silently, and sort only after mapping:

```cpp
for (const SVF::SVFGEdge* edge : CollectSvfgEdges(*view.svfg)) {
  auto source = ResolveValue(SvfValueForNode(edge->getSrcNode()), program_ir);
  auto target = ResolveValue(SvfValueForNode(edge->getDstNode()), program_ir);
  if (!source || !target) {
    facts.unknowns.push_back(MakeUnmappedSvfEdgeUnknown(
        edge->getEdgeKind(), run_context));
    continue;
  }
  facts.value_flows.push_back(MapValueFlow(
      *source, *target, edge->getEdgeKind(), run_context));
}

for (const auto& [left, right] : CandidatePointerPairs(facts.value_flows)) {
  facts.aliases.push_back(MapAliasResult(
      left, right, view.andersen->alias(left.svf_value, right.svf_value),
      run_context));
}
```

`CollectSvfgEdges` walks each node's outgoing edges and returns each edge once.
`CandidatePointerPairs` is built from pointer endpoints already related by a
load, store, GEP, call argument, return, or SVFG edge; never issue an all-pairs
alias query. The private candidate record retains callback-scoped `SVFValue*`
beside each VERITAS reference, but `MapAliasResult` returns only VERITAS types.

- [ ] **Step 6: Attach complete analyzer provenance**

Every fact cites `AnalyzerRunID`, pinned SVF commit, LLVM 22 version/ABI, adapter version, configuration bytes, whole-program module hash, and source/destination M4 origin references.

```cpp
summary::AnalyzerProvenance MakeSvfProvenance(
    const AnalyzerRunContext& run,
    const pipeline::ProgramIr& program_ir,
    const SvfConfig& config) {
  return summary::AnalyzerProvenance{
      .analyzer_run_id = run.analyzer_run_id,
      .analyzer_name = "veritas.svf",
      .analyzer_version = kSvfAdapterVersion,
      .dependency_revision =
          "18fb5650600530a54f0afc22f4df1a10b03d3c02",
      .toolchain_identity = run.llvm_toolchain_identity,
      .configuration_bytes = config.CanonicalAnalyzerConfig(),
      .input_module_hash = std::string(program_ir.module_hash()),
  };
}
```

- [ ] **Step 7: Sort and deduplicate mapped facts**

Sort by VERITAS subject/object IDs, predicate kind, field path, epistemic state, and provenance ID. Deduplicate only after mapping; never use SVF node allocation order as persistent or serialization order.

```cpp
template <typename Fact>
void CanonicalizeFacts(std::vector<Fact>* facts) {
  std::ranges::sort(*facts, {}, CanonicalFactKey<Fact>);
  facts->erase(std::ranges::unique(*facts, {}, CanonicalFactKey<Fact>).begin(),
               facts->end());
}

CanonicalizeFacts(&facts.value_flows);
CanonicalizeFacts(&facts.aliases);
CanonicalizeFacts(&facts.refined_memory_effects);
CanonicalizeFacts(&facts.refined_calls);
CanonicalizeFacts(&facts.unknowns);
CanonicalizeFacts(&facts.dependencies);
```

Implement a `CanonicalFactKey` overload for every listed fact type; each returns
the tuple of stable VERITAS fields listed above and excludes transient pointers.

- [ ] **Step 8: Add the mapper source to the private target**

```cmake
target_sources(veritas_svf_analysis PRIVATE SvfFactMapper.cpp)
```

- [ ] **Step 9: Run mapping tests and verify success**

Run: `ctest --test-dir build -R SvfFactMapperTest --output-on-failure`

Expected: parameter/return, store/load, field, and indirect-call cases pass with provenance.

- [ ] **Step 10: Commit SVF fact mapping**

```bash
git add src/analysis/svf/CMakeLists.txt src/analysis/svf/SvfFactMapper.* tests/fixtures/projects/parameter_return tests/fixtures/projects/store_load tests/fixtures/projects/field_access tests/fixtures/projects/function_pointer tests/integration/analysis/svf/SvfFactMapperTest.cpp
git commit -m "feat: map SVF facts into Summary IR"
```

---

### Task 4: Soft Budgets, Truncation, and Unknowns

**Files:**
- Modify: `src/analysis/svf/SvfFactMapper.cpp`
- Create: `src/analysis/svf/SvfBudget.h`
- Create: `src/analysis/svf/SvfBudget.cpp`
- Test: `tests/unit/analysis/svf/SvfBudgetTest.cpp`
- Test: `tests/integration/analysis/svf/SvfTruncationTest.cpp`

**Interfaces:**
- Consumes: `SvfConfig`, monotonic clock, observed graph-node count, and emitted-fact count
- Produces: `SvfBudgetState`
- Produces: scope-specific `UnknownFact` records and `SvfMappingCompletion::kCompleteWithUnknowns`

- [ ] **Step 1: Write failing budget tests**

```cpp
TEST(SvfBudgetTest, StopsEmissionAtFactLimitAndRecordsReason) {
  auto config = SvfConfig::Default();
  config.max_emitted_facts = 2;
  SvfBudget budget(config, FakeClock());
  EXPECT_TRUE(budget.TryEmit());
  EXPECT_TRUE(budget.TryEmit());
  EXPECT_FALSE(budget.TryEmit());
  EXPECT_EQ(budget.state().reason, BudgetReason::kFactLimit);
}

TEST(SvfTruncationTest, TruncatedMappingPublishesUnknownNotFalseNegative) {
  auto config = SvfConfig::Default();
  config.max_emitted_facts = 1;
  ASSERT_OK_AND_ASSIGN(auto result,
                       AnalyzeFixtureWithSvf("store_load", config));
  EXPECT_EQ(result.completion,
            SvfMappingCompletion::kCompleteWithUnknowns);
  EXPECT_HAS_UNKNOWN(result.facts.unknowns, UnknownKind::kAnalysisTruncated);
}
```

- [ ] **Step 2: Run budget tests and verify failure**

Run: `ctest --test-dir build -R "SvfBudget|SvfTruncation" --output-on-failure`

Expected: compilation fails because budget tracking is missing.

- [ ] **Step 3: Implement checkpoint-based budget tracking**

Track graph-node count before mapping, fact count before every emission, and elapsed monotonic time between supported phases. `soft_analysis_budget` records over-budget completion at the next safe checkpoint; it does not interrupt SVF inside an unsafe operation.

```cpp
enum class BudgetReason { kNone, kTimeLimit, kGraphNodeLimit, kFactLimit };

struct SvfBudgetState {
  BudgetReason reason = BudgetReason::kNone;
  std::size_t observed_graph_nodes = 0;
  std::size_t emitted_facts = 0;
  std::chrono::steady_clock::duration elapsed{};
};

class SvfBudget {
 public:
  using Now = std::function<std::chrono::steady_clock::time_point()>;
  SvfBudget(SvfConfig config, Now now);
  bool Checkpoint(std::size_t observed_graph_nodes);
  bool TryEmit();
  const SvfBudgetState& state() const;

 private:
  SvfConfig config_;
  Now now_;
  std::chrono::steady_clock::time_point started_;
  SvfBudgetState state_;
};

bool SvfBudget::Checkpoint(std::size_t observed_graph_nodes) {
  state_.observed_graph_nodes = observed_graph_nodes;
  state_.elapsed = now_() - started_;
  if (observed_graph_nodes > config_.max_graph_nodes) {
    state_.reason = BudgetReason::kGraphNodeLimit;
  } else if (state_.elapsed > config_.soft_analysis_budget) {
    state_.reason = BudgetReason::kTimeLimit;
  }
  return state_.reason == BudgetReason::kNone;
}

bool SvfBudget::TryEmit() {
  if (state_.reason != BudgetReason::kNone) return false;
  if (state_.emitted_facts >= config_.max_emitted_facts) {
    state_.reason = BudgetReason::kFactLimit;
    return false;
  }
  ++state_.emitted_facts;
  return true;
}
```

- [ ] **Step 4: Emit deterministic truncation unknowns**

When a supported limit is reached, stop mapping the affected remaining scope, retain already validated facts, add one `UnknownFact` per affected function/module with the exact limit, observed count, elapsed time, and `AnalyzerRunID`, then return `kCompleteWithUnknowns`.

```cpp
enum class SvfMappingCompletion { kComplete, kCompleteWithUnknowns };

struct SvfMappingResult {
  SvfMappingCompletion completion;
  SvfFacts facts;
};

SvfMappingResult CompleteWithTruncation(
    SvfFacts facts,
    const SvfBudgetState& state,
    const AnalyzerRunContext& run,
    std::span<const core::StableId> affected_functions) {
  for (const auto& function_id : affected_functions) {
    facts.unknowns.push_back(summary::MakeAnalysisTruncatedUnknown(
        function_id, BudgetReasonName(state.reason),
        state.observed_graph_nodes, state.emitted_facts, state.elapsed,
        run.analyzer_run_id));
  }
  CanonicalizeFacts(&facts.unknowns);
  return {SvfMappingCompletion::kCompleteWithUnknowns, std::move(facts)};
}
```

Change `MapSvfFacts` to return `StatusOr<SvfMappingResult>`. Call
`Checkpoint(ObservedGraphNodeCount(*view.svfg))` before edge mapping and at each
supported phase boundary, and call `TryEmit()` immediately before every fact is
appended. A soft limit never tears down an active SVF operation mid-call.

- [ ] **Step 5: Run budget and mapping tests**

Run: `ctest --test-dir build -R "SvfBudget|SvfTruncation|SvfFactMapper" --output-on-failure`

Expected: all tests pass and truncation never appears as an empty successful fact set.

- [ ] **Step 6: Commit budget semantics**

```bash
git add src/analysis/svf/SvfBudget.* src/analysis/svf/SvfFactMapper.cpp tests/unit/analysis/svf/SvfBudgetTest.cpp tests/integration/analysis/svf/SvfTruncationTest.cpp
git commit -m "feat: preserve unknowns at SVF budgets"
```

---

### Task 5: Required SVF Stage, Conservative Merge, and Publication

**Files:**
- Create: `include/veritas/analysis/ProjectAnalyzer.h`
- Create: `src/analysis/ProjectAnalyzer.cpp`
- Create: `src/analysis/ProjectAnalyzerInternal.h`
- Create: `src/analysis/svf/SvfAnalysisStage.h`
- Create: `src/analysis/svf/SvfAnalysisStage.cpp`
- Modify: `src/analysis/svf/CMakeLists.txt`
- Modify: `include/veritas/summary/LocalSummaryBuilder.h`
- Modify: `src/summary/LocalSummaryBuilder.cpp`
- Modify: `include/veritas/summarydb/SummaryRepository.h`
- Modify: `src/summarydb/SummaryRepository.cpp`
- Modify: `src/tools/veritas-build.cpp`
- Test: `tests/integration/analysis/ProjectAnalyzerSvfTest.cpp`
- Test: `tests/integration/build/VeritasBuildFullAnalysisTest.cpp`

**Interfaces:**
- Consumes: M1 request/manifest, M4 `LocalAnalysisResult`, M3 `SummaryRepository`, and Tasks 1-4
- Produces: public `ProjectAnalyzer::AnalyzeProject(ProjectAnalysisRequest, AnalysisConfig)`
- Produces: `ProjectAnalysisResult` containing completion, context ID, summary IDs, and unknowns
- Produces: the completed `veritas-build analyze --project <directory>` workflow

- [ ] **Step 1: Write failing required-stage integration tests**

```cpp
TEST(ProjectAnalyzerSvfTest, FullAnalysisAlwaysRunsSvfBeforePublication) {
  RecordingSummaryRepository repository;
  RecordingSvfStage svf_stage;
  auto analyzer = internal::ProjectAnalyzerTestFactory::Create(
      repository, svf_stage);
  ASSERT_OK_AND_ASSIGN(auto result, analyzer.AnalyzeProject(
      ProjectRequest("multiple_tus_flow"), AnalysisConfig::Default()));
  EXPECT_EQ(svf_stage.call_count(), 1u);
  EXPECT_GT(repository.publish_call_count(), 0u);
  EXPECT_LT(svf_stage.finished_sequence(), repository.first_publish_sequence());
  EXPECT_EQ(result.completion, AnalysisCompletion::kComplete);
}

TEST(ProjectAnalyzerSvfTest, FatalSvfFailurePublishesNothing) {
  RecordingSummaryRepository repository;
  FailingSvfStage svf_stage(Status::Internal("SVF construction failed"));
  auto analyzer = internal::ProjectAnalyzerTestFactory::Create(
      repository, svf_stage);
  auto result = analyzer.AnalyzeProject(
      ProjectRequest("multiple_tus_flow"), AnalysisConfig::Default());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(repository.publish_call_count(), 0u);
}
```

- [ ] **Step 2: Run integration tests and verify failure**

Run: `ctest --test-dir build -R "ProjectAnalyzerSvf|VeritasBuildFullAnalysis" --output-on-failure`

Expected: compilation fails because the project analyzer and required SVF stage are missing.

- [ ] **Step 3: Define the public project-level result only**

```cpp
namespace veritas::analysis {
enum class AnalysisCompletion { kComplete, kCompleteWithUnknowns };
namespace internal { class ProjectAnalyzerTestFactory; }

struct AnalysisConfig {
  std::chrono::seconds svf_soft_analysis_budget;
  std::size_t svf_max_graph_nodes;
  std::size_t svf_max_emitted_facts;

  static AnalysisConfig Default();
};

struct ProjectAnalysisResult {
  AnalysisCompletion completion;
  core::StableId program_context_id;
  std::vector<core::StableId> published_summary_ids;
  std::vector<summary::UnknownFact> unknowns;
};

class ProjectAnalyzer {
 public:
  ProjectAnalyzer();
  ~ProjectAnalyzer();
  ProjectAnalyzer(ProjectAnalyzer&&) noexcept;
  ProjectAnalyzer& operator=(ProjectAnalyzer&&) noexcept;
  ProjectAnalyzer(const ProjectAnalyzer&) = delete;
  ProjectAnalyzer& operator=(const ProjectAnalyzer&) = delete;

  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request,
      const AnalysisConfig& config);

 private:
  class Impl;
  explicit ProjectAnalyzer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class internal::ProjectAnalyzerTestFactory;
};
}
```

No public field or parameter contains a Clang, LLVM, or SVF type.

Define only the test seam in `ProjectAnalyzerInternal.h`:

```cpp
namespace veritas::analysis::internal {
class ProjectAnalyzerTestFactory {
 public:
  static ProjectAnalyzer Create(
      summarydb::SummaryRepository& repository,
      svf::SvfAnalysisStage& svf_stage);
};
}
```

Add `SummaryRepository::PublishProjectSummaries` as an atomic batch API returning
the published summary IDs. It must write every immutable object first and update
all current bindings in one SQLite transaction; no per-function publication loop
may expose a partially completed project.

- [ ] **Step 4: Implement the required ordered pipeline**

Resolve/load M1 input, persist M2 context, call M4 `RunLocalAnalysis`, create the analyzer-run context, call `RunWithSvfSession`, map facts inside its callback, merge facts into local drafts, then publish through M3 only after successful required-stage completion.

```cpp
StatusOr<ProjectAnalysisResult> ProjectAnalyzer::Impl::AnalyzeProject(
    const ProjectAnalysisRequest& request,
    const AnalysisConfig& config) {
  VERITAS_ASSIGN_OR_RETURN(auto input, build::ResolveProjectInput(request));
  VERITAS_ASSIGN_OR_RETURN(auto manifest,
                           build::LoadProjectManifest(input));
  VERITAS_ASSIGN_OR_RETURN(auto context_id,
                           PersistProgramContext(manifest.context));
  VERITAS_ASSIGN_OR_RETURN(auto local,
                           pipeline::RunLocalAnalysis(manifest));
  VERITAS_ASSIGN_OR_RETURN(auto run,
      CreateAnalyzerRunContext(manifest.context, local.program_ir,
                               ToSvfConfig(config)));
  VERITAS_ASSIGN_OR_RETURN(auto svf_result,
      svf_stage_.Analyze(local.program_ir, run, ToSvfConfig(config)));
  VERITAS_ASSIGN_OR_RETURN(auto merged,
      summary::MergeSvfFacts(std::move(local.summary_drafts),
                             svf_result.facts));
  VERITAS_ASSIGN_OR_RETURN(auto summary_ids,
      repository_.PublishProjectSummaries(context_id, merged));
  return ProjectAnalysisResult{
      .completion = ToAnalysisCompletion(svf_result.completion),
      .program_context_id = context_id,
      .published_summary_ids = std::move(summary_ids),
      .unknowns = std::move(svf_result.facts.unknowns),
  };
}
```

`SvfAnalysisStage::Analyze` is the only implementation of the required stage:

```cpp
class SvfAnalysisStage {
 public:
  virtual ~SvfAnalysisStage() = default;
  virtual StatusOr<SvfMappingResult> Analyze(
      pipeline::ProgramIr& program_ir,
      const AnalyzerRunContext& run,
      const SvfConfig& config);
};

StatusOr<SvfMappingResult> SvfAnalysisStage::Analyze(
    pipeline::ProgramIr& program_ir,
    const AnalyzerRunContext& run,
    const SvfConfig& config) {
  std::optional<SvfMappingResult> mapped;
  VERITAS_RETURN_IF_ERROR(RunWithSvfSession(
      program_ir, config, [&](const SvfSessionView& view) {
        VERITAS_ASSIGN_OR_RETURN(
            mapped, MapSvfFacts(program_ir, view, run, config));
        return Status::Ok();
      }));
  if (!mapped) return Status::Internal("SVF mapping callback did not run");
  return std::move(*mapped);
}
```

- [ ] **Step 5: Implement conservative merge rules**

Keep M4 direct `MUST_CALL` facts and Clang anchors. Add SVF value flows and aliases; refine unresolved indirect calls with bounded targets; add only conservative memory effects; append unknowns. Canonically sort/deduplicate merged components before calculating hashes and publishing.

```cpp
for (auto& draft : drafts) {
  AppendFactsForFunction(draft, svf_facts.value_flows);
  AppendFactsForFunction(draft, svf_facts.aliases);
  RefineUnknownCallsWithoutRemovingMustCalls(
      draft, svf_facts.refined_calls);
  AppendConservativeMemoryEffects(
      draft, svf_facts.refined_memory_effects);
  AppendFactsForFunction(draft, svf_facts.unknowns);
  CanonicalizeSummaryComponents(&draft);
}
```

- [ ] **Step 6: Route the standard command through `ProjectAnalyzer`**

Make `veritas-build analyze --project <directory> [--output <directory>]` construct `ProjectAnalysisRequest` and call `ProjectAnalyzer::AnalyzeProject`. Print completion state, translation units, summaries, mapped value-flow facts, alias facts, unknown count, and output directory. Add no alternate artifact-input command.

```cpp
analysis::ProjectAnalyzer analyzer;
analysis::ProjectAnalysisRequest request{
    .project_root = parsed.project,
    .output_root = parsed.output,
};
VERITAS_ASSIGN_OR_RETURN(auto result,
    analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default()));
PrintProjectAnalysisResult(result);
return EXIT_SUCCESS;
```

- [ ] **Step 7: Add the required stage source to the private target**

```cmake
target_sources(veritas_svf_analysis PRIVATE SvfAnalysisStage.cpp)
```

- [ ] **Step 8: Run project and CLI integration tests**

Run: `ctest --test-dir build -R "ProjectAnalyzerSvf|VeritasBuildFullAnalysis" --output-on-failure`

Expected: required SVF runs before publication, fatal failure publishes nothing, and the CLI completes from one project directory.

- [ ] **Step 9: Commit the complete project pipeline**

```bash
git add include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp src/analysis/ProjectAnalyzerInternal.h src/analysis/svf/CMakeLists.txt src/analysis/svf/SvfAnalysisStage.* include/veritas/summary/LocalSummaryBuilder.h src/summary/LocalSummaryBuilder.cpp include/veritas/summarydb/SummaryRepository.h src/summarydb/SummaryRepository.cpp src/tools/veritas-build.cpp tests/integration/analysis/ProjectAnalyzerSvfTest.cpp tests/integration/build/VeritasBuildFullAnalysisTest.cpp
git commit -m "feat: require SVF in project analysis"
```

---

### Task 6: Dependency, Boundary, and End-to-End Verification

**Files:**
- Create: `tests/integration/analysis/svf/RequiredSvfBoundaryTest.cpp`
- Create: `tests/integration/analysis/svf/RepeatedProjectAnalysisTest.cpp`
- Modify: `docs/third_party/SVF.md`

**Interfaces:**
- Consumes: the standard build, public headers, full `ProjectAnalyzer`, and pinned dependency metadata
- Produces: regression protection for required/pinned/in-process ownership

- [ ] **Step 1: Add boundary regression tests**

```cpp
TEST(RequiredSvfBoundaryTest, PublicApiContainsNoNativeAnalysisTypes) {
  EXPECT_FALSE(SourceTreeContains("include/veritas", "#include <SVF"));
  EXPECT_FALSE(SourceTreeContains("include/veritas", "#include <llvm"));
  EXPECT_FALSE(SourceTreeContains("include/veritas", "SVF::"));
  EXPECT_FALSE(SourceTreeContains("include/veritas", "llvm::Module"));
}

TEST(RepeatedProjectAnalysisTest, TwoProjectsRunDeterministicallyInOneProcess) {
  ProjectAnalyzer analyzer = MakeIntegrationAnalyzer();
  ASSERT_OK_AND_ASSIGN(auto first, analyzer.AnalyzeProject(
      ProjectRequest("parameter_return"), AnalysisConfig::Default()));
  ASSERT_OK_AND_ASSIGN(auto second, analyzer.AnalyzeProject(
      ProjectRequest("parameter_return"), AnalysisConfig::Default()));
  EXPECT_EQ(first.published_summary_ids, second.published_summary_ids);
}
```

- [ ] **Step 2: Document exact dependency metadata**

Record in `docs/third_party/SVF.md`:

```text
Path: third_party/SVF
Upstream: https://github.com/SVF-tools/SVF.git
Revision: 18fb5650600530a54f0afc22f4df1a10b03d3c02
License: AGPL-3.0-or-later; preserve third_party/SVF/LICENSE.TXT
Toolchain: LLVM/Clang 22.x, CMake 3.23+, compatible Z3
Initialize: git submodule update --init --recursive third_party/SVF
```

- [ ] **Step 3: Run the standard required build**

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build --target veritas-build
ctest --test-dir build --output-on-failure
```

Expected: configure, build, and all tests succeed with the pinned submodule and no enable flag.

- [ ] **Step 4: Verify the missing-submodule diagnostic in an isolated source copy**

```bash
svf_contract_tmp="$(mktemp -d)"
git clone --no-hardlinks . "$svf_contract_tmp/source"
if cmake -S "$svf_contract_tmp/source" -B "$svf_contract_tmp/build" \
    -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}" \
    >"$svf_contract_tmp/configure.stdout" \
    2>"$svf_contract_tmp/configure.stderr"; then
  echo "configuration unexpectedly succeeded without initialized SVF" >&2
  exit 1
fi
rg -F 'git submodule update --init --recursive third_party/SVF' \
  "$svf_contract_tmp/configure.stdout" \
  "$svf_contract_tmp/configure.stderr"
```

Expected: configuration fails and prints `git submodule update --init --recursive third_party/SVF`.

- [ ] **Step 5: Verify no optional or artifact-driven path remains**

```bash
if rg -n 'VERITAS_ENABLE_SVF|FindSVF|find_package\(SVF|SVF disabled|--(compile-db|manifest|bitcode|llvm-module|svf-input)' CMakeLists.txt cmake/Dependencies.cmake src include; then
  exit 1
fi
```

- [ ] **Step 6: Run the end-to-end project fixture twice**

```bash
veritas_full_output_1="$(mktemp -d)"
veritas_full_output_2="$(mktemp -d)"
./build/src/tools/veritas-build analyze \
  --project tests/fixtures/projects/multiple_tus_flow \
  --output "$veritas_full_output_1"
./build/src/tools/veritas-build analyze \
  --project tests/fixtures/projects/multiple_tus_flow \
  --output "$veritas_full_output_2"
diff -ru "$veritas_full_output_1" "$veritas_full_output_2"
```

Expected: both runs succeed and all canonical outputs are identical.

- [ ] **Step 7: Commit boundary verification and dependency documentation**

```bash
git add tests/integration/analysis/svf/RequiredSvfBoundaryTest.cpp tests/integration/analysis/svf/RepeatedProjectAnalysisTest.cpp docs/third_party/SVF.md
git commit -m "test: verify required in-process SVF pipeline"
```

---

## Milestone Verification

- [ ] Run the full standard build and tests:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON -DLLVM_PROJECT_BUILD_DIR="${LLVM_PROJECT_BUILD_DIR}"
cmake --build build
ctest --test-dir build --output-on-failure
```

- [ ] Run ownership scans:

```bash
if rg -n '#include <(SVF|llvm)/|SVF::|llvm::Module' include/veritas; then
  exit 1
fi
if rg -n 'VERITAS_ENABLE_SVF|FindSVF|--(compile-db|manifest|bitcode|llvm-module|svf-input)' CMakeLists.txt cmake/Dependencies.cmake src include; then
  exit 1
fi
```

- [ ] Run deterministic end-to-end analysis twice and compare outputs:

```bash
veritas_full_output_1="$(mktemp -d)"
veritas_full_output_2="$(mktemp -d)"
./build/src/tools/veritas-build analyze \
  --project tests/fixtures/projects/multiple_tus_flow \
  --output "$veritas_full_output_1"
./build/src/tools/veritas-build analyze \
  --project tests/fixtures/projects/multiple_tus_flow \
  --output "$veritas_full_output_2"
diff -ru "$veritas_full_output_1" "$veritas_full_output_2"
```

- [ ] Verify the branch is clean:

```bash
git diff --check
git status --short
```
