# VERITAS SummaryDB Ingest Adapters — Milestone Design Specification

**Status:** M11 design approved; its existing implementation plan must be
replaced before implementation. M12 details are superseded by
`m12-joern-cpg-summarydb-importer-design-spec.md`.
**Updated:** 2026-09-06
**Scope:** M11 unified LLVM IR acquisition, persisted IR artifacts, analysis
reporting, and performance qualification; shared boundary with M12 external
provider ingestion.

Architectural framing lives in `docs/architecture/01-platform-architecture.md`:

- §6 — the three-tier adapter picture.
- §7 — Tier 1: `compile_commands.json` project directory (`CodeGenIrSource`).
- §8 — Tier 2: bitcode / textual IR (`BitcodeIrSource`) with T0 / T1 / T2
  fidelity.
- §9 — Tier 3: SummaryDB provider projection + external fact ingestion.
- §10 — adapter interface contracts (`ProgramIrSource`, provider importer).
- §11 — invariant B10 in its current form.

The dedicated
[`M12 Joern CPG SummaryDB importer specification`](m12-joern-cpg-summarydb-importer-design-spec.md)
owns M12A–M12C identity, normalized graph, SummaryDB placement, external fact
batch, fusion, security, CLI, and test contracts. This document makes M11
normative and records only the boundary that M12 shares with it.

---

## 1. Decision Summary

M11 expands `veritas-build analyze` into one integrated acquisition and
analysis pipeline. It accepts three input shapes:

1. A project directory containing `compile_commands.json`.
2. A directory containing one LLVM bitcode or textual IR module per input
   translation unit.
3. One already-linked whole-program LLVM bitcode or textual IR file.

All three inputs converge at one invariant boundary:

```text
validated input
    -> canonical persisted input-unit bitcode
    -> deterministic linked project bitcode
    -> ProgramContext + ProgramIr
    -> existing local facts / SVF / summaries / CPG
    -> existing publication / WPA / FactStore
```

For project input, Clang CodeGen is parallelized with a bounded worker pool.
Both per-translation-unit bitcode and the final linked project bitcode are
persisted below the selected SummaryDB output directory. Every invocation also
keeps a structured result, timings, diagnostics, and a readable log.

The chosen design is an integrated acquisition pipeline. It does not expose a
separate mandatory `prepare` command and does not delegate normal acquisition
to an ungoverned collection of `clang` and `llvm-link` subprocesses.

## 2. Goals and Non-Goals

### 2.1 Goals

- Preserve current `--project` behavior while removing duplicate project
  resolution and manifest loading.
- Generate project input-unit bitcode concurrently without sharing mutable
  Clang or LLVM context state between workers.
- Persist canonical, reusable input-unit and linked-project bitcode.
- Accept `.bc`, `.ll`, a directory of separated modules, or one linked module.
- Run the same VERITAS-owned analysis stages regardless of the acquisition
  source.
- Produce detailed, machine-readable and human-readable run records on both
  success and failure.
- Keep semantic outputs deterministic across input enumeration and worker
  scheduling.
- Establish a reproducible performance evaluation for projects up to thousands
  of translation units and identify measurement-triggered optimizations.

### 2.2 Non-goals

- Parallelizing SVF inside one process in M11. SVF retains process-global state
  and its existing session mutex remains authoritative.
- Treating stripped IR as source-equivalent evidence.
- Replacing the SummaryDB, summary, CPG, WPA, or FactStore publication formats.
- Introducing a second analysis implementation for external bitcode.
- Supporting archives, native object files, executables, or arbitrary linker
  command lines as M11 inputs.
- Promising repository history continuity for anonymous external IR snapshots
  that carry no repository metadata.
- Implementing distributed or remote CodeGen.

## 3. Current Flow and Identified Gaps

The current CLI accepts only `analyze --project`. It resolves the project and
loads its manifest before calling `ProjectAnalyzer::AnalyzeProject`, which
resolves and loads the same information again.

The analyzer currently executes:

```text
ResolveProjectInput + LoadProjectManifest
    -> RunLocalAnalysis
       -> ProjectIrBuilder::BuildProjectIr
          -> serial in-process Clang CodeGen for every TU
          -> retain all llvm::Module objects
          -> serial link
       -> serial LocalFactExtractor
       -> local summaries
    -> serialized SVF session
    -> semantic merge
    -> Thin CPG
    -> SummaryDB / CPG publication
    -> WPA
    -> FactStore publication
```

