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

**Analysis Stack (V1):** C++20, CMake 3.23+, LLVM/Clang 22+, required pinned SVF (`third_party/SVF@18fb5650…`), Z3, Protobuf, RocksDB, SQLite, Soufflé, GoogleTest. SVF is mandatory for L1 Andersen pointer analysis; no `VERITAS_ENABLE_SVF` toggle exists.

**Milestones M0–M12:** Skeleton + toolchain → project ingestion → identity + metadata → Summary IR + CAS → Clang/LLVM local extraction → required in-process SVF → thin CPG projection → reverse-dep index + incremental scheduler → SCC WPA + Soufflé → provenance fact store + explain API → Evidence Builder input APIs + buffer-overflow demo → external IR adapter (bitcode) → external-facts importer (Joern/PhASAR).

**First Demo:** A `decode → memcpy(b->data, p->payload, p->len)` fixture showing upstream `validatePacket` change → one local summary recomputed → range component delta → seven dependent summaries invalidated → WPA finds unsafe flow → Evidence case built with flow, range facts, missing dominating check, unknown for `vendor_validate`, and proof obligation. No LLM required; provenance-backed semantic slices only.

**Current State:** Design-phase repository. Four architecture documents (`docs/architecture/`), engineering-backbone spec, milestone specs M1–M12 (`docs/specs/milestones/`), implementation plans for M11/M12 (`docs/plans/`). No source code yet; `main` is clean awaiting M0 implementation.

---

## Repository Policies

This repository enforces a mandatory Git worktree policy for every Claude Code session. See the referenced rule below for the full policy.

@.claude/rules/git-worktree-policy.md

This repository has a canonical documentation layout. Design specs live under `docs/specs/` and implementation plans under `docs/plans/`. See the referenced rule below; the `superpowers` skills write to these paths.

@.claude/rules/docs-layout.md
