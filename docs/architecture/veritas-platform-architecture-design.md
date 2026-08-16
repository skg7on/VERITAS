# VERITAS Platform Architecture

## System Pipeline, Design Principles, and Ingest Adapters

**Status:** Draft Architecture Specification
**Version:** 0.2
**Project:** VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics
**Peers:**

* `docs/architecture/veritas-whole-program-analysis-design.md` — analyzer engines (AST/CFG/DDG/VFG/CPG) and SOTA C/C++ alias policy.
* `docs/architecture/veritas-thin-summarydb-backends-design.md` — SummaryDB physical layers and pluggable storage backends.
* `docs/architecture/veritas-evidence-ir-design.md` — Evidence IR formal syntax and semantics.

---

# 1. Purpose

VERITAS is an evidence-centric whole-program analysis platform that combines deterministic static analysis with LLM-assisted semantic reasoning.

This document is the top-level architectural entry point. It fixes:

* the end-to-end pipeline from source or external analysis inputs through SummaryDB, whole-program analysis, and Evidence IR;
* the platform-wide design principles P1–P8 that every subsystem inherits;
* the **ingest adapter** boundary — the small set of stages through which external inputs are allowed to enter, and the invariants those adapters preserve.

The two peer documents specialize this architecture:

```text
                platform architecture (this doc)
                    /                       \
                   /                         \
   whole-program analysis            thin SummaryDB + backends
```

Readers new to VERITAS should read this document first, then either peer as needed.

---

# 2. Architectural Thesis

The organizing thesis is:

> A Review Agent should never be asked to "understand a million-line repository." Deterministic analysis first builds a compact, immutable, provenance-carrying semantic world. The Agent reasons only over that world.

To deliver that thesis, VERITAS maintains three intermediate representations and one feedback loop:

* **Function Summary IR** — the externally visible semantic effects of a function (calls, memory reads/writes, value flows, ranges, aliases, locks, state transitions, unknowns). Produced by local extraction, immutable, content-addressed.
* **SummaryDB** — a logical subsystem (object store · metadata · fact store · graph index · dependency index · evidence cache · history store) that keeps summaries immutable and incrementally maintainable. See `veritas-thin-summarydb-backends-design.md`.
* **Evidence IR (EIR)** — a claim-oriented, provenance-preserving typed graph IR that mediates the loop between whole-program analysis and the Review Agent. See `veritas-evidence-ir-design.md`.

The architectural pattern is ThinLTO-style: local translation units emit compact summaries, and whole-program analysis operates primarily over a combined summary index rather than reloading full IR.

---

# 3. Design Principles

Every subsystem in VERITAS inherits these principles. They are cited by ID from milestone specs.

### P1 — Semantic summaries are immutable

Never mutate a summary in place. New analysis produces new content-addressed objects; mutable state is confined to publication bindings.

### P2 — Everything important is versioned

Sources, builds, analyses, schemas, summaries, and facts each carry an identity that participates in downstream hashes.

### P3 — Every derived fact has provenance

No unexplained derived fact. Every non-trivial conclusion answers "why is this true" through a finite provenance subgraph.

### P4 — Uncertainty is explicit

`MUST`, `MAY`, `MUST_NOT`, `INFERRED`, `ASSUMED`, `UNKNOWN` are distinct epistemic states. Uncertainty is never collapsed to a boolean or to a confidence score.

### P5 — Incrementality operates on semantic deltas

File-level dependency invalidation is a bootstrap. The real invariant is that only consumers of a changed semantic component are re-analyzed.

### P6 — WPA consumes summaries by default

Whole-program analysis loads detailed AST or IR only when refinement demands it. Everyday queries run over summaries and the CPG projection.

### P7 — CPG is a query projection, not the sole source of truth

The Code Property Graph is one materialization over the same underlying summaries and facts. Storage and computation remain flexible.

### P8 — LLM output is a hypothesis, not a fact