The important gaps are:

| Gap | Effect |
| --- | --- |
| Manifest resolution and hashing occur twice | Avoidable startup and source-tree work |
| CodeGen is serial | Poor scaling on large compile databases |
| All live modules share one lifetime | Peak memory grows with project size |
| Only a compact stdout summary is emitted | Failures and performance are difficult to diagnose |
| Generated bitcode is not persisted | Repeated analysis cannot reuse acquisition work |
| The analyzer API is project-specific | External IR cannot enter at the intended M4 boundary |
| The performance fixture is small | It cannot characterize large-project bottlenecks |

## 4. Target Architecture

```text
veritas-build analyze
    -> AnalyzeCommand / AnalysisInputResolver
       |-- --project  -> CodeGenIrSource
       `-- --bitcode -> BitcodeSetIrSource or LinkedBitcodeIrSource
    -> ProgramIrAcquisition
       |-- validate and canonicalize input units
       |-- persist content-addressed input-unit bitcode
       |-- deterministically link and verify project.bc
       `-- publish immutable IR snapshot
    -> AcquiredProgramIr
       |-- ProgramContext
       |-- linked ProgramIr
       |-- fidelity and artifact index
       `-- acquisition diagnostics and timings
    -> existing downstream analysis
       |-- local facts and local summaries
       |-- SVF and semantic merge
       |-- Thin CPG
       |-- SummaryDB / CPG publication
       |-- WPA
       `-- FactStore
    -> AnalysisResult + run report
```

`ProgramIrAcquisition` is the only new route into `ProgramIr`. External IR may
skip Clang CodeGen, but it may not skip VERITAS verification, canonicalization,
identity, analysis, provenance, or publication.

## 5. CLI Contract

```text
veritas-build analyze --project <dir> \
    [--output <summarydb-dir>] [--jobs auto|N] [existing analysis options]

veritas-build analyze --bitcode <file.bc|file.ll|directory> \
    --output <summarydb-dir> [--jobs auto|N] [existing analysis options]
```

Rules:

- Exactly one of `--project` and `--bitcode` is required.
- A `--bitcode` regular file is treated as an already-linked program. A
  single-TU module is valid and follows the same path.
- A `--bitcode` directory is treated as a set of separated input modules.
  Regular direct children ending in `.bc` or `.ll` are enumerated in normalized
  relative-path order. An empty directory is rejected. Recursive discovery and
  archive expansion are deferred.
- Project input keeps its current default output of `<project>/.veritas` when
  `--output` is omitted. External IR has no safe project-relative default, so
  `--output` is required.
- The output path is resolved to an absolute normalized path before any
  artifact or run record is written.
- `--jobs` accepts `auto` or an integer greater than zero. It defaults to
  `auto`.
- `auto` resolves to
  `max(1, min(input_unit_count, hardware_concurrency, 8))`; an unavailable
  hardware concurrency value is treated as one.
- `--jobs 1` is the serial correctness and performance baseline.
- In M11, `--jobs` controls only acquisition work: project CodeGen or external
  module parse/verify/canonicalization. It does not imply concurrent SVF or WPA.
- Existing `--wpa-engine`, `--field-sensitive`, and `--max-alias-pairs`
  semantics are unchanged.

Unsupported legacy flags such as `--compile-db`, `--manifest`,
`--llvm-module`, and `--svf-input` remain rejected until separately designed.

## 6. Analysis and Acquisition Interfaces

The installed API remains free of LLVM and Clang native types.

