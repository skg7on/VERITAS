# `veritas-build analyze` Phase Observability Design Specification

**Status:** Draft for review

**Depends on:**
[`veritas-build-analyze-round3-performance-design-spec.md`](veritas-build-analyze-round3-performance-design-spec.md)
for the measurement method this design replaces and for the failure mode it
exists to prevent. Section 9.6 of that document is the motivating evidence:
four root-cause magnitudes were refuted on measurement, and every one of them
was wrong in the same direction — a cost was read out of the code, multiplied
by a component count, and recorded as a root cause without ever being timed.

**Tracking issue:** not yet filed.

**Motivating command:**

```bash
./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb
```

## 1. Purpose

Three performance rounds measured this command entirely from outside the
process: `/usr/bin/time -lp` for wall and peak, macOS `sample` for stack trees,
`vmmap` for the resident-versus-footprint distinction, and a hand-built SQL
digest instrument for content identity. That method has three recorded costs:

1. **Phase attribution is coarse.** Round 3 section 2.2 records phase boundaries
   carrying "roughly ±15 s of uncertainty at a 30 s sampling interval" on a
   583 s run — and the phase boundaries are exactly what the round's conclusions
   rest on.
2. **The profile names hot functions but cannot say what they belong to.** The
   same section names `MakeStableId`, `ComputeSHA256`, `StableId::operator<=>`,
   and `guarded_pwrite_np` as reproducible hot leaves, then states the phase
   boundaries are uncertain. Naming a leaf does not attribute it to a milestone.
3. **The memory story needed two tools and was still read wrong once.** Section
   2.3's headline finding is that the peak is late, at 95 % of wall time inside
   publication, contradicting round 2's claim that it occurs at 60 %. Round 3
   also records misreading a `vmmap` `resident` column where `dirty` was meant
   (section 3.6).

This design puts a span recorder inside the pipeline so that per-phase wall
time, self time, process CPU time, resident set, and physical footprint become
*fields in an artifact* instead of arguments reconstructed from stack samples.
It adds observability only. It changes no analysis semantics, and section 5
makes that structural rather than a promise.

This is **not** a performance round. It sets no wall-time or memory target. Its
deliverable is the instrument that makes the next round's targets measurable —
which is the one instruction round 3 identified as its most transferable
result.

## 2. Scope decisions

Five decisions were taken during design. They are recorded here with the
alternatives they displaced, because each alternative is reasonable on its face
and will be proposed again.

| Decision | Chosen | Displaced |
| --- | --- | --- |
| Consumer | Report plus a versioned, diffable artifact under `--output` | A run-metrics table in the SummaryDB; a CI regression gate; stdout only |
| Granularity | Top-level phases, named sub-spans, per-component aggregates | Flat top-level rows only; run-scope aggregates without per-component statistics |
| Memory | Per-phase resident and footprint delta, in-phase peak, and the raw curve | End-of-run peak only; sampling without a series |
| Inventory | Full project-outline inventory including a post-publication store read-back | In-memory counts only; inventory without the store read-back |
| Wiring | An explicitly threaded recorder | A thread-local ambient recorder; stage-boundary observer callbacks |

**No persistence and no gate.** The artifact lands in `--output`; nothing reads
it back. A run-metrics table was displaced because it would force an answer to
what a non-deterministic measurement means inside a content-addressed store, and
a CI gate was displaced by its own known flakiness budget: section 10.4 of the
round-3 spec records a 0.72 GiB run-to-run spread on this fixture before any
consideration of shared-runner noise.

**Threading over ambient state.** The thread-local recorder would have needed no
signature changes at all, and was rejected in favour of explicit plumbing on
three grounds: it is an idiom outlier in a codebase built on explicitness
(`Status`/`StatusOr` throughout, no exceptions, immutable IRs); a caller-owned
recorder still yields a usable partial timeline when a run dies mid-publication,
which result-carried metrics would not; and `SvfBudget` already establishes the
injected-dependency pattern this follows (`SvfBudget.h:62`, `Now now =
steady_clock::now`).

**Observer callbacks were disqualified, not merely deprioritized.** An
`AnalysisObserver` at stage boundaries cannot see inside the 13,716-iteration
component loop, so it cannot deliver per-component aggregates. Listing it here
records that it loses on scope, not on taste.

## 3. Non-goals

1. **No performance target.** No wall-time, CPU, or memory objective is set,
   met, or claimed. The overhead budget in section 7.3 is a *ceiling on the
   instrument*, not a performance result.
2. **No new analysis content.** No fact, witness, summary, or provenance row
   changes. Section 8.2 makes this an executable assertion.
3. **No fix for the duplicated ingest** that section 10 records. It is measured,
   not repaired.
4. **No live progress UI.** The report is emitted after the run. Interval
   sampling makes a progress display possible later; it is not designed here.
5. **No cross-run baseline, trend store, or comparison tool.** Diffing two
   artifacts is left to `jq` or an ad-hoc script, and the diffability rules in
   section 6.5 exist so that such a script can be written without fighting the
   format.
6. **No metrics for `veritas-query` or `veritas-explain`.** This design covers
   `veritas-build analyze` only.

## 4. Instrument model

### 4.1 Placement

**The recorder lives in `veritas_core`** (`include/veritas/core/RunMetrics.h`,
`src/core/RunMetrics.cpp`) and depends only on the standard library and POSIX.
The report model, its two renderers, and the store read-back live in a new
`observability` subtree (`include/veritas/observability/`,
`src/observability/`).