Neuro-symbolic reasoning enters only as `INFERRED` propositions. Promotion to `MUST` requires a deterministic verifier — static analysis, SMT, symbolic execution, or concrete replay.

---

# 4. End-to-End Pipeline

```text
        ingest adapters (§6)
                │
                ▼
        module acquisition
                │
                ▼
      local static analysis    (see veritas-whole-program-analysis-design.md)
                │
                ▼
     Function Summary IR (immutable, component-hashed)
                │
                ▼
   ┌─────────── SummaryDB ────────────┐
   │   object · metadata · facts       │  (see veritas-thin-summarydb-backends-design.md)
   │   graph index · dep index         │
   │   evidence cache · history        │
   └─────────────────┬─────────────────┘
                     │
                     ▼
        Incremental WPA (SCC + fixpoint + Datalog)
                     │
                     ▼
             Global derived facts
                     │
                     ▼
             Evidence Builder
                     │
                     ▼
              Evidence IR
                     │
                ┌────┴────┐
                ▼         ▼
             Agent   Proof Engines
                └────┬────┘
                     ▼
              Review Result
```

The pipeline is deliberately asymmetric: local extraction, SummaryDB, WPA, and Evidence Builder run deterministically and are reproducible for identical inputs. The Agent adds semantic hypotheses that only re-enter the deterministic world as proof obligations.

---

# 5. Subsystem Split

Responsibilities are cleanly divided across the three architecture documents.

| Concern | Document |
| --- | --- |
| Ingest surface, adapter tiers, invariants, principles, milestones, CLI, end-to-end pipeline | this document |
| Analyzer engines: AST, CFG, dominators, DDG, VFG, thin CPG projection, call-graph, memory-effect model, range/constraint model, SOTA C/C++ pointer-alias policy, in-process SVF integration | `veritas-whole-program-analysis-design.md` |
| Storage physical layers, pluggable backend contract, RocksDB/SQLite reference bindings, identity model, content-addressed CAS, component hashes, publication atomicity, evidence cache, history store | `veritas-thin-summarydb-backends-design.md` |
| Evidence IR syntax, semantics, epistemic lattice, provenance DAG, proof obligations | `veritas-evidence-ir-design.md` |

The four documents are complementary; none duplicates load-bearing statements from the others.

---

# 6. Ingest Adapter Architecture

VERITAS accepts inputs at three distinct stages, each with a different fidelity and epistemic contract. The adapter layer keeps the rest of the platform decoupled from how a program entered the pipeline.

```text
              ┌──────────────────────────────────────────┐
              │             Ingest Adapters              │
              │                                          │
   ┌──────────┼───────────────┐        ┌─────────────────┼──────────┐
   │          │               │        │                 │          │
   ▼          ▼               ▼        ▼                 ▼          ▼
compile_    .bc / .ll     directory   Joern CPG      PhASAR      future
commands    single file   of bitcode  export         result      producers
.json                                 (GraphML/JSON)
   │          │               │        │                 │          │
   └────┬─────┴───────┬───────┘        └─────────┬───────┴──────────┘
        ▼             ▼                          ▼
  CodeGenIrSource  BitcodeIrSource        ExternalFactsImporter
        (Tier 1)        (Tier 2)               (Tier 3)
        │             │                          │
        ▼             ▼                          ▼
   [ Clang CodeGen +               [ ExternalFact records
     Link → ProgramIr ]              → fact store
                                     + provenance store ]
        │             │
        └──────┬──────┘
               ▼
         private ProgramIr
               │
               ▼
     local static analysis
     (see analysis doc)
```

Three architectural properties define the boundary:

1. **Module-acquisition vs. facts.** Tiers 1 and 2 produce a private `ProgramIr` and then flow through the full analysis pipeline unchanged. Tier 3 injects external observations directly into the fact store; it never influences summaries, SCC propagation, or CPG projection.
2. **VERITAS owns analysis.** No adapter runs VERITAS's own analysis pipeline for the caller. Local extraction, SVF, CPG projection, WPA, and provenance derivation are always executed by VERITAS on the module it acquired.
3. **Identity is derived, never carried.** External IDs (LLVM `Value*`, Joern node IDs, PhASAR fact IDs) never persist in VERITAS artifacts. All references are re-derived to VERITAS stable IDs (see `veritas-thin-summarydb-backends-design.md` §5).

