# Link-Unit / Program-Boundary Design Specification

**Status:** Proposed design; guardrail implemented, full boundary awareness pending

**Scope:** M1 ingestion, identity model, `ProjectIrBuilder`, and WPA orchestration

**Primary inputs:** `compile_commands.json` exported from multi-target builds

## 1. Purpose

VERITAS performs whole-program analysis: local translation units are linked into
a single LLVM module and SVF/Soufflé compute program-wide facts over it. That
assumption is violated by the most common real-world `compile_commands.json`,
which a CMake (or Ninja/Bear) export produces for an **entire build tree**, not
for one program. A single database then contains translation units from the
library, its executables, its tests, and third-party dependencies — several
**link units**, each a separate program.

This specification defines:

- the concept of a **link unit** and how it is inferred from `compile_commands.json`;
- where link-unit identity sits in VERITAS's `RepositoryID → … → FunctionSummaryID`
  chain;
- the M1 schema, `ProjectIrBuilder`, and WPA-orchestration changes that make
  analysis per-link-unit instead of per-database; and
- the interim **guardrail** that fails clearly on multi-program input until the
  full change lands.

## 2. Current baseline and boundary

`ProjectIrBuilder::BuildProjectIr` (in `src/analysis/llvm/ProjectIrBuilder.cpp`)
builds one module per translation unit, then folds them all into the first via
`llvm::Linker::linkInModule`:

```cpp
auto linked = std::move(modules.front());
::llvm::Linker linker(*linked);
for (std::size_t i = 1; i < modules.size(); ++i) {
  if (linker.linkInModule(std::move(modules[i]))) {
    return Status::FailedPrecondition(
        "failed to link translation unit into whole-program module");
  }
}
```

`llvm::Linker` rejects two strong external definitions of the same symbol
(`llvm/lib/Linker/LinkModules.cpp:326`, "symbol multiply defined"). A
multi-target database defines `main` in every executable's translation unit, so
linking fails with that raw error. VERITAS currently has no notion of which
translation units belong to which program: `TranslationUnitCommand` records only
the source path, working directory, normalized arguments, and identity hashes —
the `output` field (the object file path) is discarded during ingestion.

The identity chain is `RepositoryID → RevisionID → BuildVariantID →
FunctionSymbolID → FunctionVariantID → FunctionBodyID → FunctionSummaryID`.
`BuildVariantID` currently denotes a **compiler + options** configuration (see
the `mixed_compilers_ab`/`mixed_compilers_ba` fixtures); it does not encode
program membership.

## 3. Problem statement

A single `compile_commands.json` can span many programs. Concretely, a leveldb
build export contains 97 translation units across 13 CMake targets:

| target | TUs | role |
| --- | ---: | --- |
| `leveldb` | 38 | static library (no `main`) |
| `leveldb_tests` | 24 | test executable |
| `db_bench`, `db_bench_sqlite3` | 6 | benchmarks |
| `c_test`, `leveldbutil`, `env_posix_test`, `env_windows_test` | 7 | tools/tests |
| `benchmark`, `benchmark_main`, `gmock`, `gmock_main`, `gtest`, `gtest_main` | ~22 | third-party test deps |

70 of the 97 translation units define `main`. Linking them into one module is
both mechanically impossible and semantically wrong: whole-program facts would
flow across `db_bench`, `c_test`, and the library as if they were one program.

## 4. Goals and non-goals

**Goals**

- Fail with an actionable error today (see §9) rather than a raw linker error.
- Define link-unit identity as a first-class, content-addressed dimension.
- Analyze each link unit independently: one IR module, one SVF/WPA pass, and
  link-unit-scoped facts per program.
- Keep single-target databases working unchanged.

**Non-goals**

- Reconstruct link commands from `compile_commands.json` (the database records
  compilation, not linking). Link-unit inference is from the `output` field, not
  from linker invocations.
- Cross-program interprocedural analysis (a query that spans two programs is a
  separate, future concern).
- Inferring the library/executable distinction from source semantics; that comes
  from the target's `output` shape and entry-point presence, not from scanning.

## 5. Terminology

- **Translation unit (TU):** one compilation of one source file. Already the
  granularity of `TranslationUnitCommand`.
- **Link unit** (equivalently **program** or **target**): the set of TUs linked
  into one executable or library. This is the unit of whole-program analysis.
- **Entry point:** a `main` definition; present in executables, absent in
  libraries.

## 6. Inferring link units from `compile_commands.json`

The `output` field is the reliable signal. CMake writes it as:

```text
<build>/CMakeFiles/<target>.dir/<source-relative-path>.o
```

The `<target>` segment is the link-unit key. The inference rule:

