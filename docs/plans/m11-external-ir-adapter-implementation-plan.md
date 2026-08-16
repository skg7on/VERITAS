# M11: External IR Adapter — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Accept external LLVM IR artifacts (`.bc`, `.ll`, a directory of either) as the module source for `veritas-build analyze --bitcode`, skipping Clang CodeGen but running VERITAS's own SVF/analysis unchanged.

**Architecture:** Introduce a `ProgramIrSource` abstraction at M4's module-acquisition boundary with two implementations — `CodeGenIrSource` (existing CodeGen + link) and `BitcodeIrSource` (new: parse bitcode/IR, verify, link, build origin map). Everything downstream (`LocalFactExtractor`, SVF, CPG, WPA, provenance) is untouched.

**Tech Stack:** C++20, LLVM 22 (`BitcodeReader`, `AsmParser`, `Linker`), CMake, GoogleTest.

**Spec:** `docs/specs/veritas-summarydb-ingest-adapter-design.md` (milestone-scoped signatures + tests)
**Architecture:** `docs/architecture/veritas-platform-architecture-design.md` (§6–§11 adapter tiers, invariant B10)

## Global Constraints

- Function identity is semantic, not file-line based (backbone B1).
- External IR artifacts enter at the module-acquisition boundary only; they never bypass VERITAS's own analysis, identity, or provenance derivation (rewritten B10).
- Fidelity tiers: **T0** (debug info) → source anchors preserved; **T1** (symbols, no debug) → analyze with no source anchors; **T2** (stripped) → reject with a clear error.
- LLVM native pointer/ID identity is never persisted (no `Value*` escapes into nodes/edges/IDs).
- `--project` and `--bitcode` are mutually exclusive; exactly one is required.
- Installed public headers expose no LLVM native types (LLVM stays private to `src/`).
- This milestone depends on **M4** (`ProgramIr`, `OriginMap`, `LocalFactExtractor`) and **M5** (SVF pipeline); it is a forward plan executed after those land.

---

## Files

- Create: `include/veritas/analysis/pipeline/ProgramIrSource.h`
- Create: `src/analysis/pipeline/CodeGenIrSource.h`
- Create: `src/analysis/pipeline/CodeGenIrSource.cpp`  (refactor existing M4 CodeGen out of `LocalAnalysisStage`)
- Create: `src/analysis/ir_adapter/BitcodeFidelity.h`
- Create: `src/analysis/ir_adapter/BitcodeInput.h`
- Create: `src/analysis/ir_adapter/BitcodeModuleLoader.h`
- Create: `src/analysis/ir_adapter/BitcodeModuleLoader.cpp`
- Create: `src/analysis/ir_adapter/BitcodeIrSource.h`
- Create: `src/analysis/ir_adapter/BitcodeIrSource.cpp`
- Modify: `src/analysis/pipeline/LocalAnalysisStage.cpp`  (accept `ProgramIrSource&`)
- Modify: `src/tools/veritas-build.cpp`  (add `--bitcode <path>` flag)
- Test: `tests/integration/analysis/ir_adapter/BitcodeModuleLoaderTest.cpp`
- Test: `tests/integration/analysis/ir_adapter/BitcodeIrSourceTest.cpp`
- Test: `tests/integration/analysis/ir_adapter/VeritasBuildBitcodeCliTest.cpp`
- Test: `tests/fixtures/cpp/bitcode/simple.c` (compiled to `simple.bc`/`simple.ll` in the test harness)

## Interfaces

```cpp
// include/veritas/analysis/pipeline/ProgramIrSource.h
namespace veritas::analysis::pipeline {
class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual veritas::StatusOr<ProgramIr> Build() = 0;
};
}
```

```cpp
// src/analysis/ir_adapter/BitcodeFidelity.h
namespace veritas::analysis::ir_adapter {
enum class BitcodeFidelity { DebugInfo, SymbolsOnly, Stripped };
}
```