---

# 7. Tier 1 — Project Directory (`compile_commands.json`)

Tier 1 is the canonical VERITAS ingest. The public input is a project directory containing a Clang compilation database.

```text
project directory
    └── compile_commands.json
    └── (source tree, includes)
                │
                ▼
   Build Intelligence (M1)
   • normalize compile commands
   • canonicalize paths and arguments
   • derive ProgramContext (repo/revision/build-variant/target/…)
   • derive per-TU command_hash and preprocessor_hash
                │
                ▼
   CodeGenIrSource (M4)
   • Clang CodeGen per TU
   • llvm::Linker into one private ProgramIr
   • BuildOriginMap (SourceAnchorID per BB)
                │
                ▼
        private ProgramIr
```

Contracts:

* Machine paths never enter semantic hashes; only canonicalized, project-relative components do.
* Equivalent project inputs on any host produce byte-identical `ProgramContext` and byte-identical summaries.
* Partial ingestion is a hard failure — VERITAS never silently analyzes a subset of TUs.
* `.bc` / `.ll` is **not** a valid public input at Tier 1. Bitcode enters through Tier 2 alone.

Tier 1 delivers Fidelity Tier **T0** by construction: full debug info, macro provenance, and source anchors.

---

# 8. Tier 2 — External Bitcode / Textual IR

Tier 2 accepts `.bc`, `.ll`, or a directory of either, as a **module** input. It skips Clang CodeGen but runs VERITAS's own SVF, local extraction, CPG projection, WPA, and provenance derivation unchanged.

## 8.1 Fidelity tiers

Bitcode carries a variable amount of what identity needs. VERITAS classifies inputs on entry:

| Tier | Bitcode contents | Recoverable | Policy |
| --- | --- | --- | --- |
| T0 | Debug info (`llvm.dbg` + `-g`) | mangled name, signature, source file/line, type layout | full analysis; source anchors preserved |
| T1 | Symbols, no debug info | mangled name, reconstructed signature (from IR types), linkage | analyze; **no source anchors** (Evidence shows function name only) |
| T2 | Stripped (no symbols) | nothing stable | **reject** with a clear error |

T2 is rejected because VERITAS's first invariant is semantic identity; a module with no stable symbol identity cannot produce valid `FunctionSymbolID`s.

## 8.2 Pipeline

```text
[ .bc  |  .ll  |  directory of either ]
   │
   ▼
BitcodeModuleLoader.LoadAll   (parse + verify each module)
   │
   ▼
DetectFidelity                 (T0 / T1 / T2)
   │
   ├── T2 ─► reject (FailedPrecondition)
   │
   ▼
llvm::Linker                    (merge into one private ProgramIr)
   │
   ▼
BuildOriginMap                  (DISubprogram/DILocation → source anchors, T0 only)
   │
   ▼
ProgramIr  (fidelity tier + OriginMap recorded)
   │
   ▼
local static analysis  (unchanged from Tier 1)
```

## 8.3 Identity and provenance

The fidelity tier flows into every downstream fact through the analyzer run's provenance:

```text
producer         = svf
input_fidelity   = { debug_info | symbols_only }
has_source_anchors = { true | false }
```

Source-derived and bitcode-derived summaries of "the same function" live in **different build variants** by construction (different source-tree / module inputs). Their `FunctionVariantID`s differ. VERITAS does not attempt cross-path identity convergence in V1 (see Evidence IR §44 "program revision semantics").

---

# 9. Tier 3 — External-Facts Importer

Tier 3 accepts already-computed analysis results — Joern CPG exports (GraphML / JSON) and PhASAR results — and maps them into the fact store as provenance-tagged, epistemic-lowered external observations. It never touches the CPG projection stage, SVF, or the summary pipeline.

