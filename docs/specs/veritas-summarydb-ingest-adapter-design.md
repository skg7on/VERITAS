# VERITAS SummaryDB Ingest Adapter — Milestone Spec

**Status:** Milestone spec (M11 + M12). Superseded for architectural content by `docs/architecture/veritas-platform-architecture-design.md`.
**Date:** 2026-08-16
**Scope:** Concrete interface signatures, CLI contract, error handling, and testing for the two-tier ingest adapter delivered by milestones M11 (external IR adapter) and M12 (external-facts importer).

Architectural framing lives in `docs/architecture/veritas-platform-architecture-design.md`:

- §6 — the three-tier adapter picture.
- §7 — Tier 1: `compile_commands.json` project directory (`CodeGenIrSource`).
- §8 — Tier 2: bitcode / textual IR (`BitcodeIrSource`) with T0 / T1 / T2 fidelity.
- §9 — Tier 3: external-facts importer (Joern / PhASAR) with epistemic floor.
- §10 — adapter interface contract (`ProgramIrSource`, `ExternalFactsImporter`).
- §11 — invariant B10 in its current form.

This spec fixes the milestone-scoped C++ signatures, CLI contract, error policy, and test matrix. It is the reference for `docs/plans/m11-external-ir-adapter-implementation-plan.md` and `docs/plans/m12-external-facts-importer-implementation-plan.md`.

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

## 2. External-Facts Importer — component signatures (M12)

```cpp
namespace veritas::facts::external {

enum class ExternalProducer { Joern, Phasar };

// One imported observation; never a FunctionSummary.
struct ExternalFact {
  ExternalProducer producer;
  std::string external_id;          // Joern node ID / PhASAR fact ID
  core::StableId subject_id;        // VERITAS entity if resolved,
                                    // else synthetic external:<prod>:<id>
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

}  // namespace veritas::facts::external
```

Admission rules (identity bridge, epistemic floor, producer/trust, no-WPA-participation, vocabulary normalization) are stated in `docs/architecture/veritas-platform-architecture-design.md` §9.2 and are enforced at fact-store write time.

---

## 3. CLI Contract

```text
veritas-build analyze --project <dir>      # existing → CodeGenIrSource
veritas-build analyze --bitcode <path>     # IR adapter → BitcodeIrSource (file or directory)
veritas-build import  --joern  <export>    # external importer → JoernCpgImporter
veritas-build import  --phasar <result>    # external importer → PhasarResultImporter
```

- `--project` and `--bitcode` are mutually exclusive.
- External ingestion lives on `veritas-build` (a new `import` subcommand) rather than a fifth tool binary; it can be split into a separate tool later if it grows.

---

## 4. Error Handling

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

## 5. Testing

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

## 6. Milestone Placement and Doc Touch-Points

### Milestones (after M10B in the M0-M12 chronology)

- **M11 — External IR adapter** (depends on **M5**): `BitcodeIrSource`, `--bitcode` CLI. Reuses M4 extractor + M5 SVF unchanged.
- **M12 — External-facts importer** (depends on **M9**): Joern/PhASAR importers + `veritas-build import`. Needs the fact + provenance stores.

### Existing docs consulted during implementation

1. `docs/architecture/veritas-platform-architecture-design.md` — the architectural home for adapter tiers (§6–§11).
2. `docs/specs/veritas-engineering-backbone-design-specification.md` — invariant B10 as rewritten to admit Tier 2 and Tier 3.
3. `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md` — the extraction pipeline reused by both tiers.
4. `docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md` — the CPG projection stage is unchanged; external artifacts never enter it.
