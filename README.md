<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/veritas-logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="docs/assets/veritas-logo.png">
    <img src="docs/assets/veritas-logo.png" alt="VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics" width="50%">
  </picture>
</p>

# VERITAS

**Verified Evidence Reasoning IR for Trans-program Analysis and Semantics**

<p align="center">
  <a href="https://github.com/skg7on/VERITAS/actions/workflows/ci.yml"><img src="https://github.com/skg7on/VERITAS/actions/workflows/ci.yml/badge.svg?branch=main" alt="CI Build"></a>
</p>

---

## Overview

VERITAS is an evidence-centric whole-program analysis platform that combines deterministic static analysis with LLM-assisted semantic reasoning.

**Core Thesis:** A Review Agent should never be asked to "understand a million-line repository." Deterministic analysis first builds a compact, immutable, provenance-carrying semantic world. The Agent reasons only over that structured representation (Evidence IR), and any hypothesis it produces must round-trip through a deterministic verifier before becoming a fact.

**Three IRs:**

1. **Function Summary IR** — externally visible semantic effects of a function (calls, memory reads/writes, value flows, ranges, aliases, locks, state transitions, unknowns). Immutable, content-addressed, component-hashed.

2. **SummaryDB** — a logical subsystem across seven physical layers: Object Store · Metadata · Fact Store · Graph Index · Dependency Index · Evidence Cache · History Store.

3. **Evidence IR (EIR)** — claim-oriented, provenance-preserving typed graph IR the Agent consumes. Enforces MUST / MAY / MUST_NOT / INFERRED / ASSUMED / UNKNOWN as distinct epistemic states.

**Pipeline:** Ingest (compile_commands.json, bitcode, or external analysis) → module acquisition → local static analysis → Function Summary IR → SummaryDB → Incremental WPA (SCC + fixpoint + Datalog) → global derived facts → Evidence Builder → Evidence IR → Agent + Proof Engines → Review Result.

---

## Key Features

- **Immutable summaries** — every function summary is content-addressed; changes create new versions
- **Semantic incrementality** — invalidation based on component deltas (not file timestamps)
- **Full provenance** — every derived fact traces back to source anchors and analysis rules
- **Explicit uncertainty** — MUST / MAY / INFERRED / ASSUMED / UNKNOWN states preserved throughout
- **Whole-program analysis** — SCC-aware fixpoint computation over call graphs and value flows
- **Pluggable backends** — storage adapters for RocksDB, SQLite, in-memory, and custom backends
- **Evidence-first design** — CPG is a query projection; Summary IR is the source of truth

---

## Architecture

**Identity Model:** `RepositoryID → RevisionID → BuildVariantID → FunctionSymbolID → FunctionVariantID → FunctionBodyID → FunctionSummaryID`. All IDs are semantic (not file-line based), content-derived, canonicalized. Format: `<kind>:sha256:<digest>`.

**Analysis Stack (V1):**
- **Languages:** C++20
- **Build System:** CMake 3.23+
- **Host compiler:** CMake auto-detected (current dev machine: llvm@17 17.0.6)
- **LLVM/Clang libraries:** 22+ (23.x and 24.x supported; 24.x recommended)
- **Pointer Analysis:** SVF (pinned at `third_party/SVF@18fb5650…`, required, in-process)
- **Constraint Solver:** Z3
- **WPA Engine:** Soufflé Datalog
- **Storage:** RocksDB (CAS), SQLite (metadata/facts)
- **Serialization:** Protobuf
- **Testing:** GoogleTest

**Ingest Tiers:**
1. **Tier 1 (T0 canonical)** — `compile_commands.json` project directory → `CodeGenIrSource`
2. **Tier 2 (T0/T1 fidelity)** — bitcode/textual IR → `BitcodeIrSource`
3. **Tier 3 (epistemic floor)** — external analysis (Joern, PhASAR) → `ExternalFactsImporter`

---

## Building

**Prerequisites:**

- CMake 3.23+
- Ninja (`brew install ninja` on macOS; `apt install ninja-build` on Debian/Ubuntu)
- LLVM/Clang 22+ libraries (23.x and 24.x supported; 24.x recommended). Build with `LLVM_ENABLE_RTTI=OFF`, `LLVM_ENABLE_EH=OFF`, `LLVM_BUILD_LLVM_DYLIB=ON`, and `CLANG_LINK_CLANG_DYLIB=ON` — VERITAS and the vendored SVF link the monolithic `libLLVM` and `libclang-cpp` shared libraries. The host C/C++ compiler is auto-detected separately and need not match the LLVM library version.
- Z3 (`brew install z3` on macOS)

**Canonical local configure and build** (uses `CMakePresets.json` — Ninja generator, `build/` under the repo root, Debug):

```bash
# Replace the default path if your llvm-project build is elsewhere.
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
ctest --preset default
```

Local development builds must pass `LLVM_PROJECT_BUILD_DIR`; the default local
LLVM build directory is
`/Users/skg7on/Workspace/Projects/llvm-project/build`. CMake derives `LLVM_DIR`
and `Clang_DIR` from that tree so VERITAS and the vendored SVF use the same
LLVM installation.