## 9.1 What imports become

Each imported record becomes an `ExternalFact`:

```text
ExternalFact {
    producer          = joern | phasar
    external_id       = <opaque producer-side ID>       (kept for provenance only)
    subject_id        = VERITAS stable ID  ─or─
                        synthetic external:<producer>:<id>
    predicate_kind    = one of VERITAS's fact vocab entries
                        (or opaque `external_observation`)
    predicate_canonical
    epistemic         = INFERRED | ASSUMED
    source_anchor_id  = (optional) VERITAS SourceAnchorID
    raw_record        = verbatim source record (provenance)
}
```

## 9.2 Rules of admission

1. **Identity bridge.** The importer resolves each external entity to a VERITAS stable ID only via stable inputs: mangled name → `FunctionVariantID`, `file:line` → `SourceAnchorID`. Unresolvable entities receive a synthetic subject `external:<producer>:<external_id>`. VERITAS **never** fabricates a semantic ID from a Joern/PhASAR ordinal.
2. **Epistemic floor.** External facts enter as `INFERRED` (from a semantic analysis) or `ASSUMED` (accepted as a premise). The `MUST` state is unreachable from external input. This is the P4 / P8 rule made concrete: an inferred premise cannot yield a verified fact.
3. **Trust policy.** Provenance records `producer_kind = external`, `producer_id = joern | phasar`; deployment-specific trust levels apply per `veritas-evidence-ir-design.md` §58–59. No `EXTERNAL` fact silently becomes a `VERIFIED_FACT`.
4. **No WPA participation.** External facts are terminal; they are not summary components, so they do not drive incremental invalidation, SCC propagation, or dependency-index rebuilds. They *are* citable by the Evidence Builder and queryable in the fact store.
5. **Vocabulary normalization.** Joern / PhASAR predicates map into VERITAS's fact vocabulary where a stable equivalence exists (e.g. Joern `CALL` → `CallFact`, `REACHING_DEF` → `ValueFlowFact`). Unmappable predicates persist as opaque `external_observation`, and the raw record is retained for provenance.

## 9.3 Pipeline

```text
[ Joern export | PhASAR result ]
       │
       ▼
   importer  (parse + identity-bridge + vocabulary-normalize)
       │
       ▼
   ExternalFact[]   (epistemic = INFERRED | ASSUMED)
       │
       ▼
   fact store  +  provenance store   (producer_kind = external)
       │
       ▼
   Evidence Builder / veritas-query can cite them
```

---

# 10. Adapter Interface Contract

Two abstractions define the adapter boundary. Both live in the analysis pipeline and are the only points at which external artifacts enter VERITAS.

## 10.1 `ProgramIrSource` — module acquisition (Tiers 1 and 2)

```cpp
namespace veritas::analysis::pipeline {

class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual StatusOr<ProgramIr> Build() = 0;
};

class CodeGenIrSource : public ProgramIrSource { /* Clang CodeGen + link */ };
class BitcodeIrSource : public ProgramIrSource { /* Loader + link + OriginMap */ };

}  // namespace veritas::analysis::pipeline
```

Local analysis takes a `ProgramIrSource&` and does not care which subclass produced the module:

```text
before:  RunLocalAnalysis(AnalysisManifest) → {ProgramIr, summary_drafts}
after:   RunLocalAnalysis(ProgramIrSource&) → {ProgramIr, summary_drafts}
```

`LocalFactExtractor`, SVF, CPG projection, WPA, and provenance derivation are unchanged by the source choice.

## 10.2 `ExternalFactsImporter` — fact injection (Tier 3)

```cpp
namespace veritas::facts::external {

enum class ExternalProducer { Joern, Phasar };

class JoernCpgImporter { /* ImportGraphML / ImportJson  → vector<ExternalFact> */ };
class PhasarResultImporter { /* Import                    → vector<ExternalFact> */ };

class ExternalIdentityBridge {
 public:
  StatusOr<core::StableId> Resolve(ExternalProducer, const std::string& external_ref);
};

}  // namespace veritas::facts::external
```

