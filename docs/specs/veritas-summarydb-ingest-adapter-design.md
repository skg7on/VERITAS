# VERITAS SummaryDB Ingest Adapter — Design

**Status:** Approved design (pre-implementation)
**Date:** 2026-08-16
**Scope:** An adapter layer that lets VERITAS build its SummaryDB from external analysis inputs, in addition to the canonical `compile_commands.json` pipeline.

---

## 1. Purpose and Scope

VERITAS today builds its SummaryDB from a single public input: a project directory containing `compile_commands.json`, from which VERITAS owns Clang AST traversal, LLVM IR generation/linking, and in-process SVF analysis (backbone invariants B10 and the milestone-map "Global Constraints").

This design adds an **ingest adapter** with two tiers of external input:

1. **IR adapter** — accept external LLVM IR artifacts (`.bc`, `.ll`, a directory of either) as the module source, skipping Clang CodeGen but running VERITAS's own analysis (SVF, local fact extraction, CPG, WPA, provenance) unchanged.
2. **External-facts importer** — accept already-generated analysis results (Joern CPG export, PhASAR result) and map them into VERITAS's fact store as provenance-tagged, epistemic-lowered external observations.

### In scope

- `.bc` (LLVM bitcode), single file.
- `.ll` (LLVM textual IR), single file.
- A directory of `.bc`/`.ll` files, linked into one `ProgramIr`.
- Joern CPG export (GraphML and JSON) as external facts.
- PhASAR result as external facts.

### Out of scope / non-goals

- Cross-path identity convergence: a function analyzed from source vs. from bitcode lives in a **different build variant** (different source tree / module inputs), so its `FunctionVariantID` is already distinct. Source-derived and bitcode-derived summaries are **not** required to diff against each other in v1.
- Full recovery of Clang-only identity (USRs, qualified names, template specialization details) from bitcode.
- Making Joern/PhASAR inputs participate in incremental WPA invalidation or SCC propagation.

### Design stance relative to the merged M6 design

PR #17 ("LLVM-native CPG projection") made M6 build VERITAS's **own** CPG from the live `ProgramIr` and did not use Joern/PhASAR as a CPG generator. This design **preserves** that stance in full: VERITAS still builds its own CPG. The adapter only changes the **input boundary** — external artifacts enter through separate stages (the IR adapter at module acquisition, the importer as external facts), never through the CPG projection stage.

---

## 2. Invariant Change

The backbone invariant **B10** currently reads:

> *"The only public source input is a project directory containing `compile_commands.json`; VERITAS owns AST, IR, and SVF execution."*

This design rewrites it to:

> *"The only public **source** input is a project directory containing `compile_commands.json`. External **LLVM IR artifacts** (`.bc`/`.ll`/bitcode directory) are additionally accepted as a **module** input through the IR adapter; they enter at the module-acquisition boundary (skipping Clang CodeGen) and never bypass VERITAS's own analysis or identity/provenance derivation. External **analysis results** (Joern export, PhASAR result) are accepted only through the external-facts importer as epistemic-lowered observations."*

All other invariants (B1 semantic identity, B2 immutability, B6 provenance, B7 epistemic separation, P8 external-output-is-a-hypothesis) are unchanged.

---

## 3. IR Adapter

### 3.1 Fidelity tiers

Bitcode carries a variable amount of what identity needs:

| Tier | Bitcode contents | Recoverable | Policy |
| --- | --- | --- | --- |
| T0 | Debug info (`llvm.dbg` + `-g`) | mangled name, signature, source file:line, type layout | Full analysis; source anchors preserved |
| T1 | Symbols, no debug info | mangled name, reconstructed signature (from IR types), linkage | Analyze; **no source anchors** (Evidence shows function name only) |
| T2 | Stripped (no symbols) | nothing stable | **Reject** with a clear error |

T2 is rejected because VERITAS's first invariant is *semantic identity*; a module with no stable symbol identity cannot produce valid `FunctionSymbolID`s.

### 3.2 The M4 refactor

M4 currently does two things in one call: *generate the module* (Clang CodeGen + link) and *extract local facts*. This design splits them behind a `ProgramIrSource` abstraction:

```
before:  RunLocalAnalysis(AnalysisManifest) → {ProgramIr, summary_drafts}
after:   RunLocalAnalysis(ProgramIrSource&) → {ProgramIr, summary_drafts}
```

`LocalFactExtractor`, SVF, CPG, WPA, and provenance are **not touched**.

### 3.3 Components