This split is forced by library dependencies, not taste. `veritas_wpa` must
record spans, and `veritas_facts` already includes `veritas/wpa/WpaOrchestrator.h`,
so `veritas_facts` depends on `veritas_wpa`. A recorder placed in a library that
also depends on `veritas_facts` — which the store read-back must — would close a
cycle (`wpa → observability → facts → wpa`). Putting the dependency-free
recorder in `veritas_core` instead gives `wpa → core` with no cycle, and leaves
`observability` a leaf consumer that only the CLI links.

`veritas_core` therefore gains its first POSIX dependency (`Threads::Threads`,
section 7.4) but stays free of store and LLVM knowledge. The LLVM dependency
needed for JSON rendering stays in `observability`, so a `veritas_core`-only
consumer — and every `veritas_core` test — links no LLVM.

### 4.2 Recorder and spans

`core::RunMetrics` is created by the caller — the CLI for a real run,
a test for a unit test — and passed by pointer. It is the *only* place a
measurement is accumulated.

A span is an RAII guard over an injected clock:

```cpp
core::PhaseSpan span(metrics, "wpa.component.execute");
span.SetLabel(component_kind + "/" + core::ToString(scc_id));  // top-N attribution
span.AddCounter("wpa.components.reused", reused_count, "count");
```

Opening pushes onto a span stack; closing pops and folds into the accumulator
for that dotted name. Nesting is by name path, so `wpa.component.execute` under
`wpa.orchestrate` is a single node with `count = 13716`, not 13,716 nodes.

Each accumulator records:

| Field | Meaning |
| --- | --- |
| `count` | occurrences |
| `wall_inclusive` | total wall inside the span, children included |
| `wall_self` | `wall_inclusive` minus direct children — the number that answers "where did the time go" |
| `cpu_inclusive` | process CPU delta across the span; interval-bearing spans only (section 4.5) |
| `min`, `max` | over `wall_inclusive` |

**Clocks are injected.** `Now` (default `steady_clock::now`) and `CpuNow`
(default a `CLOCK_PROCESS_CPUTIME_ID` / `getrusage` reader) are constructor
parameters. Tests supply a scripted sequence and assert exact integers; no test
sleeps.

**One honest caveat about `cpu_inclusive`.** A process CPU delta counts every
thread. VERITAS is single-threaded (section 4.7), so today it is the span's own
work; if concurrency is ever added, this field becomes "process CPU consumed
during the span" and must not be read as the span's own cost. The field is
nonetheless the right instrument for the question round 3 could not answer from
outside: 421.67 s CPU against 572.82 s wall, with the gap unattributed.

### 4.3 Distributed spans

The five per-component spans retain every sample and report exact percentiles.
Worst case is 13,716 components × 5 spans ≈ 68,580 samples ≈ 550 KB, held for
the duration of the run and **dropped once the report is built** — the artifact
carries percentiles, never samples.

- **Percentile definition:** nearest-rank on the sorted sample vector,
  `p_k = samples[ceil(k/100 × n) - 1]`. Fixed here because two implementations
  disagreeing on the boundary would make two artifacts differ for a reason that
  is not a measurement.
- **Top-N:** the K slowest occurrences (default `K = 10`) ordered by
  `(wall_inclusive desc, label asc)`. The explicit label tiebreak makes the
  artifact reproducible when durations collide, which matters because the
  artifact's purpose is byte-diffing.
- **Cap:** a per-span sample cap with an explicit `samples_truncated` flag. A
  truncated distribution is reported as truncated, never as complete.

### 4.4 Counters

`(name, value, unit)` triples, attached to a span or to the run root. Units are
strings rather than an enum, so a new instrument needs no schema change, and
counters are rendered sorted by name so that instrumentation order cannot leak
into the artifact.

### 4.5 The memory sampler and the interval join

A sampler records `(t_since_run_start, rss_bytes, footprint_bytes)` —
`task_vm_info.phys_footprint` on Darwin, resident set elsewhere. Physical
footprint is recorded alongside the resident set because round 3's central
memory argument turned on the difference: 1.42–1.45 GiB of what `/usr/bin/time
-lp` reports as peak is memory the kernel may already have reclaimed (section
2.3), and no external tool made that distinction legible.

**The sampler knows nothing about spans.** Spans record their `[start, end]`
interval; the *report* joins the series to the intervals, producing per-span
`rss_start`, `rss_end`, `delta`, and `peak_within`. Post-hoc joining is what
makes nested spans get correct peaks, and it keeps the sampler independent and
the join deterministic.

Two details this fixes rather than leaves ambiguous:

- **Boundary rule: start-inclusive and end-inclusive.** A sample exactly at
  `t_start` belongs to the span, and so does one exactly at `t_end`. Both cases
  are tested (section 8.1). An earlier draft made the end exclusive, so that a
  boundary sample would fall only to the following sibling; that reading has no
  analytical consequence here, because the report publishes per-span peaks and
  deltas rather than a partition of the series, and it cannot be stated cleanly
  for nested spans — a child's `t_end` lies inside its parent's window, so the
  parent includes the sample either way.