```cpp
namespace veritas::analysis {

struct ProjectInputSpec {
  std::filesystem::path project_dir;
};

struct BitcodeInputSpec {
  enum class Kind { LinkedFile, ModuleDirectory };
  Kind kind;
  std::filesystem::path path;
};

using AnalysisInput = std::variant<ProjectInputSpec, BitcodeInputSpec>;

struct JobCount {
  enum class Mode { Auto, Explicit };
  Mode mode = Mode::Auto;
  std::size_t value = 0;
};

struct AnalysisRequest {
  AnalysisInput input;
  std::filesystem::path output_dir;
  JobCount jobs;
  AnalysisConfig config;
};

struct AnalysisResult {
  CompletionState completion;
  ProgramContext context;
  IrSnapshotDescriptor ir_snapshot;
  PublishedAnalysisIds published_ids;
  AnalysisCounts counts;
  AnalysisTimings timings;
  std::filesystem::path report_path;
};

class ProjectAnalyzer {
 public:
  StatusOr<AnalysisResult> Analyze(const AnalysisRequest& request);

  // Compatibility wrapper for existing callers.
  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request);
};

}  // namespace veritas::analysis
```

The private acquisition boundary is richer than the old `Build()` signature so
the caller cannot lose identity, fidelity, or artifact provenance:

```cpp
namespace veritas::analysis::pipeline {

struct AcquiredProgramIr {
  ProgramContext context;
  ProgramIr program_ir;
  IrSnapshotDescriptor snapshot;
  InputFidelity fidelity;
};

class ProgramIrSource {
 public:
  virtual ~ProgramIrSource() = default;
  virtual StatusOr<AcquiredProgramIr> Acquire(
      IrArtifactStore& artifacts,
      AnalysisEventSink& events) = 0;
};

class CodeGenIrSource : public ProgramIrSource { /* project acquisition */ };
class BitcodeSetIrSource : public ProgramIrSource { /* directory acquisition */ };
class LinkedBitcodeIrSource : public ProgramIrSource { /* file acquisition */ };

}  // namespace veritas::analysis::pipeline
```

`Analyze` owns orchestration and resolves project input exactly once. The
resolved manifest and `ProgramContext` are passed into `CodeGenIrSource`; no
downstream stage may rediscover or rehash the project. `AnalyzeProject` adapts
the old request into `AnalysisRequest` and delegates to `Analyze`.

## 7. Project CodeGen and Concurrency

### 7.1 Worker isolation

Clang and LLVM context state is not shared between acquisition workers. Each
worker:

1. Receives one immutable manifest translation-unit entry.
2. Creates its own Clang invocation and `llvm::LLVMContext`.
3. Generates one module using the existing normalized compile command.
4. Applies the existing identity annotations and module normalization.
5. Verifies the module.
6. Serializes canonical bitcode into the content-addressed artifact store.
7. Releases the module and its LLVM context before accepting another unit.

Workers return descriptors and diagnostics, never a live `llvm::Module`. This
is the thread-safety boundary and prevents the old all-modules-live memory
shape.

### 7.2 Scheduling and failure

`CodegenScheduler` uses a bounded pool of the resolved job count. Translation
units are assigned a stable manifest ordinal. Completion order may vary, but
all externally visible collections are sorted by that ordinal or a canonical
ID before serialization.

On the first observed worker failure, the scheduler stops assigning unscheduled
units and drains active workers. The primary diagnostic is the failure with the
lowest manifest ordinal, not the failure that happened to arrive first. All
observed failures are preserved in the run diagnostics.

### 7.3 Deterministic final link

After every input-unit artifact is available, the coordinator creates one
destination `llvm::LLVMContext` and links modules in canonical manifest order.
Each persisted module is parsed, linked, and released before the next one is
loaded. The final module is normalized and verified, the existing multiple
`main` policy is preserved, and the linked bitcode is persisted as
`project.bc`.

Parallel scheduling must not affect:

- linked project bitcode digest;
- module hash and `ProgramContext`;
- summary, projection, WPA, fact-batch, receipt, or witness IDs;
- facts, witnesses, unknowns, or diagnostic primary-error selection.

## 8. External Bitcode Acquisition and Fidelity

External `.bc` and `.ll` inputs follow the same steps after CodeGen:

1. Detect format from extension and parse into an isolated LLVM context.
2. Verify the module and reject unsupported LLVM compatibility.
3. Detect fidelity and normalize module identifiers and non-semantic paths.
4. Serialize canonical bitcode, making equivalent `.bc` and `.ll` inputs share
   the same artifact digest.
5. Link a directory input in canonical relative-path order, or accept a file as
   the already-linked program.
6. Verify and persist the linked result before constructing `ProgramIr`.

Fidelity is detected per input module and aggregated to the weakest tier:

