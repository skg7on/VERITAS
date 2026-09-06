# Build Warning Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the canonical clean Debug build complete without configure, compiler, or linker warnings while preserving the pinned third-party revisions and runtime behavior.

**Architecture:** Correct redundant CMake dependency declarations at their source, initialize SVF ABI options from the already-discovered LLVM configuration, and keep unavoidable pinned/generated Souffle diagnostics suppressed only on the affected third-party targets. Do not weaken the warning-as-error contract for VERITAS-owned code.

**Tech Stack:** CMake 3.23+, Ninja, C++20, AppleClang/Clang/GNU-compatible warning flags, LLVM/Clang 24, vendored SVF and Souffle.

**Spec:** `CLAUDE.md` and the user-requested zero-warning clean build.

## Global Constraints

- Build from a dedicated worktree based directly on synchronized `main`; never mutate the primary checkout.
- Use `cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build` and `cmake --build --preset default`.
- Preserve `-Werror` for all VERITAS-owned targets.
- Preserve the pinned SVF and Souffle source revisions; third-party warning handling must be target-scoped.
- Preserve LLVM-compatible RTTI and exception settings.

---

### Task 1: Remove all canonical clean-build warnings

**Files:**
- Modify: `cmake/VeritasSVF.cmake`
- Modify: `cmake/VeritasSouffle.cmake`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `docs/plans/README.md`
- Create: `docs/plans/build-warning-cleanup-implementation-plan.md`

**Interfaces:**
- Consumes: `LLVM_ENABLE_RTTI`, `LLVM_ENABLE_EH`, the `libsouffle` target, generated Souffle object-library targets, `veritas::summary`, and `GTest::gtest_main`.
- Produces: the same public library and executable targets with warning-free configure, compile, and link command lines.

- [x] **Step 1: Reproduce the warning-bearing baseline**

Run:

```bash
rm -rf build
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default 2>&1 | tee build/baseline-build.log
```

Expected: configuration warns while force-disabling SVF RTTI; compilation reports pinned/generated Souffle diagnostics; linking reports duplicate `veritas_analysis_semantic` and GTest archives.

- [x] **Step 2: Initialize SVF ABI options before adding the vendored project**

Add to `cmake/VeritasSVF.cmake` before `add_subdirectory(...)`:

```cmake
set(SVF_ENABLE_RTTI ${LLVM_ENABLE_RTTI} CACHE BOOL
    "Match SVF RTTI to LLVM" FORCE)
set(SVF_ENABLE_EXCEPTIONS ${LLVM_ENABLE_EH} CACHE BOOL
    "Match SVF exception handling to LLVM" FORCE)
```

This prevents SVF from announcing incompatible defaults and then repairing them during its own configuration.

- [x] **Step 3: Scope unavoidable Souffle warning suppressions to third-party targets**

For Clang-family and GNU compilers, add `-Wno-pessimizing-move` and `-Wno-unused-but-set-variable` only to `libsouffle`, and add `-Wno-deprecated-declarations` only to the generated Souffle bundle object libraries. Leave VERITAS-owned warning options unchanged.

- [x] **Step 4: Remove redundant direct link dependencies**

Remove `veritas_analysis_semantic` from `veritas_facts` because `veritas::summary` already exposes it publicly. Remove the same direct dependency from `veritas_analysis` because its private component libraries already carry it to the final link. Remove `GTest::gtest` from `summary_v2_pipeline_integration_test` because `GTest::gtest_main` already links it transitively.

- [x] **Step 5: Verify the warning-clean build from scratch**

Run:

```bash
rm -rf build
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default 2>&1 | tee build/verified-build.log
```

Expected: both commands exit 0, all 575 build steps complete, and `rg -n '(warning:|CMake Warning)' build/verified-build.log` reports no matches. Inspect configure output separately for `CMake Warning`.

- [x] **Step 6: Run regression and repository-policy checks**

Run:

```bash
ctest --preset default --output-on-failure
git diff --check
```

Expected: all 364 tests pass, no whitespace errors are reported, and every modified CMake file retains its Apache-2.0 header.

- [x] **Step 7: Commit the verified change**

```bash
git add cmake/VeritasSVF.cmake cmake/VeritasSouffle.cmake src/facts/CMakeLists.txt src/analysis/CMakeLists.txt tests/integration/analysis/CMakeLists.txt docs/plans/README.md docs/plans/build-warning-cleanup-implementation-plan.md
git commit -m "build: eliminate clean-build warnings"
```