- **When the `memory` block is present at all — and this rule applies at both
  levels.** A *span's* block is present only when at least one sample falls
  inside its window; the *run-level* `memory` block, including its `peak`, is
  present only when the series holds at least one sample. With no series — which
  is what `--metrics-interval-ms 0` produces, and the state of the library until
  the CLI wires a sampler — or with a window that no sample lands in, the block
  is **absent**, not zeroed. An all-zero block is indistinguishable from a
  measured zero, and the artifact's contract everywhere else is that an absent
  measurement is visibly absent rather than plausibly zero; the memory column
  exists precisely to tell a small phase apart from no measurement at all. A
  present block therefore means "measured", and its figures are real.

  The two levels are stated together because an earlier revision of this rule
  was span-scoped, and the run level then kept emitting a zeroed `peak` for
  exactly the runs the rule was written to protect — the same reading, one level
  up. Stating a rule for one level of a nested structure is how the sibling
  survives.
- **Which spans keep intervals:** only interval-bearing spans (tens, not
  13,716) and the retained top-N entries. A distributed span does not retain
  68,580 intervals.

**Series bounds.** The buffer is pre-allocated at 65,536 samples (≈1.5 MiB) so
the sampler thread never allocates and the analysis path never locks. On
overflow the sampler does not ring-buffer — a ring discards the *early* curve,
which is where the SVF burst lives. It drops every other sample, doubles
`series_decimation`, and continues, keeping the whole timeline at coarser
resolution. The decimation factor is written into the artifact.

### 4.6 The span inventory

Every site below was read from the source. `[D]` marks a distributed span.

| Span | Site |
| --- | --- |
| `run` | root, opened by the CLI |
| `cli.ingest` | `src/tools/veritas-build.cpp:211-220` — resolve, load manifest, write `manifest.json` |
| `m1.ingest` | `src/analysis/ProjectAnalyzer.cpp:362-367` — the analyzer re-resolves and re-loads |
| `m4.local_analysis` | `:370` `RunLocalAnalysis` |
| `m5.svf` | `:380` `SvfAnalysisStage::Analyze` |
| `m5.svf.module_set` | `src/analysis/svf/SvfSession.cpp:103` `buildSVFModule` |
| `m5.svf.svfi` | `:112-113` `SVFIRBuilder::build` |
| `m5.svf.andersen` | `:120-121` `createAndersenWaveDiff` |
| `m5.svf.svfg` | `:128-129` `buildFullSVFG` |
| `m5.svf.map_facts` | `:136` the callback into `MapSvfFacts` |
| `m5.model_bundle_load` | `ProjectAnalyzer.cpp:388-392` |
| `m5.merge_svf_facts` | `:395-396` `MergeSvfFactsV2` |
| `m6.cpg_projection` | `:409-414` `BuildThinCpg` |
| `m2m3.publish_summaries` | `:424-437` `PersistManifestContext` + `Publish` |
| `wpa.orchestrate` | `src/wpa/WpaOrchestrator.cpp` `Run` |
| `wpa.graph_build` | `:150-170` call/SCC graph and the frozen expected set |
| `wpa.component.materialize` `[D]` | `:176-194` `WpaInputMaterializer::Build`, plus root collection `:203-210` |
| `wpa.component.cache_lookup` `[D]` | `:212-214` `LoadReusableComponent` |
| `wpa.component.execute` `[D]` | `:227` `executor_.Execute` |
| `wpa.component.canonicalize` `[D]` | `:234-238` `ResultCanonicalizer::Canonicalize` |
| `wpa.scc_state_flush` | `:292` `SccStateRepository::FlushStateCache` |
| `facts.batch_assemble` | `ProjectAnalyzer.cpp:332` `MakeAnalysisFactBatch` |
| `facts.store_open` | `:333-336` `FactStore::Open` |
| `facts.publish` | `:337-342` `AnalysisFactBus::Publish` |
| `facts.publish.validate` | `AnalysisFactBus::Validate` |
| `facts.publish.sink.fact-store` | `FactStore::Publish` |

**A note on `m5.svf.andersen`.** Round 3's section 2.2 cites "SVF self-reports
20.30 s Andersen and 32.61 s MemorySSA". Those are SVF's own figures. No SVF
timing is currently captured into a VERITAS type — `SvfAnalysisStage.h` has no
timing field — so the span wraps the call at `SvfSession.cpp:120-121` and
measures it directly. The two numbers should agree in magnitude and may not
agree exactly; where they diverge, this span is the measured one.

### 4.7 The store read-back

After publication the inventory collects, per published table, a row count; per
store file, a byte size; and a **cross-check** of counts the pipeline also holds
in memory — components expected versus components in the store, facts published
versus fact rows. Round 3 had to hand-roll exactly this to confirm the component
count "independently of the tool's own summary output" (section 2.4), and an
independent check is worth more than a self-report.

This block is the newest coupling in the design: it depends on store internals
and therefore on schema versions. Section 7.1 makes it degrade alone.

## 5. Identity and determinism boundary

### 5.1 The surfaces that must not move