| Tier | Meaning | Policy |
| --- | --- | --- |
| T0 — `debug_info` | Stable symbols and usable debug/source metadata | Analyze and build source anchors |
| T1 — `symbols_only` | Stable named symbols without usable source metadata | Analyze; source anchors are absent |
| T2 — `stripped` | Stable semantic symbol identity cannot be recovered | Reject the whole acquisition |

Any T2 input module rejects a module set. Mixed T0/T1 input is accepted and the
aggregate fidelity is T1 while per-module fidelity remains recorded.

Because `--bitcode` supplies no repository metadata, M11 creates an anonymous,
content-addressed external-IR snapshot context. Its semantic identity includes
the ordered canonical module digests, target triple, data layout, relevant LLVM
module flags, producer/toolchain metadata when available, fidelity, and
acquisition schema versions. Host-absolute paths and output paths are excluded.
This identity supports deterministic re-analysis of the same snapshot; it does
not claim cross-revision repository continuity for unrelated anonymous inputs.

There is one provenance-preserving exception: when the input is a
VERITAS-managed snapshot artifact, the loader discovers its adjacent
`index.json`, verifies every referenced digest and schema, and reuses the
recorded project `ProgramContext`. A linked `<snapshot>/project.bc` uses the
index in the same directory; a `<snapshot>/tus` module directory uses the index
in its parent. Unverified or mismatched sidecar metadata is rejected rather than
trusted. This round-trip is what permits a project acquisition and a later
analysis of its persisted bitcode to produce identical context-dependent IDs.

Debug paths used for T0 anchors are normalized independently from semantic
context identity. Raw host paths may appear only in sanitized diagnostics when
needed to identify an invalid user-supplied file.

## 9. IR Artifact Store and Cache

The selected SummaryDB directory has the following additional layout:

```text
<output>/
  manifest.json
  metadata.db
  objects/
  wpa-component-results/
  llvm/
    objects/
      <sha256>.bc
    snapshots/
      <acquisition-id>/
        tus/
          <input-unit-id>.bc
        project.bc
        index.json
    current.json
  runs/
    <invocation-id>/
      request.json
      result.json
      timings.json
      diagnostics.jsonl
      analysis.log
    latest.json
```

`llvm/objects` is an immutable content-addressed store. Snapshot entries use
hard links when supported and fall back to copies without changing identity.
An external module is represented as a synthetic input unit, so the `tus/`
layout remains common to all acquisition sources. For a linked-file input,
`project.bc` and its one synthetic input unit may reference the same object.
Concurrent object insertion uses temporary files in the destination filesystem
and atomic put-if-absent publication. A racing writer must verify that an
existing object has the expected digest before treating the insertion as a
cache hit.

For project input, root `manifest.json` retains the existing M1 analysis
manifest. For anonymous external input, it is a schema-discriminated external
IR manifest containing the normalized module-set and derived snapshot context;
consumers must inspect the manifest kind rather than assume a compile database.

`index.json` records, in canonical order:

- acquisition and schema versions;
- input kind and aggregate fidelity;
- resolved job count;
- every input-unit ID, canonical logical name, object digest, fidelity, cache
  state, and diagnostic status;
- linked project object digest, module hash, target triple, and data layout;
- the `ProgramContext` and toolchain identity.

`llvm/current.json` is a portable pointer document, not a symlink. It is
atomically replaced only after all input units and the linked module have been
generated or loaded, verified, persisted, and indexed. A later downstream
analysis failure does not corrupt that completed acquisition snapshot.

### 9.1 Safe project input-unit cache key

A project input-unit cache key contains:

```text
normalized semantic compile command
+ primary source content digest
+ transitive include-closure content digest
+ target and toolchain identity
+ Clang/LLVM CodeGen settings
+ identity annotation and acquisition schema versions
```

Timestamps are not semantic inputs. The current empty `preprocessor_hash`
placeholder is insufficient; implementation must populate the include-closure
digest using Clang dependency scanning or an equivalent compiler-owned
dependency computation. Exact cache hits skip CodeGen and reuse the immutable
bitcode object. A missing dependency digest is a cache miss, never permission to
reuse potentially stale IR.

## 10. Run Reporting and Logs