Downstream, the fact-store `Put` path applies the epistemic-floor validation. An attempt to insert an external fact at epistemic `MUST` is rejected at write time (`FailedPrecondition`).

Concrete signatures and testing tables for these interfaces live in `docs/specs/veritas-summarydb-ingest-adapter-design.md` (milestone-scoped spec).

---

# 11. Invariants

VERITAS carries a set of platform-wide invariants. Analysis-specific invariants are stated in `veritas-whole-program-analysis-design.md`; storage-specific invariants in `veritas-thin-summarydb-backends-design.md`. The invariants below govern the platform as a whole and the ingest boundary in particular.

| ID | Invariant | Reason |
| --- | --- | --- |
| B1 | Function identity is semantic, not file-line based. | Source locations drift, and macros/templates create multiple meanings. |
| B2 | Summary objects are immutable and content-addressed. | Enables deduplication, parallel generation, historical queries, reproducibility. |
| B3 | Summary components have independent semantic hashes. | Enables analysis-specific invalidation instead of "invalidate all callers." |
| B4 | Dependency edges are reverse-queryable by component delta. | Incremental WPA must find consumers of exactly the changed dimension. |
| B5 | Recursive call regions are propagated as SCCs. | Recursion requires monotone fixpoint convergence, not caller-chain unfolding. |
| B6 | Every derived fact has provenance. | VERITAS must answer why a fact is true. |
| B7 | Epistemic state and confidence are separate. | `MAY` is not "low confidence"; `INFERRED` is not "verified." |
| B8 | Publication is atomic at metadata bindings, not at object mutation. | Readers must never observe a half-written summary revision. |
| B9 | Deterministic analysis is reproducible for the same inputs. | Enables stable hashes, diffing, regression analysis. |
| B10 | The only public **source** input is a project directory containing `compile_commands.json`. External **LLVM IR artifacts** (`.bc` / `.ll` / bitcode directory) are additionally accepted as a **module** input through the IR adapter; they enter at module acquisition (skipping Clang CodeGen) and never bypass VERITAS's own analysis or identity/provenance derivation. External **analysis results** (Joern export, PhASAR result) are accepted only through the external-facts importer as epistemic-lowered observations. | Prevents external preprocessing artifacts from becoming public contracts or reproducibility gaps, while allowing binary-only and cross-tool ingest at controlled boundaries. |

B10 is the load-bearing invariant for this document; it is the successor to the pre-adapter statement (which admitted only Tier 1).

---

# 12. Incremental Update Pipeline

The end-to-end update flow is designed so that the answer to "what changed?" is expressed in semantic deltas, not file deltas.

```text
     Git change  (or new revision, new build variant, new external input)
             │
             ▼
   Changed-file detection
             │
             ▼
   Build dependency resolver
             │
             ▼
   Affected translation units
             │
             ▼
   AST semantic diff
             │
             ▼
   Changed functions
             │
             ▼
   Local static analysis  (analysis doc §…)
             │
             ▼
   New FunctionSummary  (component hashes recomputed)
             │
             ▼
   Component-level summary diff
             │
             ▼
   Semantic dependency index
             │
             ▼
   Affected consumers  (only those depending on the changed component)
             │
             ▼
   SCC propagation
             │
             ▼
   Fixpoint
             │
             ▼
   Updated WPA facts   →   affected Evidence cases marked STALE
```

Two properties of the update pipeline are worth calling out because they follow directly from P5:

* **Semantic-hash stopping.** If a source change does not change the summary, propagation stops at the local analysis boundary. `int x = a + b;` → `int x = b + a;` changes the source hash but not (necessarily) the summary hash.
* **Component-scoped invalidation.** If only a function's `RangeHash` changes, consumers of its `CallHash` are not re-analyzed. The dependency index is keyed by `(producer_id, component)` for exactly this reason.

