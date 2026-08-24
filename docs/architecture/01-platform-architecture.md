# VERITAS Platform Architecture

## System Pipeline, Design Principles, and Ingest Adapters

**Status:** Draft Architecture Specification
**Version:** 0.2
**Project:** VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics
**Peers:**

* `docs/architecture/02-whole-program-analysis-architecture.md` — analyzer engines (AST/CFG/DDG/VFG/CPG) and SOTA C/C++ alias policy.
* `docs/architecture/03-summarydb-storage-architecture.md` — SummaryDB physical layers and pluggable storage backends.
* `docs/architecture/04-evidence-ir-architecture.md` — Evidence IR formal syntax and semantics.
* `docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md` — approved M8R WPA ownership and M9 entry gate.

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
* **SummaryDB** — a logical subsystem (object store · metadata · fact store · graph index · dependency index · evidence cache · history store) that keeps summaries immutable and incrementally maintainable. See `03-summarydb-storage-architecture.md`.
* **Evidence IR (EIR)** — a claim-oriented, provenance-preserving typed graph IR that mediates the loop between whole-program analysis and the Review Agent. See `04-evidence-ir-architecture.md`.

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

Function Summary IR is the durable WPA boundary. Whole-program analysis loads
detailed AST or IR only when refinement demands it. Typed `relations.v2` rows
are reconstructed as a run-local execution projection and never become another
durable platform IR. Everyday queries run over summaries and the CPG
projection.

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
      local static analysis    (see 02-whole-program-analysis-architecture.md)
                │
                ▼
     Function Summary IR v2 (immutable, component-hashed)
                │
                ▼
   ┌─────────── SummaryDB ────────────┐
   │   object · metadata · facts       │  (see 03-summarydb-storage-architecture.md)
   │   graph index · dep index         │
   │   evidence cache · history        │
   └─────────────────┬─────────────────┘
                     │
                     ▼
        WPA input materializer (run-local relations.v2)
                     │
                     ▼
        Incremental WPA (SCC + compiled Souffle)
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

**Current versus approved target.** Implemented M8 currently uses the C++
fixpoint engine and can optionally compare file-based Souffle output. The
approved M8R target, which is not yet delivered, keeps pinned SVF authoritative
for V1 points-to, aliases, SVFG, and indirect calls; requires compiled Souffle
for normal production recursive WPA; and restricts C++ to conformance or an
explicit `cpp-emergency` engine. There is no automatic fallback. See the
[M8R bridge specification](../specs/milestones/m08r-souffle-wpa-remediation-design-spec.md)
for delivery status and the executable M9 gate.

---

# 5. Subsystem Split

Responsibilities are cleanly divided across the three architecture documents.

| Concern | Document |
| --- | --- |
| Ingest surface, adapter tiers, invariants, principles, milestones, CLI, end-to-end pipeline | this document |
| Analyzer engines: AST, CFG, dominators, DDG, VFG, thin CPG projection, call-graph, memory-effect model, range/constraint model, SOTA C/C++ pointer-alias policy, in-process SVF integration | `02-whole-program-analysis-architecture.md` |
| Storage physical layers, pluggable backend contract, RocksDB/SQLite reference bindings, identity model, content-addressed CAS, component hashes, publication atomicity, evidence cache, history store | `03-summarydb-storage-architecture.md` |
| Evidence IR syntax, semantics, epistemic lattice, provenance DAG, proof obligations | `04-evidence-ir-architecture.md` |

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
commands    single file   of bitcode  export         result      providers
.json                                 (GraphSON/GraphML)
   │          │               │        │                 │          │
   └────┬─────┴───────┬───────┘        └─────────┬───────┴──────────┘
        ▼             ▼                          ▼
  CodeGenIrSource  BitcodeIrSource        Provider Importers
        (Tier 1)        (Tier 2)               (Tier 3)
        │             │                          │
        ▼             ▼                          ▼
   [ Clang CodeGen +               [ normalized provider projection
     Link → ProgramIr ]              → SummaryDB graph index
                                     + fact/provenance stores ]
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

1. **Module acquisition vs. provider observations.** Tiers 1 and 2 produce a
   private `ProgramIr` and then flow through the full native analysis pipeline.
   Tier 3 publishes a separate provider projection plus selected semantic facts
   into SummaryDB. It never mutates M6's native projection or participates in
   native summaries, SCC propagation, or WPA.
2. **VERITAS owns analysis.** No adapter runs VERITAS's own analysis pipeline for the caller. Local extraction, SVF, CPG projection, WPA, and provenance derivation are always executed by VERITAS on the module it acquired.
3. **Semantic identity is derived, never carried.** LLVM pointer identity and
   Joern/PhASAR ordinals never become semantic graph or fact IDs. Provider-side
   IDs may be retained only inside hashed provider-record identities and rooted
   provenance. Canonical references use VERITAS stable IDs (see
   `03-summarydb-storage-architecture.md` §5).

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