The run directory is created as soon as the output root and invocation ID are
known. `AnalysisEventSink` receives typed events from acquisition and every
downstream boundary. One sink writes chronological JSON Lines to
`diagnostics.jsonl`; another renders the same event stream to `analysis.log`.

`result.json` uses schema `analysis-result.v1` and records:

- completion state and the last successfully completed boundary;
- input kind, fidelity, context, module, projection, WPA run, fact batch, and
  publication receipt IDs when produced;
- snapshot, linked bitcode, and per-input-unit artifact references;
- per-input-unit cache state and diagnostics;
- function, summary, component, CPG node/edge, fact, witness, and explicit
  unknown counts;
- expected, completed, failed, and reused WPA component counts;
- stage timings, resolved concurrency, CPU time, and peak resident memory when
  supported;
- stable error codes and diagnostic references.

JSON objects use canonical key order and semantic collections use stable ID or
manifest order. Operational fields such as invocation ID and elapsed time are
expected to differ between runs; semantic IDs and collections must not.

`request.json` records the normalized request without secrets or redundant
source contents. `timings.json` contains stage and per-input-unit measurements.
`runs/latest.json` is atomically updated after the final success or failure
record is closed. The CLI keeps stdout compact and prints the completion state,
report path, linked bitcode path when available, and principal published IDs.

Normal logs must not copy source contents, environment values, or unrestricted
compiler arguments. Diagnostics may show a sanitized argument subset and a
normalized path needed to identify the failing input.

## 11. Failure and Publication Semantics

| Failure | Policy |
| --- | --- |
| Invalid CLI input combination | Usage error; no acquisition snapshot |
| Output directory cannot be created | Error on stderr; no run directory can be promised |
| Project manifest failure | Failed run report; no acquisition snapshot |
| CodeGen worker failure | Stop scheduling, drain active workers, deterministic primary error |
| Bitcode parse/verify error | `InvalidArgument` with offending logical input; no snapshot advancement |
| LLVM incompatibility | `FailedPrecondition`; never silently downgrade |
| Any T2 stripped module | `FailedPrecondition` with rebuild guidance |
| Duplicate/incompatible symbol during link | Fatal acquisition error; no snapshot advancement |
| Linked module verification failure | Fatal acquisition error; no snapshot advancement |
| Downstream analysis failure | Preserve completed IR snapshot and failed run report |
| Existing SummaryDB/CPG publication failure | Preserve current atomic publication semantics |
| WPA or FactStore failure | Record exact boundary and IDs; do not describe the run as complete |

Immutable input-unit objects produced before an acquisition failure may remain
in `llvm/objects` and may be reused by a later exact cache hit. Partial snapshot
directories are never selected by `llvm/current.json`. M11 does not pretend
that SummaryDB, CPG, WPA, and FactStore share one database transaction; the run
report makes each publication boundary explicit.

## 12. Performance Design and Evaluation

### 12.1 First-line changes included in M11

1. Resolve and hash a project manifest once.
2. Use bounded, isolated acquisition workers.
3. Persist one module and release its context before generating the next.
4. Reuse exact input-unit cache hits.
5. Parse and link persisted modules one at a time in deterministic order.
6. Instrument acquisition, linking, local extraction, SVF, projection,
   publication, WPA, and FactStore separately before claiming a speedup.

These changes target the currently visible redundant work, serial CodeGen, and
module-lifetime memory costs without weakening determinism or SVF isolation.

### 12.2 Evaluation matrix

The benchmark corpus has three scales:

- Small: existing deterministic fixtures for correctness and overhead.
- Medium: a representative 100–500 input-unit C/C++ project.
- Large: a representative 1,000–5,000 input-unit single-program target.

Each applicable project is measured in these scenarios:

| Scenario | Purpose |
| --- | --- |
| Cold project, `--jobs 1` | Serial baseline |
| Cold project, `--jobs auto` | Parallel CodeGen scaling |
| Warm project, `--jobs auto` | Input-unit cache effectiveness |
| Linked whole-project bitcode | Downstream analysis cost without CodeGen/link-set acquisition |
| Directory of input-unit bitcode | Parse/verify/link-set acquisition cost |