| Surface | Derived at | Feeds |
| --- | --- | --- |
| `run_id` | `ProjectAnalyzer.cpp:192-221` `MakeAnalysisRun` | every fact, witness, and provenance row |
| `svf_configuration_hash` | `:99-101` `HashString(ToSvfConfig(config).CanonicalAnalyzerConfig())` | `run_id` |
| `wpa_configuration_hash` | `:106-122` explicit length-prefixed field list | `run_id` |
| `engine_toolchain_identity` | `:204-220`, Soufflé provenance digest | `run_id`; binds the linked binaries |
| `batch_id` | `AnalysisFactBus.h:70` `DeriveBatchId` | fact publication |
| `projection_id` | `CpgCanonicalizer::ProjectionId` | CPG identity |
| `ExternalHash`, `FixpointHash`, `SccConvergenceHash` | component derivation | incrementality and cache reuse |
| summary ids, `witness_id`, `producer_id`, `source_anchor_id` | provenance derivation | explanation |

### 5.2 Rules

**Rule 1 — metrics options do not live in `AnalysisConfig`.** They travel in a
separate `core::RunMetricsOptions`. This is structural, not stylistic.
`svf_configuration_hash` hashes *whatever `ToSvfConfig` emits*, and `ToSvfConfig`
(`:82-91`) is a designated-initializer copy of five `AnalysisConfig` fields.
Adding a metrics field to `AnalysisConfig` plus one line in `ToSvfConfig` would
move the SVF configuration hash, hence `run_id`, hence every digest chained off
the run — with no compile error, and with no test that notices, because content
identity is only compared across builds by a manual instrument. Disjoint types
make this impossible rather than merely discouraged. This is the same failure
class as the recorded trap that editing `SemanticKeyCodec.cpp` moves both run
and batch ids.

**Rule 2 — the recorder is a parameter, never a field of a hashed struct.**
`WpaRunRequest` gains `core::RunMetrics* metrics = nullptr`. That
request is not hashed today — `wpa_configuration_hash` reads `AnalysisConfig` —
and this design keeps it so. The guard is executable: section 8.2.

**Rule 3 — the artifact is write-only.** It is written by the CLI *after*
`AnalyzeProject` returns, and `observability` ships no analysis-side reader. No
analysis code path opens it, so "the metrics changed the result" is not a thing
that can happen quietly.

**Rule 4 — spanning does not alter control flow.** A span guard cannot swallow
or reorder a `Status`; its destructor folds numbers only, with no I/O, no
allocation, and no error path. It folds on the error return path deliberately:
a run that dies during publication is when the partial timeline is worth most.
With `metrics == nullptr` the guard is one pointer test and never reads a clock.

**Rule 5 — in-run timing may not feed decisions.** No span duration may drive a
budget, a timeout, or a retry. `SvfBudget` holds a soft time budget that does
change behaviour; sharing one clock between it and the recorder would make the
artifact an input to semantics and collapse Rule 1. They stay separate:
`SvfBudget` keeps its clock, `RunMetrics` is output-only.

## 6. Report and artifact

### 6.1 Two outputs, one source

1. A human-readable phase report appended to stdout after the existing
   `Analysis complete` block.
2. `<output>/run-metrics.json`, sibling to `manifest.json`.

**Why not extend `manifest.json`.** `VeritasBuildAnalyzeCliTest.WritesDeterministicDiagnosticManifest`
(`tests/integration/build/VeritasBuildAnalyzeCliTest.cpp:142-169`) asserts that
`manifest.json` is **byte-identical** across two runs of the same fixture in
different directories. Timings can never satisfy that, and the manifest has a
`ToCanonicalBytes` path used for hashing that a non-semantic measurement must
not be reachable from. The test reads only `manifest.json` and no test
enumerates the output directory, so a sibling file is purely additive.

### 6.2 CLI flags