```cpp
namespace veritas::analysis::ir_adapter {

enum class BitcodeFidelity { DebugInfo, SymbolsOnly, Stripped };

struct BitcodeInput {
  enum class Kind { SingleFile, Directory };
  Kind kind;
  std::filesystem::path path;   // file, or directory to enumerate *.bc / *.ll
};

// Parse + verify each module; no linking yet.
class BitcodeModuleLoader {
 public:
  StatusOr<std::vector<std::unique_ptr<llvm::Module>>> LoadAll(const BitcodeInput&);
  BitcodeFidelity DetectFidelity(llvm::Module&);          // T0/T1/T2
};

// Merge modules into the single private ProgramIr (reuses M4's llvm::Linker logic).
StatusOr<ProgramIr> LinkIntoProgramIr(std::vector<std::unique_ptr<llvm::Module>>);

// T0: from DISubprogram/DILocation → source anchors. T1: empty (name-only).
StatusOr<OriginMap> BuildOriginMap(llvm::Module&, BitcodeFidelity);
}
```

```cpp
namespace veritas::analysis::pipeline {

class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual StatusOr<ProgramIr> Build() = 0;
};

class CodeGenIrSource : public ProgramIrSource { /* existing M4 CodeGen + link */ };
class BitcodeIrSource : public ProgramIrSource { /* BitcodeModuleLoader + link + OriginMap */ };
}
```

The fidelity tier is recorded into the analyzer run / provenance so downstream facts are tagged (e.g., `producer = svf`, `input_fidelity = symbols_only`, `has_source_anchors = false`).

### 3.4 Data flow

```
[ .bc | .ll | directory ]
   → BitcodeInput
   → BitcodeModuleLoader.LoadAll      (parse + verify)
   → DetectFidelity                    (T2/stripped → reject)
   → LinkIntoProgramIr                 (llvm::Linker)
   → BuildOriginMap                    (debug info if present)
   → ProgramIr (OriginMap + fidelity recorded)
   → LocalFactExtractor   (M4, unchanged)
   → SVF                  (M5, unchanged)
   → summary_drafts → publish (M3, unchanged)
```

---

## 4. External-Facts Importer (Joern + PhASAR)

Delivered in a later milestone than the IR adapter. This tier does **not** touch the CPG projection stage (M6), SVF, or the summary pipeline.

### 4.1 Components

```cpp
namespace veritas::facts::external {

enum class ExternalProducer { Joern, Phasar };

// One imported observation; never a FunctionSummary.
struct ExternalFact {
  ExternalProducer producer;
  std::string external_id;          // Joern node ID / PhASAR fact ID
  core::StableId subject_id;        // VERITAS entity if resolved, else synthetic external:<prod>:<id>
  std::string predicate_kind;       // normalized VERITAS predicate vocabulary
  std::string predicate_canonical;
  EpistemicState epistemic;         // always INFERRED or ASSUMED, never MUST
  std::optional<core::StableId> source_anchor_id;
  std::string raw_record;           // verbatim source record for provenance
};

class JoernCpgImporter {
 public:
  StatusOr<std::vector<ExternalFact>> ImportGraphML(const std::filesystem::path&);
  StatusOr<std::vector<ExternalFact>> ImportJson(const std::filesystem::path&);
};

class PhasarResultImporter {
 public:
  StatusOr<std::vector<ExternalFact>> Import(const std::filesystem::path&);
};

// Resolves external entities to VERITAS IDs where a stable bridge exists.
class ExternalIdentityBridge {
 public:
  StatusOr<core::StableId> Resolve(ExternalProducer, const std::string& external_ref);
};
}
```

### 4.2 Rules

1. **Identity bridge** — `ExternalIdentityBridge` resolves an external entity to a VERITAS stable ID *only* via stable inputs (mangled name → `FunctionVariantID`, `file:line` → `SourceAnchorID`). Unresolvable entities get a synthetic `external:<producer>:<external_id>` subject. **Never** fabricate a VERITAS semantic ID from a Joern/PhASAR ordinal.
2. **Epistemic floor** — external facts enter as `INFERRED` (from a semantic analysis) or `ASSUMED` (accepted as a premise). The `MUST` state is unreachable from external input. This matches backbone `P8` and the `INFERRED → INFERRED` propagation rule.
3. **Producer/trust** — provenance records `producer_kind = external`, `producer_id = joern|phasar`; trust level is assigned per-deployment (backbone §58–59). No `EXTERNAL_MODEL` fact silently becomes a `VERIFIED_FACT`.
4. **No WPA participation** — external facts are terminal facts, not summary components, so they do not drive incremental invalidation or SCC propagation. They *are* citable by Evidence Builder and queryable in the fact store.
5. **Vocabulary normalization** — Joern/PhASAR predicates are mapped into VERITAS's fact vocabulary (e.g., Joern `CALL` → `CallFact`, `REACHING_DEF` → `ValueFlowFact`); unmappable predicates are stored as opaque `external_observation` with the raw record retained for provenance.