Measurements include wall time, CPU time, peak RSS, input units per second,
cache hit/miss counts, artifact size, linked module size, SVF graph size,
alias-budget utilization, CPG size, WPA component counts, and WPA reuse.
Repeated timing samples report median and dispersion; correctness is checked on
every sample.

Every configuration for the same semantic input must produce identical linked
bitcode digest, summaries, projection IDs, facts, witnesses, and explicit
unknowns. Context-dependent identity equality between project and bitcode modes
uses the verified managed-snapshot round-trip described in §8; arbitrary
external IR uses its own anonymous context. CI uses a generated bounded fixture
for determinism, failure, and cache tests. Large-project timing results are
published as benchmark reports, not flaky pass/fail gates, until stable
environment-specific thresholds are established.

### 12.3 Measurement-triggered follow-up optimizations

These are explicitly deferred until profiles show that their stage dominates:

- Parallel function-local extraction after its shared structures become
  immutable or thread-safe.
- A normalized SVF-result cache keyed by linked module digest, SVF config,
  toolchain identity, and fact schema.
- Concurrent WPA execution across independent SCC DAG levels or components.
- Balanced-tree linking in isolated contexts if serial incremental linking
  dominates.
- Batched SummaryDB or FactStore writes if storage profiles dominate.

SVF's process-wide mutex must not be removed merely to improve a benchmark.
Any SVF concurrency design requires a separate proof of global-state isolation,
typically process isolation or an upstream architectural change.

## 13. Verification Strategy

### 13.1 CLI and input resolution

- Project-only, bitcode-file-only, and bitcode-directory-only requests succeed.
- Missing input or mixed `--project`/`--bitcode` requests fail with usage errors.
- External input without `--output` fails clearly.
- `--jobs auto`, `--jobs 1`, valid `N`, zero, negative, and malformed values
  have specified behavior.

### 13.2 Acquisition and determinism

- The same multi-TU project produces identical artifacts and semantic outputs
  with `--jobs 1`, `--jobs 2`, and `--jobs auto`.
- Intentionally reordered worker completion does not change serialization.
- Each worker owns an isolated LLVM context; concurrency stress tests run the
  acquisition repeatedly.
- A worker failure cancels unscheduled work, drains active work, and selects the
  lowest-ordinal primary error.
- `current.json` never selects a partial or unverified snapshot.

### 13.3 Cache behavior

- Exact repeat runs reuse every unchanged input-unit object.
- Source, included header, compile command, target, toolchain, or schema changes
  invalidate the affected key.
- Missing dependency information forces regeneration.
- An interrupted run may reuse completed immutable objects without treating its
  partial snapshot as current.

### 13.4 External IR

- `.bc` with debug info produces T0 source anchors.
- Equivalent `.ll` and `.bc` produce the same canonical bitcode digest and
  semantic result.
- A directory of modules links every function exactly once.
- A linked module skips set linking but still verifies, canonicalizes, persists,
  and runs the complete downstream analysis.
- Re-analyzing a managed project `project.bc` or `tus/` directory verifies its
  sidecar and preserves the original `ProgramContext` and dependent IDs.
- Arbitrary external IR without verified sidecar metadata receives an anonymous
  content-addressed context.
- T1 succeeds without source anchors; T2 fails clearly.
- Mixed T0/T1 becomes aggregate T1 with per-module fidelity retained.
- Malformed, incompatible, duplicate-symbol, and linked-verification failures
  advance no snapshot.

### 13.5 Reporting and compatibility

- Successful and failed invocations both retain complete run records when the
  output directory is writable.
- Result arrays and diagnostics use canonical ordering.
- Logs redact environment values, source contents, and non-allowlisted compiler
  arguments.
- Existing `AnalyzeProject` callers and current project CLI behavior remain
  compatible.
- Installed public headers expose no LLVM, Clang, Joern, or PhASAR native types.

## 14. Delivery Boundaries

Implementation is divided into four reviewable stages:

1. **Input-neutral orchestration and reporting:** introduce `AnalysisRequest`,
   `Analyze`, event sinks, run reports, and single manifest ownership while
   retaining serial CodeGen.
2. **IR artifact baseline:** persist canonical input-unit and linked bitcode,
   add immutable snapshots, and establish serial determinism and failure tests.
3. **Parallel project acquisition:** add dependency-complete cache keys,
   isolated CodeGen workers, bounded scheduling, and cold/warm performance
   measurements.
