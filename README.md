<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/veritas-logo-dark.png">
    <source media="(prefers-color-scheme: light)" srcset="docs/assets/veritas-logo.png">
    <img src="docs/assets/veritas-logo.png" alt="VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics" width="50%">
  </picture>
</p>

# VERITAS

**Verified Evidence Reasoning IR for Trans-program Analysis and Semantics**

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
- **Compiler:** LLVM/Clang 22.x
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

## Current State

**Design-phase repository.** Four architecture documents (`docs/architecture/`), engineering-backbone spec, milestone specs M1–M12 (`docs/specs/milestones/`), implementation plans for M11/M12 (`docs/plans/`). No source code yet; `main` is clean awaiting M0 implementation.

**Documentation:**
- Architecture: `docs/architecture/`
  - `veritas-platform-architecture-design.md` — system pipeline, principles P1–P8, ingest adapters
  - `veritas-whole-program-analysis-design.md` — analyzer engines and SOTA C/C++ pointer-alias policy
  - `veritas-thin-summarydb-backends-design.md` — SummaryDB physical layers and pluggable storage
  - `veritas-evidence-ir-design.md` — Evidence IR formal syntax and semantics
- Specifications: `docs/specs/`
  - `veritas-engineering-backbone-design-specification.md` — connective spec between architecture and milestones
  - `veritas-backbone-milestones-and-implementation-plan.md` — implementation checklist
  - `docs/specs/milestones/` — detailed design specs for M1–M12
- Implementation Plans: `docs/plans/` — executable per-milestone plans (M11/M12 in place)

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