# 9. Tier 3 — External Provider Projections

Tier 3 accepts already-computed provider artifacts. M12 directly parses Joern
whole-graph GraphSON/GraphML; independently designed adapters may later accept
PhASAR or other result schemas. The normalized result is integrated across
SummaryDB rather than reduced to a stringly vector of terminal facts.

## 9.1 What imports become

```text
ProviderArtifact + ProviderRun + ProviderCapabilitySet
    |
    +-> ProviderProgramGraph
    |      canonical program entities/relations
    |      provider observations and extensions
    |      -> SummaryDB Graph Index
    |
    +-> ExternalFactBatch
           registered relations.v2 facts
           run bindings, assumptions, rooted witnesses
           -> Fact + Provenance Stores
```

The Graph Index retains structural topology such as AST and CFG without
inflating every edge into a fact. Registered semantic relations such as
`MayCall`, `DefUse`, and control dependence may additionally become facts.

## 9.2 Rules of admission

1. **Context binding.** An import must match an existing VERITAS repository,
   revision, and build-variant binding. Provider metadata cannot create that
   context by itself. If the raw export has no verifiable source fingerprint,
   or does not prove compatible build/frontend configuration, import requires
   explicit user-asserted context assumptions inherited by every observation;
   a verified source or build mismatch is always rejected.
2. **Three-level identity.** Provider record, source occurrence, and semantic
   program entity are distinct. Joern/PhASAR ordinals remain provenance only.
   Unresolved entities receive canonical hashed external IDs, never fabricated
   native identities.
3. **Epistemic floor.** External facts enter as `INFERRED` or `ASSUMED`; `MUST`
   is rejected. Relation modality (for example `MayCall`) remains separate.
4. **Provider overlay, not native mutation.** Provider projections do not alter
   M6, Summary IR, native dependency invalidation, SCCs, or WPA. They may
   invalidate provider-dependent queries and Evidence cases.
5. **Capabilities and assumptions are durable.** Overlay availability,
   frontend limitations, external-call semantics, unresolved resolution, and
   provider-side truncation accompany affected observations.
6. **Open-world absence.** A missing provider relation never becomes negative
   evidence or satisfies a VERITAS closed-world completion rule.
7. **Extensible vocabulary.** Known constructs normalize into provider-neutral
   entities and relations. Unknown extensions remain inert, typed, queryable
   provider observations instead of being dropped.
8. **Atomicity.** The provider graph, facts, witnesses, history, and current
   provider binding become visible together or not at all.

## 9.3 Pipeline

```text
[ Joern GraphSON | Joern GraphML ]
       |
       v
bounded reader -> RawProviderGraph -> schema/context validation
       |
       v
identity resolution + semantic normalization
       |
       +-> ProviderProgramGraph -> SummaryDB Graph Index
       +-> ExternalFactBatch    -> Fact/Provenance Stores
       +-> capabilities/extensions/assumptions -> Metadata/History
       |
       v
UnifiedProgramGraphQuery / EvidenceQueryService
```

Detailed identity, graph, publication, CLI, security, and acceptance contracts
are defined by
`docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`.

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

## 10.2 Provider importer — SummaryDB projection publication (Tier 3)

```text
ProviderGraphReader     -> RawProviderGraph
ProviderNormalizer      -> ProviderProgramGraph + ExternalFactBatch
ProviderPublisher       -> atomic SummaryDB provider binding
UnifiedProgramGraphQuery -> pinned native + selected provider snapshot
```

The reader boundary owns GraphSON/GraphML-specific types. The normalizer emits
provider-neutral stable records. The publisher validates graph identity,
expected/completed fact components, rooted witnesses, and epistemic floor
before one cross-layer visibility transaction.

M12's concrete contracts and tests live in
`docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`.

---

# 11. Invariants

VERITAS carries a set of platform-wide invariants. Analysis-specific invariants are stated in `02-whole-program-analysis-architecture.md`; storage-specific invariants in `03-summarydb-storage-architecture.md`. The invariants below govern the platform as a whole and the ingest boundary in particular.

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
| B10 | The only public **source** input is a project directory containing `compile_commands.json`. External LLVM IR enters only at module acquisition and still runs VERITAS analysis. External provider artifacts enter only through a context-bound, normalized, epistemic-lowered SummaryDB provider projection; they never mutate native summaries/M6 or bypass identity/provenance validation. | Allows optional cross-tool evidence while preserving reproducibility, native authority, and controlled ingest boundaries. |

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
veritas-build import --joern <export> --project <dir> --output <db>
    [--format auto|graphson|graphml]
    [--accept-unverified-context]                   # Tier 3

veritas-query summary   <symbol>
veritas-query callers   <symbol>
veritas-query writes    <global>
veritas-query evidence  overflow  <sink>

