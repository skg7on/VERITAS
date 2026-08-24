# VERITAS SummaryDB Ingest Adapter — Milestone Spec

**Status:** Milestone overview. M11 signatures remain normative; M12 details are
superseded by `m12-joern-cpg-summarydb-importer-design-spec.md`.
**Date:** 2026-08-24
**Scope:** Shared boundary between M11 native analysis of external LLVM IR and
M12 SummaryDB ingestion of external provider graphs/facts.

Architectural framing lives in `docs/architecture/01-platform-architecture.md`:

- §6 — the three-tier adapter picture.
- §7 — Tier 1: `compile_commands.json` project directory (`CodeGenIrSource`).
- §8 — Tier 2: bitcode / textual IR (`BitcodeIrSource`) with T0 / T1 / T2 fidelity.
- §9 — Tier 3: SummaryDB provider projection + external fact ingestion.
- §10 — adapter interface contracts (`ProgramIrSource`, provider importer).
- §11 — invariant B10 in its current form.

This spec fixes M11's milestone-scoped signatures and records the shared
boundary. The dedicated
[`M12 Joern CPG SummaryDB importer specification`](m12-joern-cpg-summarydb-importer-design-spec.md)
owns M12A–M12C identity, normalized graph, SummaryDB placement, external fact
batch, fusion, security, CLI, and test contracts. The existing M12 plan must be
replaced after that written specification is approved.

---

## 1. IR Adapter — component signatures (M11)

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

}  // namespace veritas::analysis::ir_adapter
```

```cpp
namespace veritas::analysis::pipeline {

class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual StatusOr<ProgramIr> Build() = 0;
};

class CodeGenIrSource : public ProgramIrSource { /* existing M4 CodeGen + link */ };
class BitcodeIrSource  : public ProgramIrSource { /* BitcodeModuleLoader + link + OriginMap */ };

}  // namespace veritas::analysis::pipeline
```

The fidelity tier is recorded into the analyzer run and provenance so downstream facts are tagged (`producer = svf`, `input_fidelity = symbols_only`, `has_source_anchors = false`).

M4's `RunLocalAnalysis` is refactored to accept a `ProgramIrSource&`; `LocalFactExtractor`, SVF, CPG, WPA, and provenance are untouched.

---

## 2. External Provider Ingestion — boundary summary (M12)

```text
Joern GraphSON / GraphML
    -> bounded provider reader
    -> RawProviderGraph
    -> schema/context validation
    -> identity resolution + semantic normalization
    -> ProviderProgramGraph + ExternalFactBatch
    -> atomic SummaryDB provider publication
```

The imported projection is stored across SummaryDB's Object, Metadata, Graph,
Fact/Provenance, Evidence Cache, and History layers. It does not mutate the M6
`ThinCpg`, native summary bindings, or WPA inputs. Provider and native
observations share a semantic query view while retaining separate authority,
epistemic state, capabilities, assumptions, and witnesses.

M12 adds `ExternalFactBatch`; it does not publish raw vectors of stringly
`ExternalFact` records. Joern ordinals remain provenance only and unresolved
subjects receive canonical hashed `ExternalEntityID` values.

---

## 3. CLI Contract

```text
veritas-build analyze --project <dir>      # existing → CodeGenIrSource
veritas-build analyze --bitcode <path>     # IR adapter → BitcodeIrSource (file or directory)
veritas-build import --joern <export> --project <dir> --output <db>
    [--format auto|graphson|graphml] [--accept-unverified-context]
```

- `--project` and `--bitcode` are mutually exclusive.
- M12 import requires an existing matching repository/revision/build binding in
  the selected SummaryDB.
- A raw export without verifiable source and build/frontend fingerprints
  requires explicit `--accept-unverified-context` assumptions for the missing
  dimensions; a verified mismatch is never overridable.
- PhASAR moves to independently designed M12D and does not share Joern's graph
  parser API.

---

## 4. Error Handling

| Failure | Policy |
| --- | --- |
| Bitcode parse/verify error | `InvalidArgument` + offending file, no partial module |
| LLVM version mismatch | `FailedPrecondition`, clear message (never silent downgrade) |
| **T2 stripped bitcode** | `FailedPrecondition` — "no stable symbol identity; keep symbols or rebuild with `-g`" |
| Duplicate symbol during link | fatal, reported (never silently merged) |
| Identity bridge is ambiguous/unresolved | hashed `ExternalEntityID` + explicit unknown; not an error |
| Joern parse error | `InvalidArgument`, atomic — no partial provider state |
| Unsupported Joern/CPG schema | `FailedPrecondition`, no publication |
| Repository/revision/build mismatch | `FailedPrecondition`, no publication |
| Import resource budget exhausted | `ResourceExhausted`, no publication |
| External → `MUST` upgrade attempt | rejected at fact-store validation (epistemic floor enforced) |

---

## 5. Testing

### IR adapter

- `.bc` with debug info → source anchors present; `FunctionSymbolID` matches mangled name.
- `.ll` (textual) → identical result to `.bc` of the same module.
- Directory of modules → linked into one `ProgramIr`, all functions present.
- T1 (symbols, no debug) → analysis succeeds, no source anchors, Evidence shows name only.
- T2 (stripped) → rejected with clear error.
- Determinism: same bitcode twice → identical summaries + `ProjectionID`.
- LLVM version mismatch → clean error.

### External provider importer

- Equivalent GraphSON/GraphML exports produce the same normalized provider
  projection identity.
- Joern `CALL` maps to `MayCall` with `INFERRED` epistemic state.
- Joern `REACHING_DEF` maps to a rooted value-flow fact.
- Structural AST/CFG topology is queryable without one fact per graph edge.
- Identity bridge resolves stable entities and preserves ambiguous/unresolved
  subjects without fabricated semantic IDs.
- Malformed, mismatched, over-budget, or invalid input fails atomically.
- Provider publication never advances native summary/M6 bindings or schedules
  native WPA.
- Imported absence never produces negative evidence.
- Full cases are specified in M12 §22.

### Boundary scans (mirroring existing M6 tests)

- Installed public headers contain no Joern/PhASAR/bitcode-parser native types.
- The provider importer and IR adapter are isolated from the M6 projection and
  SVF stages.

---

## 6. Milestone Placement and Doc Touch-Points

### Milestones (after M10C in the M0-M12 chronology)

- **M11 — External IR adapter** (depends on **M5**): `BitcodeIrSource`, `--bitcode` CLI. Reuses M4 extractor + M5 SVF unchanged.
- **M12A — SummaryDB external-provider substrate** (depends on **M2/M3/M6/M9**).
- **M12B — Joern GraphSON/GraphML importer** (depends on **M12A**).
- **M12C — Provider fusion and Evidence integration** (depends on **M10B/M12B**).
- **M12D — PhASAR result adapter** (depends on **M12A**; separate detailed design required).

### Existing docs consulted during implementation

1. `docs/architecture/01-platform-architecture.md` — the architectural home for adapter tiers (§6–§11).
2. `docs/specs/veritas-engineering-backbone-design-specification.md` — invariant B10 as rewritten to admit Tier 2 and Tier 3.
3. `docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md` — the extraction pipeline reused by both tiers.
4. `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md` — the CPG projection stage is unchanged; external artifacts never enter it.
5. `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md` — canonical M12A–M12C design and acceptance contract.