---

# 13. Update Scheduler

The scheduler is a worklist over typed work items:

```text
LOCAL_SUMMARY          — a function must be re-extracted
SCC_RECOMPUTE          — an SCC must be re-joined
WPA_COMPONENT          — a WPA domain must be re-derived
FACT_DERIVATION        — a Datalog rule must be re-evaluated
EVIDENCE_INVALIDATION  — a stored Evidence case must be revalidated
```

Conceptually:

```cpp
while (!worklist.empty()) {
  Item n = worklist.pop();
  Summary old   = db.get(n);
  Summary fresh = recompute(n);
  SummaryDelta delta = diff(old, fresh);
  if (delta.empty()) continue;
  db.publish(fresh);
  for (Consumer c : dependencyIndex.users(n, delta))
    worklist.push(c);
}
```

The essential ingredient is `dependencyIndex.users(n, delta)` — consumers of the specific component that changed, not `all_callers(n)`. Sensitivity tags distinguish `SEMANTIC`, `EVIDENCE_ONLY`, `IDENTITY`, and `CONFIGURATION` deltas so that evidence-only churn (comment changes, source-line renumbering) never triggers WPA rework.

---

# 14. Distributed Scaling Model

Because summaries are immutable and content-addressed, workers can generate them in parallel without coordination:

```text
                        Coordinator
                             │
             ┌───────────────┼───────────────┐
             ▼               ▼               ▼
         Worker 1        Worker 2         Worker N
          TU-A            TU-B             TU-Z
             │               │               │
             └───────────────┼───────────────┘
                             ▼
                     Summary Object Store  (CAS)
                             │
                             ▼
                Global WPA Coordinator  (SCC + fixpoint)
```

V1 focuses on the local single-machine version. Once the local model works, the same architecture generalizes because the CAS store is the coordination surface — content-addressed writes are inherently idempotent.

---

# 15. CLI Contract

The developer surface for the platform is deliberately small:

```bash
veritas-build analyze --project <directory>        # Tier 1: compile_commands.json
veritas-build analyze --bitcode <path>             # Tier 2: .bc / .ll / directory
veritas-build import  --joern  <export>            # Tier 3: Joern CPG export
veritas-build import  --phasar <result>            # Tier 3: PhASAR result

veritas-query summary   <symbol>
veritas-query callers   <symbol>
veritas-query writes    <global>
veritas-query evidence  overflow  <sink>

veritas-diff  <rev-a>  <rev-b>
veritas-explain fact <fact-id>
```

Contracts:

* `--project` and `--bitcode` are **mutually exclusive** (exactly one is required).
* External ingestion lives on `veritas-build import` rather than a separate tool; it can be split later if the surface grows.
* All read commands take the current published SummaryDB revision by default and accept `--at <rev>` for historical queries.

Sample output from `veritas-build analyze`:

```text
Translation Units:       8,421
Functions:             692,812
Call Edges:          4,281,923
Value-flow Edges:   19,281,372
Summaries:             681,221
Unknown Calls:           8,231
```

Sample output from `veritas-diff HEAD~1 HEAD`:

```text
Source changed functions:          214
Summary changed functions:          37
Call behavior changed:               4
Memory effects changed:             12
Range contracts changed:             9
State transitions changed:           3
WPA affected functions:            186
```

---

# 16. Milestone Map

The backbone is delivered as ten sequential milestones (plus M0 skeleton, plus M11/M12 for the external adapters). Each milestone has a design spec under `docs/specs/milestones/` and an implementation plan under `docs/plans/`.