veritas-diff  <rev-a>  <rev-b>
veritas-explain fact <fact-id> --run <run-id>
```

Contracts:

* `--project` and `--bitcode` are **mutually exclusive** (exactly one is required).
* External ingestion lives on `veritas-build import` rather than a separate tool; it can be split later if the surface grows.
* Tier-3 import requires an existing matching repository/revision/build binding.
* Read commands select native and provider projections explicitly; the ordered
  selection is part of the immutable query snapshot.
* All read commands take the current published SummaryDB revision by default and accept `--at <rev>` for historical queries.
* Fact explanation requires `--run <run-id>`. Canonical `FactID` may be shared
  across runs while its selected witness is occurrence-specific; the current
  revision or `--at <rev>` cannot disambiguate build, configuration, engine,
  or witness selection.

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

M0-M8 are implemented history. Five remediation gates are inserted between M8
and M9; M9 begins only after all ten M8R entry criteria pass with no missing,
extra, disabled, skipped, failed, or errored executable tests. Future M10 is
split into recursive domain expansion, Evidence Builder input delivery, and
Evidence IR semantic modeling and serialization.

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
| M8 | Implemented SCC-aware WPA: C++ fixpoint plus optional Souffle comparison. |
| M8R.1 | Semantic fact contract. |
| M8R.2 | SVF and memory refinement with native `summary.v2`. |
| M8R.3 | Run-local `relations.v2` WPA projection and generic rooted witnesses. |
| M8R.4 | Required compiled-Souffle production WPA; C++ oracle/emergency only. |
| M8R.5 | Qualification, complete `AnalysisFactBatch`, Fact Bus, and M9 entry gate. |
| M9 | Provenance fact store + explain API, after all M8R gates pass. |
| M10A | Recursive domain expansion (`MayRead`, `GlobalFlow`, `UnknownEffect`, `SoundnessCoverage`). |
| M10B | Evidence Builder input APIs + first end-to-end demo over M9/M10A facts. |
| M10C | Validated, canonical Evidence IR with EIR-T, Protobuf, and full-EIR diagnostic JSON serialization. |
| M11 | External IR adapter (`BitcodeIrSource`, `veritas-build analyze --bitcode`). |
| M12A | SummaryDB external-provider graph/fact substrate and atomic publication. |
| M12B | Direct Joern GraphSON/GraphML importer and normalization. |
| M12C | Provider fusion, provider-aware queries, and M10B integration. |
| M12D | Separately designed PhASAR result adapter over the M12A substrate. |
| M13 | Benchmark-gated Souffle PTA research, independent of the M9-M12 critical path. |

The Review Agent and its verification loop are post-backbone milestones and are
not required for the platform's V1 usefulness. M10C supplies the stable EIR
boundary they will consume. See §17 for the first target demo.

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
    M10C Evidence IR generated:
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
     CodeGenIrSource  BitcodeIrSource  ProviderImport
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
                  WPA input materializer
              native Summary IR/facts only
                  run-local relations.v2
                             │
                             ▼
                    Incremental WPA
                  SCC / compiled Souffle
                  C++ oracle or explicit
                     emergency engine
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

The WPA arrow selects only native Summary IR and native WPA inputs. Provider
projections remain excluded from recursion and rejoin native/global facts only
through the pinned SummaryDB query snapshot used by Evidence Builder.

The Agent's role is compact: interpret evidence, rank findings, identify missing semantics, infer API contracts, request additional analysis. It does not directly promote any claim to `VERIFIED_DEFECT` or `VERIFIED_SAFE`; those transitions require a deterministic verifier per P8.

---

# 19. Reading Order

New readers:

1. This document — platform pipeline, principles, ingest boundary.
2. `02-whole-program-analysis-architecture.md` — how the analyzer engines fit together and what precision the pointer/alias layer delivers.
3. `03-summarydb-storage-architecture.md` — how identity, hashing, and storage are laid out.
4. `04-evidence-ir-architecture.md` — the Evidence IR the Agent consumes.
5. `../specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md`
   and `../plans/milestones/m10c-evidence-ir-semantic-model-serialization-implementation-plan.md`
   — the backbone milestone that realizes the EIR semantic and representation
   boundary.

Implementers:

* Historical and normal milestone specs live under `docs/specs/milestones/`;
  their available implementation plans live under `docs/plans/`.
* M8R uses the canonical
  [`M8R bridge spec`](../specs/milestones/m08r-souffle-wpa-remediation-design-spec.md)
  and
  [`remediation implementation plan`](../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md).
* M10A's detailed design and implementation plan are still pending. M13 is the
  separately approved, benchmark-gated research scope in the
  [architecture refinement design](../specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md#m13--benchmark-gated-pta-research)
  and remains independent of the M9-M12 critical path.
* The
  [engineering-backbone spec](../specs/veritas-engineering-backbone-design-specification.md)
  connects this document to milestone-level detail.