### 4.3 Data flow

```
[Joern export | PhASAR result]
   → importer (parse + bridge + normalize)
   → ExternalFact[]  (epistemic = INFERRED/ASSUMED)
   → fact store + provenance store   (producer = external)
   → Evidence Builder / veritas-query can cite them
```

---

## 5. CLI Contract

```text
veritas-build analyze --project <dir>      # existing → CodeGenIrSource
veritas-build analyze --bitcode <path>     # IR adapter → BitcodeIrSource (file or directory)
veritas-build import  --joern  <export>    # external importer → JoernCpgImporter
veritas-build import  --phasar <result>    # external importer → PhasarResultImporter
```

- `--project` and `--bitcode` are mutually exclusive.
- External ingestion lives on `veritas-build` (a new `import` subcommand) rather than a fifth tool binary; it can be split into a separate tool later if it grows.

---

## 6. Error Handling

| Failure | Policy |
| --- | --- |
| Bitcode parse/verify error | `InvalidArgument` + offending file, no partial module |
| LLVM version mismatch | `FailedPrecondition`, clear message (never silent downgrade) |
| **T2 stripped bitcode** | `FailedPrecondition` — "no stable symbol identity; keep symbols or rebuild with `-g`" |
| Duplicate symbol during link | fatal, reported (never silently merged) |
| Identity bridge can't resolve | `external:<prod>:<id>` synthetic subject + provenance flag (not an error) |
| Joern/PhASAR parse or schema-version error | `InvalidArgument`, **atomic** — no partial facts published |
| External → `MUST` upgrade attempt | rejected at fact-store validation (epistemic floor enforced) |

---

## 7. Testing

### IR adapter

- `.bc` with debug info → source anchors present; `FunctionSymbolID` matches mangled name.
- `.ll` (textual) → identical result to `.bc` of the same module.
- Directory of modules → linked into one `ProgramIr`, all functions present.
- T1 (symbols, no debug) → analysis succeeds, no source anchors, Evidence shows name only.
- T2 (stripped) → rejected with clear error.
- Determinism: same bitcode twice → identical summaries + `ProjectionID`.
- LLVM version mismatch → clean error.

### External importer

- Joern `CALL` edge → `CallFact` (producer=joern, epistemic=INFERRED).
- PhASAR result → `ValueFlowFact` (or opaque observation), epistemic=INFERRED.
- Identity bridge resolves mangled name → `FunctionVariantID`.
- Unresolvable entity → synthetic `external:` ID.
- Malformed file → atomic failure, no partial facts.
- External fact never reaches `MUST` / never becomes `VERIFIED_FACT`.

### Boundary scans (mirroring existing M6 tests)

- Installed public headers contain no Joern/PhASAR/bitcode-parser native types.
- The importer and IR adapter are isolated from the CPG projection and SVF stages.

---

## 8. Milestone Placement and Doc Touch-Points

### New milestones (appended to M0–M10)

- **M11 — External IR adapter** (depends on **M5**): `BitcodeIrSource`, `--bitcode` CLI. Reuses M4 extractor + M5 SVF unchanged.
- **M12 — External-facts importer** (depends on **M9**): Joern/PhASAR importers + `veritas-build import`. Needs the fact + provenance stores.

### Existing docs to revise during implementation

1. **Backbone spec** (`veritas-engineering-backbone-design-specification.md`) — rewrite **B10** (allow bitcode at module-acquisition + external facts via importer) and add an external-input note.
2. **Backbone plan** (`veritas-backbone-milestones-and-implementation-plan.md`) — update "Global Constraints" (drop "no bitcode input" absolutism), add M11/M12 to the milestone map, adjust M4 exit criteria.
3. **M4 spec** (`m4-clang-llvm-local-extraction-design-spec.md`) — relax the "public CLI accepts no bitcode" test/constraint.
4. **M6 spec** (`m6-thin-veritas-cpg-projection-design-spec.md`) — re-scope §2 "accepts no `.bc`/`.ll`/Joern/PhASAR" to "the CPG *projection stage* accepts no artifacts; those arrive via M11/M12 stages."