| Milestone | Scope |
| --- | --- |
| M0 | Project skeleton, toolchain, CI. |
| M1 | Build Intelligence and `ProgramContext` from `compile_commands.json`. |
| M2 | Identity, canonical hashing, metadata store. |
| M3 | Summary IR and CAS object store. |
| M4 | Clang/LLVM local extraction (`CodeGenIrSource`). |
| M5 | Required in-process SVF value-flow / pointer analysis. |
| M6 | Thin VERITAS CPG projection. |
| M7 | Reverse dependency index + incremental scheduler. |
| M8 | SCC-aware WPA + Soufflé fact engine. |
| M9 | Provenance fact store + explain API. |
| M10 | Evidence Builder input APIs + first end-to-end demo. |
| M11 | External IR adapter (`BitcodeIrSource`, `veritas-build analyze --bitcode`). |
| M12 | External-facts importer (Joern / PhASAR, `veritas-build import`). |

The Review Agent and its verification loop are post-backbone milestones and are not required for the platform's V1 usefulness. See §17 for the first target demo.

---

# 17. First High-Value Demo

The demo that exercises the whole platform is deliberately narrow — one small program, one upstream change:

```cpp
void decode(Packet *p, Buffer *b) {
    ...
    memcpy(b->data, p->payload, p->len);
}
```

Story:

```text
AST → CFG → VFG → FunctionSummary → SummaryDB

Then upstream:
    validatePacket()   changes

Result:
    1 function modified
        ↓
    1 local summary recomputed
        ↓
    range contract changes
        ↓
    only 7 dependent summaries invalidated
        ↓
    WPA finds new unsafe flow
        ↓
    Evidence IR generated:
        packet.len → decode → copy → memcpy
        no dominating check
        ↓
    Review Agent inspects
```

This single demo demonstrates the entire VERITAS thesis: incremental semantic invalidation, provenance-carrying facts, and a compact Evidence case delivered to the Agent.

---

# 18. Target Architecture

```text
              ┌──────────────────────────────────┐
              │           Source World           │
              │ C/C++ · Build · Specs · Tests    │
              └──────────────┬───────────────────┘
                             │
                             ▼
                    Ingest Adapters
                    (§6 — three tiers)
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
     CodeGenIrSource  BitcodeIrSource  ExternalFacts
                             │              │
                             ▼              │
                   Static Analysis Frontend │
                   Clang + LLVM + Domain    │
                             │              │
                             ▼              │
                  ┌────────────────────┐    │
                  │   FunctionSummary  │    │
                  │        IR          │    │
                  └──────────┬─────────┘    │
                             │              │
                             ▼              ▼
        ┌──────────────── SummaryDB ─────────────┐
        │  object · metadata · facts             │
        │  graph index · dependency index        │
        │  evidence cache · history store        │
        │  (see thin-summarydb-backends doc)     │
        └────────────────────┬───────────────────┘
                             │
                    Incremental WPA
                     SCC / Datalog
                             │
                             ▼
                       Global Facts
                             │
                             ▼
                     Evidence Builder
                             │
                             ▼
                       Evidence IR
                             │
                        ┌────┴────┐
                        ▼         ▼
                     Agent     Proof Engines
                        │         │
                        └────┬────┘
                             ▼
                      Review Result
```

The Agent's role is compact: interpret evidence, rank findings, identify missing semantics, infer API contracts, request additional analysis. It does not directly promote any claim to `VERIFIED_DEFECT` or `VERIFIED_SAFE`; those transitions require a deterministic verifier per P8.

---

# 19. Reading Order

New readers:

1. This document — platform pipeline, principles, ingest boundary.
2. `veritas-whole-program-analysis-design.md` — how the analyzer engines fit together and what precision the pointer/alias layer delivers.
3. `veritas-thin-summarydb-backends-design.md` — how identity, hashing, and storage are laid out.
4. `veritas-evidence-ir-design.md` — the Evidence IR the Agent consumes.

Implementers:

* Milestone specs under `docs/specs/milestones/` (m1 … m12) refine sections of this architecture.
* Implementation plans under `docs/plans/` (m1 … m12) drive the concrete build order.
* The engineering-backbone spec at `docs/specs/veritas-engineering-backbone-design-specification.md` is the connective spec between this document and the milestone-level detail.