```cpp
// src/analysis/ir_adapter/BitcodeInput.h
namespace veritas::analysis::ir_adapter {
struct BitcodeInput {
  enum class Kind { SingleFile, Directory };
  Kind kind;
  std::filesystem::path path;   // file, or directory to enumerate *.bc / *.ll
};
}
```

```cpp
// src/analysis/ir_adapter/BitcodeModuleLoader.h
namespace veritas::analysis::ir_adapter {
class BitcodeModuleLoader {
 public:
  veritas::StatusOr<std::vector<std::unique_ptr<llvm::Module>>> LoadAll(
      const BitcodeInput& input);
  BitcodeFidelity DetectFidelity(llvm::Module& module);   // T0/T1/T2
};

veritas::StatusOr<veritas::analysis::pipeline::ProgramIr> LinkIntoProgramIr(
    std::vector<std::unique_ptr<llvm::Module>> modules);
veritas::StatusOr<veritas::analysis::pipeline::OriginMap> BuildOriginMap(
    llvm::Module& module, BitcodeFidelity fidelity);
}
```

```cpp
// src/analysis/ir_adapter/BitcodeIrSource.h
namespace veritas::analysis::ir_adapter {
class BitcodeIrSource : public veritas::analysis::pipeline::ProgramIrSource {
 public:
  explicit BitcodeIrSource(BitcodeInput input);
  veritas::StatusOr<veritas::analysis::pipeline::ProgramIr> Build() override;
 private:
  BitcodeInput input_;
};
}
```

`CodeGenIrSource` has the same `Build()` signature and is the existing M4 CodeGen + link logic moved behind the interface. `ProgramIr` exposes `module()` (→ `llvm::Module&`) and `origin_map()` (→ `OriginMap`), as specified by M4.

## Implementation Plan

### Task 1: `ProgramIrSource` interface and `CodeGenIrSource` refactor

- [ ] Create `ProgramIrSource.h` with the abstract `Build()` interface above.
- [ ] Move M4's existing CodeGen + link logic out of `LocalAnalysisStage` into `CodeGenIrSource::Build()`, preserving current behavior byte-for-byte.
- [ ] Change `LocalAnalysisStage` to `RunLocalAnalysis(ProgramIrSource&)` → `StatusOr<LocalAnalysisResult>`, where `LocalAnalysisResult` carries the `ProgramIr` returned by the source plus the extracted `summary_drafts`.
- [ ] Run the existing M4 integration tests (`LocalAnalysisStageTest`, `ProjectIrBuilderTest`); they must pass unchanged. Expected: PASS (pure refactor, no behavior change).
- [ ] Commit: `refactor: extract ProgramIrSource behind module acquisition`.

### Task 2: `BitcodeFidelity` detection

- [ ] Write `DetectFidelity` tests first: a module built from a C fixture with `-g` → `DebugInfo`; same fixture without `-g` but with symbols → `SymbolsOnly`; a stripped module → `Stripped`.
- [ ] Implement `DetectFidelity`: check for `llvm.dbg` metadata / `DISubprogram` (T0), else check for named function symbols (T1), else T2.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: detect bitcode fidelity tier`.

### Task 3: `BitcodeModuleLoader` — parse and verify

- [ ] Write a test that loads a `.bc` file and an `.ll` file into `std::vector<std::unique_ptr<llvm::Module>>`, asserting the function count matches.
- [ ] Implement `LoadAll`: enumerate a single file or `*.bc`/`*.ll` in a directory; dispatch `.bc` → `llvm::parseBitcodeFile`, `.ll` → `llvm::parseAssemblyFile`; verify each module (`llvm::verifyModule`); fail with `Status::InvalidArgument` on parse/verify error. Detect an LLVM-version-mismatched bitcode header and return `Status::FailedPrecondition` with a clear message (never a silent downgrade).
- [ ] Write a test that a malformed `.ll` returns `Status::InvalidArgument` and loads no partial module.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: load external bitcode modules`.