`LLVM_PROJECT_BUILD_DIR` supplies only the LLVM/Clang **headers and libraries**.
It must not be used as `CMAKE_TOOLCHAIN_FILE` or as a compiler path. CMake
auto-detects the host C/C++ build toolchain independently unless the compiler is
explicitly overridden through `CC`/`CXX`, `CMAKE_C_COMPILER`, or
`CMAKE_CXX_COMPILER`; the compiler version does not need to match the LLVM
library version.

For non-local environments, omit `LLVM_PROJECT_BUILD_DIR` to use
`find_package(LLVM/Clang CONFIG)`: provide `LLVM_DIR` and `Clang_DIR` explicitly,
or let CMake search system paths.

**Available presets:** `default` (Debug), `debug`, `release`, `static-release` (opts out of `BUILD_SHARED_LIBS`). All resolve `binaryDir` to `<repo>/build`.

**Configure without presets** (for CMake < 3.19 or non-Ninja generators):

```bash
# Replace the default path if your llvm-project build is elsewhere.
cmake -S . -B build -G Ninja \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build build -j
```

Non-Ninja generators still work but emit a warning at configure time; only Ninja is exercised in CI.

**Build options:**

| Option | Default | Effect |
|---|---|---|
| `BUILD_SHARED_LIBS` | `ON` | VERITAS and SVF build as shared libraries (`.dylib` / `.so`). Set `OFF` for static. |
| `VERITAS_BUILD_TESTS` | `ON` | Build the unit and integration test targets. |
| `VERITAS_BUILD_TOOLS` | `ON` | Build the four CLI binaries (`veritas-build`, `veritas-query`, `veritas-diff`, `veritas-explain`). |

**Build layout:**

- `build/` — VERITAS build tree (libraries under `build/lib/`, binaries under `build/bin/`)
- `build/svf-build/` — vendored SVF build tree (`SvfCore`, `SvfLLVM`, `extapi.bc`, CMake exports)

See `docs/third_party/LLVM.md` and `docs/third_party/SVF.md` for the full toolchain contract.

---

## Development

VERITAS C and C++ code follows the LLVM coding style. Enable the tracked
pre-commit hook once per clone:

```bash
git config core.hooksPath .githooks
```

The hook requires `clang-format` on `PATH` and rejects commits whose staged
C or C++ content is not formatted. It checks the Git index without modifying
the working tree, so partially staged changes remain intact.

---

## Current State

M0–M8 are implemented and tested. Within the M8R remediation bridge, M8R.1 and
M8R.2 are delivered; M8R.3–M8R.5 remain outstanding, and M9–M12 sit behind the
M8R.5 entry gate. The current project pipeline and CLI are available now, with
the remaining milestones extending provenance, recursive analysis, Evidence
Builder inputs, Evidence IR semantic modeling and serialization, and external
inputs.

**Documentation:** Start at the [documentation index](docs/README.md).

- Architecture: [platform](docs/architecture/01-platform-architecture.md),
  [whole-program analysis](docs/architecture/02-whole-program-analysis-architecture.md),
  [SummaryDB storage](docs/architecture/03-summarydb-storage-architecture.md),
  and [Evidence IR](docs/architecture/04-evidence-ir-architecture.md).
- Specifications: [cross-cutting specs](docs/specs/README.md) and the
  [milestone specification matrix](docs/specs/milestones/README.md).
- Plans: the [backbone milestone roadmap](docs/plans/veritas-backbone-milestone-roadmap.md)
  and [milestone implementation plans](docs/plans/README.md).

---

## CLI Overview (Planned)

```bash
# Tier 1: Direct project analysis
veritas-build analyze --project <directory>

# Tier 2: Bitcode input
veritas-build analyze --bitcode <module>

# Tier 3: External-facts import
veritas-build import --joern <export.json>
veritas-build import --phasar <results.json>

# Query operations
veritas-query summary <function-id>
veritas-query callers <function-id>
veritas-query writes <memory-ref>
veritas-query evidence <claim-id>

# Incremental operations
veritas-diff <revision-a> <revision-b>
veritas-explain fact <fact-id>
```

---

## Principles (P1–P8)

1. **Immutable summaries** — summary bytes never change; current bindings are mutable metadata
2. **Everything versioned** — repository, revision, build variant, analyzer run, and summary IDs
3. **Every derived fact has provenance** — tuples trace back to rules, inputs, and source anchors
4. **Explicit uncertainty** — MUST / MAY / INFERRED / ASSUMED / UNKNOWN preserved; no silent promotion
5. **Incrementality on semantic deltas** — invalidation based on component hashes, not file timestamps
6. **WPA consumes summaries by default** — external artifacts enter via Tier 3 only
7. **CPG is a query projection** — Summary IR is the source of truth; CPG indexes it
8. **LLM output is a hypothesis** — requires deterministic verification before becoming a fact

---

## License

TBD

---

## Contact

Project maintained by the VERITAS team.