1. If an entry carries a non-empty `output`, derive the link unit from the
   object's directory relative to the build root: strip a trailing
   `CMakeFiles/<target>.dir` (CMake convention) when present, else take the
   object's parent directory. Entries sharing a link-unit key form one program.
2. If an entry has no `output`, fall back to a single anonymous link unit keyed
   by the whole database (preserving today's behavior), with the §9 guardrail
   still catching multiple entry points.

The `output` field is optional per the spec, so inference must degrade
gracefully. Because VERITAS's primary ingestion target is CMake (the
`compile_commands.json` generator in the canonical build path), the
`CMakeFiles/<target>.dir` convention is a strong, deterministic signal.

## 7. Identity model

Insert a **LinkUnitID** into the chain between `BuildVariantID` and
`FunctionSymbolID`:

```text
RepositoryID → RevisionID → BuildVariantID → LinkUnitID
  → FunctionSymbolID → FunctionVariantID → FunctionBodyID → FunctionSummaryID
```

Rationale:

- `BuildVariantID` already means "compiler + options"; two programs built by the
  same compiler/options share a build variant but must remain distinct programs.
- A link unit is content-derived: `LinkUnitID = sha256(version ‖
  canonical-member-TU-id-set ‖ link-kind)` so identity is stable under TU
  reordering and survives moving a target between databases.

Every downstream fact — function summaries, SVF points-to, Soufflé relations —
becomes scoped by `LinkUnitID`. `BuildVariantID` remains the parent; a link
unit belongs to exactly one build variant.

## 8. Schema and pipeline changes

### 8.1 M1 ingestion (`ProjectManifestLoader`, `AnalysisManifest`)

- Capture the `output` field from each database entry.
- Add to `TranslationUnitCommand`: `std::string output_path` (absolute or
  build-root-relative) and `std::string link_unit_id`.
- Add to `AnalysisManifest`: an ordered `std::vector<LinkUnit>` where
  `LinkUnit { link_unit_id, kind, member_tu_ids, entry_point_count }`.
- Extend `ToCanonicalBytes` / `ToDiagnosticJson` to encode the new fields with
  stable, ordering-independent serialization (link units sorted by id; member TU
  ids sorted).

### 8.2 IR building (`ProjectIrBuilder`)

- Group `manifest.translation_units` by `link_unit_id`.
- Build and link one `pipeline::ProgramIr` per link unit; the entry-point
  multiplicity guardrail becomes per-link-unit (one `main` per executable unit).
- The current single-module path is retained as the degenerate single-link-unit
  case.

### 8.3 WPA orchestration

- Run SVF/Soufflé per link unit, not per database.
- Key SummaryDB facts and WPA run/component state by `LinkUnitID`.
- The `ProgramContext` (and its `build_variant_id`) remains the parent scope; the
  analyzer iterates link units within it.

### 8.4 CLI

- Add a `--link-unit <id|name>` selector; with no selector, analyze all link
  units in deterministic order, publishing facts scoped by `LinkUnitID`.

## 9. Interim guardrail (implemented)

Until §8 lands, `ProjectIrBuilder::BuildProjectIr` detects when more than one
translation unit defines `main` and fails with an actionable message, e.g.:

```text
compile_commands.json spans multiple programs: 70 translation units define
`main`. VERITAS analyzes one program per invocation; restrict --project to a
single target (or the library's translation units). Entry points:
  …/db_bench.cc
  …/c_test.c
  … and 68 more
```

This turns the raw LLVM error into guidance without silently merging programs.
It is deliberately a stopgap: it detects the *symptom* (multiple entry points),
not the full link-unit structure, and a library-only database (no `main` at all)
still analyzes correctly.

## 10. Implementation ordering

1. **Guardrail** (done): clear failure on multiple `main`s.
2. **M1 schema**: capture `output`, derive `link_unit_id`, serialize link units.
3. **IR builder**: per-link-unit module building and linking.
4. **WPA**: per-link-unit orchestration and link-unit-scoped facts.
5. **CLI**: link-unit selection; multi-unit deterministic sweep.
6. **Regression fixtures**: `multiple_mains` (guardrail) plus a mixed
   library+executable fixture proving per-unit isolation.

## 11. Open questions

- Should `LinkUnitID` be a new node in the identity chain (§7), or should the
  link unit be folded into a widened `BuildVariantID`? This spec recommends a
  new node to keep "compiler+options" and "program" orthogonal.
- When a database has no `output` fields at all, is single-link-unit fallback the
  right default, or should ingestion require an explicit `--link-unit` mode?
- Should cross-program facts (e.g., a call from an executable into the library)
  ever be materialized, or is the library's own link unit the analysis scope?