The existing `--flag value` idiom is mandatory, not stylistic: `take_value`
(`veritas-build.cpp`) rejects any value beginning with `-`, so `--no-metrics`
would be parsed as an unknown argument. `--metrics false` is the only
well-formed spelling.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--metrics true\|false` | `true` | record and report; `false` restores pre-change behaviour exactly, with no thread |
| `--metrics-interval-ms N` | `250` | sampler period; `0` disables the series entirely — no sampler thread and no samples |
| `--metrics-top-n K` | `10` | slowest occurrences retained per distributed span |
| `--metrics-series true\|false` | `true` | emit the memory series array. It suppresses **only** `memory.series`; `memory.peak` and `series_decimation` remain, because the peak is the report's headline memory figure and suppressing the curve is not a reason to withhold it |
| `--metrics-path <path>` | `<output>/run-metrics.json` | override the artifact location |

The library default stays `metrics == nullptr`: no thread, no clock reads, no
behaviour change for any caller that does not ask for metrics. That covers most
of the 811-test suite, which calls `AnalyzeProject` directly. It does **not**
cover the tests that drive the CLI binary: those run with metrics on by default,
gain a `run-metrics.json` and a report block, and take the appended stdout of
section 8.4 as their updated expectation. The two stages that carry this change
are named in section 9.2. The sampler thread exists only when recording is on.

### 6.3 Schema

```json
{
  "schema": "veritas.run-metrics.v1",
  "complete": true,
  "diagnostics": [],
  "identity":    { "run_id": "", "batch_id": "", "repository_id": "",
                   "revision_id": "", "build_variant_id": "", "projection_id": "",
                   "svf_config_hash": "", "wpa_config_hash": "",
                   "engine_toolchain_identity": "" },
  "environment": { "os": "", "arch": "", "cpu_model": "", "cores": 0,
                   "ram_bytes": 0, "build_type": "", "host_compiler": "",
                   "veritas_version": "", "git_revision": "" },
  "config":      { "metrics": {}, "conformance_oracle": false },
  "inventory":   { "input":  { "translation_units": 0, "compiler_id": "",
                               "compiler_version": "", "target_triple": "",
                               "source_tree_hash": "", "include_closure_hash": "" },
                   "output": { "summaries_published": 0, "unknowns_by_reason": {},
                               "cpg_nodes": 0, "cpg_edges": 0,
                               "svfg_nodes": 0, "svfg_edges": 0,
                               "components_by_kind": {}, "rooted_input_facts": 0,
                               "canonical_facts": 0 },
                   "incrementality": { "components_reused": 0,
                                       "components_executed": 0,
                                       "summaries_recomputed": 0,
                                       "summaries_reused": 0 } },
  "store":       { "tables": [], "bytes": {}, "cross_checks": [] },
  "phases":      [],
  "memory":      { "peak": {}, "series": [], "series_decimation": 1 },
  "counters":    []
}
```

`identity` is the block that moves run to run, and it is separated precisely so
that a comparison script can ignore it.

**Three inventory fields have no source and are reported as not recorded, not as
zero.** `inventory.output.svfg_edges`, `inventory.incrementality.
summaries_recomputed` and `...summaries_reused` are fields the design wants and
no current producer can fill:

- `svfg_edges` is the sharpest case, because a source *appeared* to exist.
  `SVFG::getTotalEdgeNum()` is reachable through `SVFG → VFG → GenericGraph`, but
  it reads `GenericGraph::edgeNum`, and nothing increments that field for a
  VFG/SVFG: `incEdgeNum()` is called only from `lib/Graphs/CDG.cpp` and
  `lib/SVFIR/SVFStatements.cpp`, while `VFG::addVFGEdge` maintains only the
  endpoint nodes' edge lists. It returns 0 for every SVFG ever built. An earlier
  revision of this design instructed the SVF session to emit it, which would have
  put a structurally-zero field into the artifact in place of a measurement —
  a falsehood that is indistinguishable from a measured zero, and therefore
  precisely what this design exists to prevent.
- `summaries_recomputed` and `summaries_reused` have no producer at all.

All three are reported with a `metrics note:` line rather than a zero or an
invented count. The general rule they illustrate: **verify that a source carries
the value, not merely that it exists.** Checking that an accessor is reachable
is not checking that anything writes what it reads.

**Three things in the shape above are conditional, and the example shows their
populated form only.** Every empty string shown is a *shape*, not a value the
artifact may contain.

1. `memory` is absent entirely when nothing was measured — see section 4.5's
   presence rule — so `"peak": {}` above means "present and populated when
   measured", not "always emitted, zero when not". An artifact produced with no
   sampler at all therefore carries no `memory` key.
2. Within a present `memory`, `series` is absent when `--metrics-series false`
   suppresses it while `peak` and `series_decimation` remain. So one produced
   with the series suppressed carries `memory.peak` with no `series`.
3. **An `identity` field with no value omits its key entirely**, rather than
   being emitted as `""`. The reason is the same one the memory rule rests on,
   and it is sharper here: `""` is a *value*, so it compares equal between two
   runs that differ — an empty `wpa_config_hash` reads as "unchanged" against a
   run configured differently, which is precisely the comparison failure this
   artifact exists to prevent. In practice all nine are populated on any
   successful run, because `RunWpa` assigns them from the descriptor and the
   batch it already holds and a `RunWpa` failure returns before the report is
   built; the omission rule is the guard for the case where that stops being
   true.

**The presence test is "was it measured", never "was it emitted".** Those are
different questions — `emit_series` decides whether the samples are *published*,
not whether they were *collected* — and keying presence on the published copy
makes the artifact contradict itself: under `--metrics-series false` the
per-span blocks still carry real measured figures, so a run level that claimed no
measurement would be false next to them, and the peak would be silently lost.

**Why `config` echoes the analysis configuration only as a hash and one flag.**
The effective `AnalysisConfig` is already recoverable from the two configuration
hashes in `identity`: `wpa_configuration_hash` covers the component timeout,
memory cap, thread count, and the rule and model bundle versions
(`src/analysis/ProjectAnalyzer.cpp:106-122`), and `svf_configuration_hash`
covers the pointer-analysis kind, soft budget, graph-node, emitted-fact, and
alias-pair limits and field sensitivity (`:99-101` via `ToSvfConfig`, `:82-91`).
A change to any of those fields moves the corresponding hash, so two runs whose
hashes agree had the same effective configuration. Echoing the fields
individually would duplicate that and force this library to depend on the
analysis library's config type.

The one exception is `run_cpp_conformance_oracle`: no hash covers it, and it
changes what the run *does* by executing a second full WPA and requiring the two
canonical results to agree. It is therefore emitted explicitly as
`config.conformance_oracle`. This is why the block carries
`{ "metrics": {}, "conformance_oracle": false }` rather than the
`{ "analysis": {}, "metrics": {} }` an earlier draft showed. **Durations are integer nanoseconds and
never floating point.** A float's text form is a diff-noise and portability
hazard, and byte-diffing is this file's purpose; the text report performs the
human conversion.

A `phases` node:

```json
{ "name": "wpa.component.execute", "count": 13716,
  "wall_inclusive_ns": 0, "wall_self_ns": 0, "cpu_inclusive_ns": 0,
  "min_ns": 0, "max_ns": 0,
  "distribution": { "p50_ns": 0, "p95_ns": 0, "p99_ns": 0, "max_ns": 0,
                    "samples_truncated": false },
  "top_n": [ { "rank": 1, "label": "", "wall_ns": 0 } ],
  "memory": { "rss_start": 0, "rss_end": 0, "delta": 0, "peak_within": 0 },
  "children": [] }
