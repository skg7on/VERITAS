# CLAUDE.md

## Project Overview

**VERITAS** — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics

**Core Thesis:** A Review Agent should never be asked to "understand a million-line repository." Deterministic analysis first builds a compact, immutable, provenance-carrying semantic world. The Agent reasons only over that structured representation (Evidence IR), and any hypothesis it produces must round-trip through a deterministic verifier before becoming a fact.

**Three IRs + Feedback Loop:**

1. **Function Summary IR** — externally visible semantic effects of a function (calls, memory reads/writes, value flows, ranges, aliases, locks, state transitions, unknowns). Immutable, content-addressed, component-hashed.

2. **SummaryDB** — a logical subsystem across seven physical layers: Object Store · Metadata · Fact Store · Graph Index · Dependency Index · Evidence Cache · History Store. Backed by RocksDB (CAS), SQLite (metadata/facts), and Soufflé (WPA execution).

3. **Evidence IR (EIR)** — claim-oriented, provenance-preserving typed graph IR the Agent consumes. Enforces MUST / MAY / MUST_NOT / INFERRED / ASSUMED / UNKNOWN as distinct epistemic states. LLM output enters as INFERRED and requires deterministic verification (static analysis / SMT / symbolic execution) before promotion to MUST.

**Pipeline:** Ingest (compile_commands.json, bitcode, or external analysis) → module acquisition → local static analysis → Function Summary IR → SummaryDB → Incremental WPA (SCC + fixpoint + Datalog) → global derived facts → Evidence Builder → Evidence IR → Agent + Proof Engines → Review Result.

**Key Principles (P1–P8):** Immutable summaries · everything versioned · every derived fact has provenance · explicit uncertainty · incrementality on semantic deltas (not file timestamps) · WPA consumes summaries by default · CPG is a query projection, not the source of truth · LLM output is a hypothesis, never a fact.

**Identity Model:** `RepositoryID → RevisionID → BuildVariantID → FunctionSymbolID → FunctionVariantID → FunctionBodyID → FunctionSummaryID`. All IDs are semantic (not file-line based), content-derived, canonicalized. Format: `<kind>:sha256:<digest>`.

**Analysis Stack (V1):** C++20, CMake 3.23+, LLVM/Clang 22+ libraries, required pinned SVF (`third_party/SVF@18fb5650…`), Z3, Protobuf, RocksDB, SQLite, Soufflé, GoogleTest. SVF is mandatory for L1 Andersen pointer analysis; no `VERITAS_ENABLE_SVF` toggle exists.

**Milestones M0–M12:** Skeleton + toolchain → project ingestion → identity + metadata → Summary IR + CAS → Clang/LLVM local extraction → required in-process SVF → thin CPG projection → reverse-dep index + incremental scheduler → SCC WPA + Soufflé → provenance fact store + explain API → Evidence Builder input APIs + buffer-overflow demo → Evidence IR semantic model + serialization → external IR adapter (bitcode) → SummaryDB external-provider substrate → direct Joern GraphSON/GraphML importer → provider fusion/Evidence integration; PhASAR remains a separately designed provider adapter.

**First Demo:** A `decode → memcpy(b->data, p->payload, p->len)` fixture showing upstream `validatePacket` change → one local summary recomputed → range component delta → seven dependent summaries invalidated → WPA finds unsafe flow → M10B builds a completeness-aware input → M10C builds a validated Evidence case with flow, range facts, missing dominating check, unknown for `vendor_validate`, and a proof obligation. No LLM required.

**Current State:** M0–M8 are implemented and tested. The standard `veritas-build analyze --project <dir>` runs M1 → M4 → M5 → M3 → M6 entirely in-process: Clang/LLVM local extraction → required in-process SVF → Summary IR → atomic summary+CPG publication, with the thin CPG queryable via `veritas-query`. M7 adds the reverse-dependency index and deterministic incremental scheduler. M8 adds deterministic call/SCC graphs, C++ fixpoint evaluation, persisted SCC convergence hashes, M7 propagation only for externally visible changes, and an optional Souffle relation export/import and rule-execution path. Souffle was unavailable during local M8 verification, so its executable comparison test is registered but explicitly skipped; the required C++ WPA path passes without it. M8R is approved and pending implementation; M9–M12 (durable provenance and explain APIs, recursive domain expansion, Evidence Builder inputs, Evidence IR semantic modeling and serialization, the external IR adapter, and the external-provider/Joern SummaryDB path) remain planned. Start with the [documentation index](docs/README.md), then use the [architecture index](docs/architecture/README.md), [milestone specifications](docs/specs/milestones/README.md), and [milestone plans](docs/plans/README.md).

---

## Build

Canonical configure and build (uses `CMakePresets.json` at repo root):

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/path/to/llvm-project/build
cmake --build --preset default
```

- Presets pin the generator to **Ninja** and the binary directory to `<repo>/build`. Variants: `default` (Debug), `debug`, `release`, `static-release`.
- Requires CMake 3.23+, Ninja, LLVM/Clang 22+ (24.x recommended), Z3. See `docs/third_party/LLVM.md` and `docs/third_party/SVF.md` for the toolchain contract (VERITAS and SVF automatically match LLVM's RTTI/EH settings).
- `LLVM_PROJECT_BUILD_DIR` is optional. When set, `cmake/VeritasLLVM.cmake` derives `LLVM_DIR` and `Clang_DIR` from that tree; otherwise `find_package(LLVM/Clang CONFIG)` falls back to explicit `LLVM_DIR` / `Clang_DIR` or system paths.
- The **host C/C++ compiler** is auto-detected by CMake (or set via `CC`/`CXX` / `CMAKE_C_COMPILER`) and is independent of the LLVM library version: `LLVM_PROJECT_BUILD_DIR` supplies only the LLVM/Clang **headers and libraries**, never the compiler. On the current dev machine the host compiler is llvm@17 (clang 17.0.6) against LLVM 24.x libraries — that skew is intentional. VERITAS code is compiled as C++20.
- SVF is vendored at `third_party/SVF/` and always builds. Its build tree lives at `build/svf-build/` and stays out of the default `all` target — `SvfCore`/`SvfLLVM` build on demand through the private `veritas_third_party_svf` wrapper.
- `BUILD_SHARED_LIBS` defaults to `ON`; VERITAS and SVF both build shared. `--preset static-release` opts out.
- Other options: `VERITAS_BUILD_TESTS` (ON), `VERITAS_BUILD_TOOLS` (ON).
- Non-Ninja generators still configure but emit a warning; only Ninja is exercised in CI.

---

## Repository Policies

This repository enforces a mandatory Git worktree policy for every Claude Code session. See the referenced rule below for the full policy.

@.claude/rules/git-worktree-policy.md

This repository has a canonical documentation layout. Architecture documents,
design specs, and implementation plans have separate indexes under
`docs/README.md`; see the referenced rule below for the exact paths.

@.claude/rules/docs-layout.md

Every VERITAS-authored source, header, and CMake file must open with an SPDX-style Apache-2.0 header. See the referenced rule below for the exact format, per-language comment style, and the pre-commit verification snippet.

@.claude/rules/license-header-policy.md

VERITAS builds with RTTI and exceptions disabled to maintain compatibility with LLVM. See the referenced rule below for the rationale, coding constraints, and alternative patterns.

@.claude/rules/cpp-compilation-policy.md

Every push to the remote must pass local build and test verification. See the referenced rule below for the complete pre-push checklist and verification steps.

@.claude/rules/pre-push-verification-policy.md
