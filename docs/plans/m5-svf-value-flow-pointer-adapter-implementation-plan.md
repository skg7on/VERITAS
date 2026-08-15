# M5 SVF Value-Flow and Pointer Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional SVF adapter that improves value-flow, alias, and memory-effect facts without leaking SVF-native types into VERITAS public APIs.

**Architecture:** Gate SVF behind `VERITAS_ENABLE_SVF`. Convert SVF nodes, points-to answers, and value-flow edges into VERITAS `ValueRef`, `MemoryRef`, `AliasFact`, and `ValueFlowFact` records. Preserve unknowns and timeouts explicitly.

**Tech Stack:** C++20, LLVM, SVF, CMake optional dependency detection, GoogleTest.

**Spec:** `docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md`

## Global Constraints

- SVF is optional at build time.
- No public header outside `include/veritas/analysis/svf` includes SVF headers.
- SVF node IDs are not persisted as VERITAS IDs.
- Timeout or unsupported analysis emits `UnknownFact`.
- SVF augments M4 summaries and does not delete M4 `MUST` facts.

---

### Task 1: Optional SVF Build Integration

**Files:**
- Modify: `cmake/FindSVF.cmake`
- Modify: `cmake/Dependencies.cmake`
- Create: `include/veritas/analysis/svf/SvfConfig.h`
- Create: `src/analysis/svf/SvfConfig.cpp`
- Test: `tests/unit/analysis/svf/SvfConfigTest.cpp`

**Interfaces:**
- Produces: `VERITAS_ENABLE_SVF`
- Produces: `SvfConfig`

- [ ] **Step 1: Write config test**

```cpp
TEST(SvfConfigTest, DefaultsToBoundedAndUnknownPreserving) {
  auto config = veritas::analysis::svf::SvfConfig::Default();
  EXPECT_TRUE(config.emit_unknowns_on_timeout);
  EXPECT_GT(config.max_analysis_seconds, 0);
}
```

- [ ] **Step 2: Add CMake option**

Add `option(VERITAS_ENABLE_SVF "Enable SVF adapter" OFF)`.

- [ ] **Step 3: Implement SVF detection**

When enabled and not found, configure must fail with a clear message.

- [ ] **Step 4: Run config test with SVF disabled**

Run: `ctest --test-dir build -R SvfConfigTest --output-on-failure`

Expected: pass.

---

### Task 2: SVF Adapter Boundary

**Files:**
- Create: `include/veritas/analysis/svf/SvfAdapter.h`
- Create: `src/analysis/svf/SvfAdapter.cpp`
- Test: `tests/integration/analysis/svf/SvfAdapterDisabledTest.cpp`
- Test: `tests/integration/analysis/svf/SvfAdapterTest.cpp`

**Interfaces:**
- Produces: `SvfAdapter::AnalyzeModule`
- Produces: `SvfFacts`
- Consumes: M4 `LlvmModuleInput`

- [ ] **Step 1: Write disabled-build test**

Build with `VERITAS_ENABLE_SVF=OFF` and assert non-SVF targets compile. The adapter target is not linked.

- [ ] **Step 2: Write enabled adapter test**

When SVF is available, analyze a fixture and expect at least one value-flow fact.

- [ ] **Step 3: Implement adapter target isolation**

Place all SVF includes in `src/analysis/svf/SvfAdapter.cpp`.

- [ ] **Step 4: Add include-boundary check**

Run: `rg -n "SVF|WPA|SVFIR" include/veritas | rg -v "include/veritas/analysis/svf"`

Expected: no output.

---

### Task 3: Fact Mapping and Merge

**Files:**
- Create: `include/veritas/analysis/svf/SvfFactMapper.h`
- Create: `src/analysis/svf/SvfFactMapper.cpp`
- Create: `tests/fixtures/cpp/svf/parameter_return.cpp`
- Create: `tests/fixtures/cpp/svf/store_load.cpp`
- Create: `tests/fixtures/cpp/svf/field_access.cpp`
- Test: `tests/integration/analysis/svf/SvfFactMapperTest.cpp`
- Modify: `include/veritas/summary/LocalSummaryBuilder.h`
- Modify: `src/summary/LocalSummaryBuilder.cpp`

**Interfaces:**
- Produces: `MapSvfFactsToSummaryFacts`
- Consumes: `ValueRef`, `MemoryRef`, `summary::v1::FunctionSummary`

- [ ] **Step 1: Write mapping tests**

```cpp
TEST(SvfFactMapperTest, MapsParameterToReturnFlow) {
  auto facts = AnalyzeSvfFixture("parameter_return.cpp");
  EXPECT_HAS_VALUE_FLOW(facts.value_flows, "arg0", "return");
}
```

- [ ] **Step 2: Implement value-flow mapping**

Translate SVF value-flow nodes through M4 `ValueRef` mapping.

- [ ] **Step 3: Implement alias mapping**

Emit `MUST_ALIAS`, `MAY_ALIAS`, `NO_ALIAS`, and `UNKNOWN_ALIAS`.

- [ ] **Step 4: Implement summary merge**

Add SVF facts to ValueFlow, AliasFacts, MemoryEffects, Unknowns, Dependencies, and Provenance components.

- [ ] **Step 5: Run mapping tests**

Run: `ctest --test-dir build -R "SvfAdapter|SvfFactMapper" --output-on-failure`

Expected: pass when SVF is enabled; disabled tests pass when SVF is off.

---

### Task 4: Timeout and Unknown Policy

**Files:**
- Modify: `src/analysis/svf/SvfAdapter.cpp`
- Test: `tests/integration/analysis/svf/SvfTimeoutTest.cpp`

**Interfaces:**
- Consumes: `SvfConfig`
- Produces: timeout-scoped `UnknownFact`

- [ ] **Step 1: Write injected timeout test**

Use a fake adapter clock or test hook to force a timeout and assert an `UnknownFact` is emitted.

- [ ] **Step 2: Implement timeout budget checks**

Emit function- or module-scoped unknowns on timeout.

- [ ] **Step 3: Run timeout tests**

Run: `ctest --test-dir build -R SvfTimeoutTest --output-on-failure`

Expected: pass.

---

### Task 5: Milestone Verification

- [ ] **Step 1: Run disabled build tests**

Run:

```bash
cmake -S . -B build-nosvf -DVERITAS_ENABLE_SVF=OFF
cmake --build build-nosvf
ctest --test-dir build-nosvf --output-on-failure
```

- [ ] **Step 2: Run enabled tests when SVF is installed**

Run:

```bash
cmake -S . -B build-svf -DVERITAS_ENABLE_SVF=ON
cmake --build build-svf
ctest --test-dir build-svf -R "Svf" --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add cmake include/veritas/analysis/svf src/analysis/svf include/veritas/summary src/summary tests/fixtures/cpp/svf tests/unit/analysis tests/integration/analysis
git commit -m "feat: add SVF value-flow adapter"
```