```

`cpu_inclusive_ns` appears only on interval-bearing spans; `distribution` and
`top_n` only on distributed spans; `memory` only where intervals were retained
(section 4.5).

### 6.4 The stdout report

Indented tree, wall/self/CPU in human units, peak resident and delta columns,
then a component block, a store block, and the cross-check. The figures below
are **illustrative of layout only** — they are not measurements, and this design
produces none.

```
Analysis phase report
  run                                         572.820s    0.101s  421.670s   8.59 GiB  +8.59
  ├─ cli.ingest                                 1.234s    1.100s    1.234s   0.89 GiB  +0.12
  ├─ m5.svf                                   105.100s  105.100s   52.910s   3.52 GiB  +2.34
  │  ├─ m5.svf.andersen                        20.300s   20.300s   20.300s   2.90 GiB  +1.10
  │  └─ m5.svf.svfg                            32.610s   32.610s   32.610s   3.52 GiB  +0.62
  └─ wpa.orchestrate                          254.900s   41.200s  186.400s   6.92 GiB  +3.40
WPA components: 13716 expected, 5614 reused, 8102 executed
  execute p50 1.9ms · p95 14.2ms · p99 41.0ms · max 2.31s
  slowest: flow/scc:sha256:4a1c… 2.310s · memory_effects/scc:sha256:8f02… 1.870s