4. **External IR and qualification:** add directory/linked bitcode sources,
   fidelity enforcement, cross-input equivalence tests, the large-project
   benchmark report, and user documentation.

The replacement implementation plan must name exact files, tests, commands,
and commits for these stages. It must supersede rather than append to
`docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md`, whose
current scope covers only the narrower external-IR adapter.

Documentation work in the implementation includes:

1. Reconcile the historical project-only wording in the M1 and M4 milestone
   specifications while retaining it as the behavior of those completed
   milestones.
2. Update `docs/architecture/01-platform-architecture.md` with acquisition,
   artifact, and reporting ownership.
3. Update the engineering-backbone B10 wording only if the new detailed design
   exposes a remaining contradiction.
4. Update `docs/guides/tutorial-build-summarydb-analysis-tool.md`, the SummaryDB
   generation guide, CLI help, and relevant README examples.
5. Add a reproducible benchmark procedure and report format.

## 15. M11 Acceptance Criteria

M11 is complete only when:

- `veritas-build analyze --project` persists per-TU and linked bitcode below the
  chosen output and supports deterministic bounded parallel CodeGen.
- `veritas-build analyze --bitcode` accepts a separated module directory or one
  linked `.bc`/`.ll` program and runs the same downstream VERITAS analysis.
- T0/T1/T2 fidelity policy is enforced and recorded.
- Success and failure reports identify every reached acquisition, analysis, and
  publication boundary.
- Exact cache reuse is dependency-complete and never timestamp-only.
- Serial, parallel, `.bc`, and `.ll` equivalent inputs yield identical semantic
  output.
- The performance matrix is executed on small, medium, and large inputs, and
  measured bottlenecks and recommendations are recorded.
- No LLVM native identity or unredacted host-specific input leaks into stable
  IDs, public APIs, or normal logs.

## 16. External Provider Ingestion Boundary (M12)

```text
Joern GraphSON / GraphML
    -> bounded provider reader
    -> RawProviderGraph
    -> schema/context validation
    -> identity resolution + semantic normalization
    -> ProviderProgramGraph + ExternalFactBatch
    -> atomic SummaryDB provider publication
```

M12 provider ingestion does not enter through `ProgramIrSource` and does not
mutate M11 IR snapshots. The imported projection is stored across SummaryDB's
Object, Metadata, Graph, Fact/Provenance, Evidence Cache, and History layers.
It does not mutate the M6 `ThinCpg`, native summary bindings, or WPA inputs.
Provider and native observations share a semantic query view while retaining
separate authority, epistemic state, capabilities, assumptions, and witnesses.

M12 adds `ExternalFactBatch`; it does not publish raw vectors of stringly
`ExternalFact` records. Joern ordinals remain provenance only and unresolved
subjects receive canonical hashed `ExternalEntityID` values. PhASAR remains an
independently designed M12D adapter and does not share Joern's graph parser API.

## 17. Milestone Placement and References

- **M11 — Unified IR acquisition and external IR adapter** depends on M4 and
  M5. It reuses existing local extraction and all downstream analysis.
- **M12A — SummaryDB external-provider substrate** depends on M2/M3/M6/M9.
- **M12B — Joern GraphSON/GraphML importer** depends on M12A.
- **M12C — Provider fusion and Evidence integration** depends on M10B/M12B.
- **M12D — PhASAR result adapter** depends on M12A and requires a separate
  detailed design.

Relevant documents:

1. `docs/architecture/01-platform-architecture.md` — adapter tiers and B10.
2. `docs/specs/veritas-engineering-backbone-design-specification.md` — global
   identity, provenance, and adapter invariants.
3. `docs/specs/link-unit-program-boundary-design-spec.md` — definition of the
   analyzed link unit and program boundary.
4. `docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md`
   — existing project manifest and `ProgramContext`.
5. `docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md` —
   CodeGen, linking, `ProgramIr`, and local extraction reused by M11.
6. `docs/specs/milestones/m05-required-svf-analysis-design-spec.md` — required
   downstream SVF analysis.
7. `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md` — unchanged
   native CPG projection boundary.
8. `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md` —
   canonical M12A–M12C design and acceptance contract.