### Task 4: `LinkIntoProgramIr` and `BuildOriginMap`

- [ ] Write a test that links two `.bc` modules (each defining one function) into one `ProgramIr` whose `module()` contains both functions.
- [ ] Implement `LinkIntoProgramIr`: `llvm::Linker::linkModules` into a fresh module; report duplicate-symbol errors fatally (never silently merge).
- [ ] Write `BuildOriginMap` tests: T0 module → source anchors present for a function; T1 module → empty origin map, no source anchors.
- [ ] Implement `BuildOriginMap`: T0 reads `DISubprogram`/`DILocation` to build source anchors; T1 returns an empty origin map.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: link external bitcode and build origin map`.

### Task 5: `BitcodeIrSource` end-to-end

- [ ] Write a test that `BitcodeIrSource{BitcodeInput{Directory, dir}}.Build()` returns a `ProgramIr` whose `module()` and `origin_map()` reflect the inputs, and that records `BitcodeFidelity` into the analyzer-run/provenance context.
- [ ] Write a test that a stripped (T2) module makes `Build()` return `Status::FailedPrecondition` with a message containing "no stable symbol identity".
- [ ] Implement `BitcodeIrSource::Build()`: `LoadAll` → `DetectFidelity` (reject T2) → `LinkIntoProgramIr` → `BuildOriginMap`; stamp fidelity into the run context.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: add BitcodeIrSource`.

### Task 6: CLI `--bitcode`

- [ ] Add `--bitcode <path>` to `veritas-build analyze`; reject `--project` + `--bitcode` together with a usage error; require exactly one.
- [ ] Wire `--bitcode` → `BitcodeIrSource`, `--project` → `CodeGenIrSource`, then the shared `ProjectAnalyzer` pipeline (M5).
- [ ] Write a CLI integration test: `veritas-build analyze --bitcode tests/fixtures/.../simple.bc` succeeds and publishes summaries; `--project <dir> --bitcode <path>` fails with a usage error.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: accept external bitcode input via --bitcode`.

### Task 7: Revise invariant and milestone docs

- [ ] Rewrite backbone invariant **B10** in `docs/specs/veritas-engineering-backbone-design-specification.md` per design doc §2 (allow bitcode at module acquisition + external facts via importer, without bypassing VERITAS-owned analysis/identity/provenance).
- [ ] Relax the M4 "public CLI accepts no bitcode" test/constraint in `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md` to permit bitcode only via module acquisition.
- [ ] Update `docs/specs/veritas-backbone-milestones-and-implementation-plan.md` "Global Constraints" and M4 exit criteria to drop the "no bitcode input" absolutism.
- [ ] Commit: `docs: revise B10 and M4 for external IR input`.

## Tests (required assertions)

```text
.bc with debug info -> source anchors present; FunctionSymbolID matches mangled name
.ll (textual) -> identical result to .bc of the same module
directory of modules -> linked into one ProgramIr, all functions present
T1 (symbols, no debug) -> analysis succeeds, no source anchors
T2 (stripped) -> rejected with "no stable symbol identity"
LLVM version mismatch -> clean FailedPrecondition, no silent downgrade
duplicate symbol across modules -> fatal, reported
determinism: same bitcode twice -> identical summaries + ProjectionID
--project and --bitcode together -> usage error
public headers expose no LLVM native types (boundary scan)
```

## Exit Criteria

```text
veritas-build analyze --bitcode <path> builds and publishes the same SummaryDB
  as --project, using external LLVM IR as the module source.
External bitcode is analyzed by VERITAS's own SVF/analysis; CodeGen is the only
  skipped stage.
Fidelity T0/T1/T2 policy is enforced; stripped input is rejected.
No LLVM native identity escapes into summaries, IDs, or public headers.
```
