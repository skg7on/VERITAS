# M4 Clang/LLVM Local Extraction Adapter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract local C/C++ semantic facts through Clang and LLVM and publish real FunctionSummary objects.

**Architecture:** Keep Clang and LLVM behind adapters. Convert AST declarations, source anchors, LLVM values, direct calls, local memory effects, and local value-flow facts into VERITAS IDs and Summary IR without persisting tool-native pointers.

**Tech Stack:** C++20, Clang LibTooling, LLVM analysis APIs, Protobuf summaries, GoogleTest.

**Spec:** `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md`

## Global Constraints

- Do not persist Clang AST node pointers or LLVM `Value*` addresses.
- M4 emits direct and local facts only.
- Clang owns language semantics; VERITAS owns stable refs and summary facts.
- Function identity uses M2 APIs.
- Published summaries use M3 `SummaryRepository`.

---

### Task 1: Clang Declaration Extractor

**Files:**
- Create: `include/veritas/frontend/clang/ClangExtractor.h`
- Create: `include/veritas/frontend/clang/SourceAnchorBuilder.h`
- Create: `src/frontend/clang/ClangExtractor.cpp`
- Create: `src/frontend/clang/SourceAnchorBuilder.cpp`
- Create: `tests/fixtures/cpp/functions/overloads.cpp`
- Create: `tests/fixtures/cpp/functions/templates.cpp`
- Create: `tests/fixtures/cpp/functions/internal_a.cpp`
- Create: `tests/fixtures/cpp/functions/internal_b.cpp`
- Test: `tests/integration/frontend/ClangExtractorTest.cpp`

**Interfaces:**
- Produces: `ExtractedFunctionDecl`
- Produces: `ClangExtractor::ExtractDeclarations`
- Consumes: M1 `TranslationUnitCommand`

- [ ] **Step 1: Write declaration extraction tests**

```cpp
TEST(ClangExtractorTest, DistinguishesOverloadsAndInternalLinkage) {
  auto decls = ExtractFixtureDeclarations("tests/fixtures/cpp/functions");
  EXPECT_HAS_DISTINCT_SYMBOLS(decls, "overloaded(int)", "overloaded(double)");
  EXPECT_INTERNAL_LINKAGE_DOES_NOT_COLLIDE(decls, "internal_a.cpp", "internal_b.cpp");
}
```

- [ ] **Step 2: Run frontend tests**

Run: `ctest --test-dir build -R ClangExtractorTest --output-on-failure`

Expected: fail because extractor is missing.

- [ ] **Step 3: Implement Clang `FrontendAction`**

Visit function declarations and definitions, collect qualified name, mangled name, canonical signature, linkage, template identity, and source anchor.

- [ ] **Step 4: Implement source anchor builder**

Store repository-relative path, line/column range, spelling location, expansion location, and macro expansion stack.

- [ ] **Step 5: Run frontend tests again**

Expected: pass for declaration and source anchor extraction.

---

### Task 2: LLVM Local Fact Extractor

**Files:**
- Create: `include/veritas/analysis/llvm/LlvmExtractor.h`
- Create: `include/veritas/analysis/llvm/ValueRef.h`
- Create: `src/analysis/llvm/LlvmExtractor.cpp`
- Create: `src/analysis/llvm/ValueRef.cpp`
- Create: `tests/fixtures/cpp/local_facts/direct_call.cpp`
- Create: `tests/fixtures/cpp/local_facts/memcpy_path.cpp`
- Create: `tests/fixtures/cpp/local_facts/function_pointer.cpp`
- Test: `tests/integration/analysis/LlvmExtractorTest.cpp`

**Interfaces:**
- Produces: `LocalIrFacts`
- Produces: `LlvmExtractor::ExtractLocalFacts`
- Produces: `ValueRef` and `MemoryRef`

- [ ] **Step 1: Write LLVM fact tests**

```cpp
TEST(LlvmExtractorTest, EmitsDirectCallAndMemcopyFacts) {
  auto facts = ExtractLlvmFacts("tests/fixtures/cpp/local_facts/memcpy_path.cpp");
  EXPECT_HAS_CALL(facts, "memcpy", "MUST_CALL");
  EXPECT_HAS_MEMORY_WRITE(facts, "arg1.data");
}
```

- [ ] **Step 2: Run LLVM tests**

Run: `ctest --test-dir build -R LlvmExtractorTest --output-on-failure`

Expected: fail because LLVM extractor is missing.

- [ ] **Step 3: Generate or load LLVM module**

Use the compile command from M1 and keep module creation inside `analysis/llvm`.

- [ ] **Step 4: Extract direct calls and unresolved calls**

Emit `MUST_CALL` for direct calls and `UNKNOWN_CALL` for unresolved function pointer targets.

- [ ] **Step 5: Extract CFG summary facts**

Use LLVM dominator analysis for summary-level reachability and domination facts.

- [ ] **Step 6: Extract memory and local flow facts**

Handle loads, stores, calls to memcpy-like functions, returns, parameters, phi nodes, and select instructions.

- [ ] **Step 7: Run LLVM tests again**

Expected: pass.

---

### Task 3: Local Summary Builder

**Files:**
- Create: `include/veritas/summary/LocalSummaryBuilder.h`
- Create: `src/summary/LocalSummaryBuilder.cpp`
- Test: `tests/integration/summary/LocalSummaryBuilderTest.cpp`
- Modify: `src/tools/veritas-build.cpp`

**Interfaces:**
- Consumes: `ExtractedFunctionDecl`
- Consumes: `LocalIrFacts`
- Produces: `BuildLocalSummary`
- Produces: `veritas-build index --local-only`

- [ ] **Step 1: Write local summary test**

Build a summary for `memcpy_path.cpp` and assert calls, value-flow, memory effects, unknowns, dependencies, and source anchors exist.

- [ ] **Step 2: Implement `BuildLocalSummary`**

Merge Clang declaration data and LLVM local facts into `summary::v1::FunctionSummary`.

- [ ] **Step 3: Publish summaries**

Use M3 `SummaryRepository::PublishSummary`.

- [ ] **Step 4: Add CLI command**

Support:

```text
veritas-build index --manifest <manifest.json> --local-only --db <path>
```

- [ ] **Step 5: Run local summary integration tests**

Run: `ctest --test-dir build -R LocalSummaryBuilderTest --output-on-failure`

Expected: pass.

---

### Task 4: Milestone Verification

**Files:**
- Modify: none

**Interfaces:**
- Consumes: all M4 extraction APIs
- Produces: real local summaries from fixtures

- [ ] **Step 1: Run M4 tests**

Run: `ctest --test-dir build -R "ClangExtractor|LlvmExtractor|LocalSummaryBuilder" --output-on-failure`

- [ ] **Step 2: Run smoke local index**

Run:

```bash
./build/src/tools/veritas-build index \
  --manifest /tmp/veritas-smoke-manifest.json \
  --local-only \
  --db /tmp/veritas-smoke.db
```

- [ ] **Step 3: Commit**

```bash
git add include/veritas/frontend include/veritas/analysis/llvm include/veritas/summary src/frontend src/analysis/llvm src/summary src/tools/veritas-build.cpp tests/fixtures/cpp tests/integration
git commit -m "feat: extract local Clang LLVM summaries"
```