Store: analysis_facts 1249792 rows · provenance_edges 1375911 rows · metadata.db 412 MiB · total 3.18 GiB
Cross-check: components 13716 (store) == 13716 (in-memory) OK
```

### 6.5 Diffability rules

Recorded here so future instruments do not quietly break them:

1. JSON object keys sorted; counters sorted by name.
2. Top-N ordered by `(wall_inclusive desc, label asc)`.
3. Durations are integers; no floats anywhere.
4. **No absolute paths anywhere in the artifact.** `project_root` is
   deliberately excluded. It is the one field guaranteed to differ between
   machines and checkouts, and `manifest.json` already carries it.
5. The series lives in its own block, so `--metrics-series false` yields a calm
   diff.
6. The `identity` block is separable, so a comparison script can exclude the
   run-scoped coordinates without parsing the rest.

## 7. Failure handling, overhead, and concurrency

### 7.1 Failure handling

**Metrics never fail the analysis.** Recording, sampling, and artifact writing
are observability; the analysis is the product. But a *silent* degradation would
invite quoting a partial curve as a complete one, which is the round-3 failure
mode this design exists to prevent. Every degradation is therefore loud and
self-describing: a stderr line, `"complete": false`, and an entry in
`"diagnostics"`.

| Failure | Behaviour |
| --- | --- |
| `pthread_create` fails | degrade to span-boundary sampling; `complete: false`; diagnostic |
| A platform memory reader fails (`task_info` or `statm`) | append **no sample at all** and record a diagnostic once; never append the failure value. A reader that returns 0 on failure and has that 0 appended as a genuine sample produces a *present* block of zeroes on the measured path — the reading this design exists to prevent, reached through a different door. |
| The component counts **disagree** — the sum of `components_by_kind` differs from the `wpa.components.expected` counter | `complete: false` and a **degraded** diagnostic, not a note. A divergence is a contradiction, not an absence: two figures in one artifact both claim to be the expected component count and do not match, so neither can be trusted. The two-way diagnostic split is defined by what the message says about the artifact — a *failure* means a measurement did not happen, a *not-recorded* means a field was never produced, and a *contradiction* means the artifact is internally inconsistent. The third kind is a degradation. |
| Artifact path unwritable | one stderr line; exit code unchanged |
| Store read-back fails (schema drift, locked DB) | omit only the `store` block; keep the rest; diagnostic |
| Negative span duration (reachable only via a misbehaving injected clock) | clamp to 0 and count a diagnostic, **unconditionally** |
| `EndSpan` token not the innermost open span | record a diagnostic and return without folding; never abort |
| Series capacity reached | adaptive thinning; `series_decimation` incremented |
| Per-span sample cap reached | `samples_truncated: true`; diagnostic |

**The rejected alternative for an unwritable artifact.** Failing the command
would be defensible if the artifact were a product. It is not: by the time the
report is written, the summaries, facts, CPG, and manifest are already durably
committed, so failing the command would report completed work as failed. The
stderr line and `complete: false` are the honest signals instead.

### 7.2 Structurally absent failures

Two classic instrumentation bugs are unrepresentable here, and are recorded so
that nobody writes defensive code against them:

- **Unclosed spans.** `PhaseSpan`'s destructor closes exactly what its
  constructor opened, on every return path including the error paths.
- **A span folded onto the wrong stack.** Spans are taken from one thread;
  there is no second stack to get wrong (section 4.7).

`RunMetrics::BeginSpan`/`EndSpan` are also reachable directly, where RAII does
not apply. That path is checked rather than assumed: a mismatched `EndSpan`
records a diagnostic and returns without folding (section 7.1), so a caller
error becomes a visible diagnostic instead of a silently corrupted tree. In
correct use the check never fires and the two bullets above hold.

### 7.3 Overhead budget

Recording must add **≤0.5 % CPU and ≤0.05 GiB peak resident** on the LevelDB
fixture, measured as worst-of-three runs of the same binary with metrics on
versus off. Three runs are the minimum honest sample, because this fixture's own
spread is 0.72 GiB (round-3 section 10.4) — a two-run comparison is not
evidence.

The design's contribution to that budget:

- `getrusage` costs roughly 1–2 µs, so `cpu_inclusive` is captured **only on
  interval-bearing spans** (tens), never on the 68,580 per-component stages,
  which take `steady_clock` reads only (≈50 ns each, on the order of 17 ms
  across a 421 s CPU run).
- The sampler buffer is pre-allocated at ≈1.5 MiB; the sampler thread never
  allocates.
- The analysis path takes no lock. `RunMetrics` needs no mutex because VERITAS
  is single-threaded at the component level and `SouffleWpaExecutor.cpp:435-437`
  rejects `limits.threads != 1` outright; the sampler is the only other actor,
  and it writes only into its own pre-allocated buffer behind an atomic cursor.

### 7.4 The sampler is the codebase's first thread

Verified: `grep` for `std::thread`, `std::async`, `std::jthread`,
`pthread_create`, and thread pools across all first-party `src/` and `include/`
returns nothing. The sampler would be the first concurrency in a deliberately
single-threaded codebase, and that is a design decision rather than an
implementation detail.

**`std::thread` is not available.** Its constructor throws `std::system_error`
on failure, which under `-fno-exceptions`
(`.claude/rules/cpp-compilation-policy.md`) is unusable — the throw path aborts.
The sampler therefore uses `pthread_create` directly, checks its error code
explicitly, and joins in the destructor. This adds a `Threads::Threads` link
dependency.

**Degradation is the price of admission.** If `pthread_create` fails, the
recorder falls back to span-boundary sampling, sets `complete: false`, and
records the reason. It never runs blind silently.

**A note for the next reader.** This thread's existence should be discoverable
from a comment at its definition, because "VERITAS is single-threaded" is a
property the rest of the codebase currently relies on without stating it.

## 8. Verification strategy

### 8.1 Unit tests

All of `observability` is testable without a real clock:

- **Scripted clock:** a `Now` sequence returning fixed time points makes every
  duration an exact integer. No test sleeps.
- **Span tree:** nesting; `wall_self = wall_inclusive − children`; count
  aggregation across repeated spans; min/max.
- **Percentiles:** nearest-rank against a hand-computed sorted vector,
  including the boundary and the tie case.
- **Top-N tie-break:** two spans with identical durations must order by label
  ascending. Constructed deliberately to test exactly that.
- **Interval join:** a synthetic series against synthetic intervals, including a
  sample landing exactly on `t_start` and exactly on `t_end`, per the boundary
  rule in section 4.5.
- **Series thinning:** forced overflow must double `series_decimation`, preserve
  the earliest sample, and keep the timeline spanning the whole run.
- **Counters and ordering:** rendered sorted by name regardless of insertion
  order.

### 8.2 The identity guard

The executable form of Rule 2, and the single most important test in this
design:

- Derive `run_id`, `batch_id`, `projection_id`, and the summary ids **twice from
  identical inputs — once recording, once with `nullptr`** — and assert byte
  equality.
- Compare the four published table digests between the same two runs using the
  round-3 section 9.1 instrument.

Because both derivations come from the *same binary*, unlike round 3's
cross-build comparison, the exclusion set is **empty** and the assertion is
total. This is strictly stronger than section 9.1's Step 2b, which needed
exclusions only because it compared different builds.

### 8.3 Renderer determinism and hygiene

- Rendering the same `RunReport` twice is byte-identical.
- The artifact contains no floating-point numbers and no absolute paths — a
  direct grep for the temp-directory prefix, in the spirit of the existing
  no-absolute-path-leak test at `VeritasBuildAnalyzeCliTest.cpp:143-145`.
- The artifact parses and every required key is present with the right type
  (the versioned-schema contract).

**No checked-in golden with real timings.** It would be brittle by construction;
the schema test carries the contract instead.

### 8.4 CLI tests

- `--metrics false` produces no artifact and no report block.
- The default produces both, and `--metrics false` output matches the
  pre-change stdout byte for byte.
- `--metrics maybe` is `InvalidArgument`.
- `--metrics-interval-ms 0` is accepted and disables the series entirely: the
  run succeeds and the artifact still parses, but no samples are collected. The
  span-boundary fallback of section 7.4 is reachable only through a
  `pthread_create` failure, never through this flag.
- `--metrics-top-n K` honours K.
- `--metrics-path` is honoured.

### 8.5 The measured criterion is not a CI assertion

The overhead budget of section 7.3 is **measured and recorded**, not asserted in
a test. Wall-clock assertions in this suite are flaky for reasons already on
record: macOS background daemons have stretched the heaviest integration tests
past their timeouts, and `ctest -j` has raced on fixed fixture paths. Unit tests
assert structure; the budget is established by measurement and written into this
document's verification record when the implementation lands, in the round-3
style.

### 8.6 Pre-push

Per `.claude/rules/pre-push-verification-policy.md`: clean build, full `ctest`
with zero skips, `git diff --check`, license headers (the two new source files
and the new `CMakeLists.txt`), and a clean tree. A `GTEST_SKIP` reports as
passed, so the test count is verified against the registry's expected name set
rather than the summary line.

## 9. Implementation shape

### 9.1 Files

| File | Change |
| --- | --- |
| `include/veritas/core/RunMetrics.h`, `src/core/RunMetrics.cpp` | new — recorder, spans, counters, distributions, sampler (std + POSIX only) |
| `include/veritas/observability/RunReport.h`, `src/observability/RunReport.cpp` | new — report model, JSON schema, text rendering, inventory assembly |
| `include/veritas/observability/StoreSummary.h`, `src/observability/StoreSummary.cpp` | new — post-publication store read-back (depends on `summarydb`/`facts`) |
| `src/observability/CMakeLists.txt` | new — links `veritas_core`, `veritas_facts`, `veritas_summarydb`, LLVM |
| `src/core/CMakeLists.txt` | `RunMetrics.cpp` added to the library; `find_package(Threads)` + `Threads::Threads` |
| `include/veritas/analysis/ProjectAnalyzer.h` | `AnalyzeProject` gains a trailing defaulted `RunMetrics*` |
| `src/analysis/ProjectAnalyzer.cpp` | spans; `RunWpa` signature; publication and batch spans |
| `include/veritas/wpa/WpaOrchestrator.h` | `WpaRunRequest::metrics` |
| `src/wpa/WpaOrchestrator.cpp` | orchestrate and per-component spans; reuse/execute counters |
| `include/veritas/analysis/svf/SvfAnalysisStage.h`, `SvfSession.h`, and their `.cpp` | metrics parameter and the five session-step spans |
| `include/veritas/facts/AnalysisFactBus.h`, `src/facts/AnalysisFactBus.cpp` | `SetMetrics` plus validate and sink spans |
| `src/tools/veritas-build.cpp` | five flags, report emission, artifact write, diagnostics on stderr |
| `docs/guides/` | a short reader's guide carrying the diffability rules |

### 9.2 Staged tasks

1. Recorder and renderer as standalone units, fully unit-tested against the
   scripted clock, with no pipeline integration. The identity guard and the
   renderer-hygiene tests land here.
2. Threading: `PhaseSpan` at the top-level sites in `ProjectAnalyzer`, plus the
   `AnalyzeProject` parameter and the CLI's `--metrics` flag and artifact write.
   At the end of this stage a real run produces a populated artifact.
3. Sub-spans: SVF session steps, WPA orchestrate and per-component spans with
   distributions, counters, and top-N.
4. Publication spans, the `AnalysisFactBus` seam, the sampler thread and the
   interval join, and the inventory and store read-back.
5. The measured overhead comparison and its record in this document; the
   reader's guide.

Stages 1–2 are independently useful: even the flat report alone retires the
±15 s sampling uncertainty of round-3 section 2.2.

## 10. Expected first finding

`veritas-build analyze` currently ingests the project twice. `Analyze()`
(`src/tools/veritas-build.cpp:211-220`) calls `ResolveProjectInput` and
`LoadProjectManifest` and writes the diagnostic manifest; then
`ProjectAnalyzer::Impl::AnalyzeProject` (`src/analysis/ProjectAnalyzer.cpp:362-367`)
calls `ResolveProjectInput` and `LoadProjectManifest` again.

This design **measures** that duplication (`cli.ingest` versus `m1.ingest`) and
does not repair it: repairing it changes the CLI/library boundary and belongs in
its own change. The `cli.ingest` and `m1.ingest` spans exist so that the cost is
a number rather than an inference — which is the whole point of this design, and
which is why the duplication is recorded here as the first thing the instrument
should be expected to show.

## 11. Open risks

1. **The sampler thread is a new class of failure in a single-threaded
   codebase.** A pthread in the CLI means every integration test that runs
   `analyze` through the binary now creates one. Mitigation: the library default
   is `metrics == nullptr`, so direct `AnalyzeProject` callers — which is most of
   the 811-test suite — create nothing; and creation failure degrades rather
   than aborts. The risk that remains is test-level, and stage 2 should watch
   for it.

2. **The store read-back is coupled to schema versions.** A future migration
   that renames a table breaks the `store` block. This is mitigated by
   block-wise degradation, but the coupling is real and is why the read-back
   lives in `observability` rather than `core`.

3. **`cpu_inclusive` becomes misleading under concurrency.** It is a process CPU
   delta. Today VERITAS is single-threaded; if that changes, the field must be
   renamed or re-derived, and its documented meaning must be updated. Recorded
   here because the field's apparent precision invites over-reading.

4. **Interval-bearing spans are a fixed list, maintained by hand.** A new phase
   added without a span produces no row rather than a wrong one, so the failure
   is visible by absence — but only to a reader who knows the pipeline. The
   reader's guide should carry the span inventory so absence is noticeable.

5. **The artifact's stability depends on discipline, not enforcement.** Rules
   like "no absolute paths" and "no floats" are enforced by tests only where a
   test was written (section 8.3); a new field added in a later change can
   violate them. The diffability rules in section 6.5 are recorded as a
   contract for exactly that reason.

6. **The overhead budget is a target, not a result.** It is untested until
   stage 5 measures it, and round 3's lesson applies to this document as much
   as to that one: every magnitude quoted here about cost is a hypothesis until
   a task measures its own target. This design deliberately quotes none.
