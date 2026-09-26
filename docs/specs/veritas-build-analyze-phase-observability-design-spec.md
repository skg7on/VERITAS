# `veritas-build analyze` Phase Observability Design Specification

**Status:** Draft for review

**Depends on:**
[`veritas-build-analyze-round3-performance-design-spec.md`](veritas-build-analyze-round3-performance-design-spec.md)
for the measurement method this design replaces and for the failure mode it
exists to prevent. Section 9.6 of that document is the motivating evidence:
four root-cause magnitudes were refuted on measurement, and every one of them
was wrong in the same direction — a cost was read out of the code, multiplied
by a component count, and recorded as a root cause without ever being timed.

**Tracking issue:** [#138](https://github.com/skg7on/VERITAS/issues/138), which
carries the problem statement, the scope, the acceptance criteria, and the known
open items.

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
during the span" and must not be read as the span's own cost.

**And a correction to this section's own original justification, because it
cited a magnitude nobody measured.** An earlier revision of this paragraph said
the field answers the question round 3 could not, quoting "421.67 s CPU against
572.82 s wall, with the gap unattributed". Those two figures are not from the
same series and their difference is not a gap. `572.82 s` is run 4 of the
**pre-round** binary `62bc573` (round-3 spec section 2.1), while `421.67 s` is
the **post-round** worst-of-three CPU (its section 9.4); the difference between
them is the round's measured −26.5 % gain, not an unattributed wall-versus-CPU
gap. That record also states those runs were "99.7 % CPU-bound", i.e. wall minus
CPU was 0.07–1.24 s, so at *run* granularity there was never a large gap to
attribute. The instrument's value is therefore not closing a run-level gap; it
is **per-phase** attribution, which is what a process-wide figure cannot give
and what made this round's phase boundaries come from 30-second stack samples.
This is recorded rather than quietly deleted because it is the same error the
round-3 record identifies as its most transferable lesson — a plausible
magnitude asserted without measurement — committed while writing a design whose
subject is that failure.

### 4.3 Distributed spans

The **four** per-component spans retain every sample and report exact
percentiles. Worst case is 13,716 components × 4 spans ≈ 54,864 samples ≈ 440 KB, held for
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
  54,864 intervals.

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

`identity` is separated because part of it is run-scoped, so a comparison script
can exclude those coordinates without parsing the rest. **Exclude them
selectively, not wholesale.** Only `run_id` and `batch_id` move between two runs
of the same fixture; the other seven fields are content- and config-derived and
therefore stable, and they are the comparison the block exists to enable. An
earlier revision of this sentence said the block was separated so a script "can
ignore it" — the wholesale reading that section 6.5's rule 6 corrects, and the
one that would discard the configuration evidence while believing it had
excluded noise.

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

All three are reported with a `metrics note:` line, and — this is the part an
earlier revision left ambiguous — **their keys are omitted from the artifact, not
emitted as `0`.** The omission mechanism is the same one `memory` and `identity`
use, applied to numeric fields: an unproduced count is `std::optional<std::uint64_t>`
in the report model and is absent from the JSON when unset.

The reason is the one this design applies everywhere else, and it is worth
stating in the numeric case explicitly because it is less obvious than the string
case. `0` is a *value*. A reader diffing two artifacts sees `svfg_edges: 0` in
both and concludes it is unchanged, when in fact neither run measured it. A
diagnostic string elsewhere in the document does not repair that, because a
field-level comparison never consults it. Presence must mean "measured" at the
granularity a consumer actually reads.

The general lesson they illustrate, which cost this project an always-zero counter
and is recorded because it is easy to repeat: **verify that a source carries the
value, not merely that it exists.** Checking that an accessor is reachable is not
checking that anything writes what it reads.

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
  run                                           6.240s    0.180s    6.410s   1.02 GiB  +0.41 GiB
  ├─ cli.ingest                                 0.412s    0.390s    0.412s   0.11 GiB  +0.02 GiB
  ├─ m5.svf                                     2.810s    2.810s    2.640s   0.86 GiB  +0.24 GiB
  │  ├─ m5.svf.andersen                         0.980s    0.980s    0.980s   0.60 GiB  +0.11 GiB
  │  └─ m5.svf.svfg                             1.120s    1.120s    1.030s   0.86 GiB  +0.09 GiB
  └─ wpa.orchestrate                            2.230s    0.470s    2.980s   1.02 GiB  +0.15 GiB
WPA components: 13716 expected, 5614 reused, 8102 executed
  wpa.component.execute p50 1.9ms · p95 14.2ms · p99 41.0ms · max 2.31s
  slowest: flow/scc:sha256:4a1c… 2.310s · memory_effects/scc:sha256:8f02… 1.870s
Store: analysis_facts 1249792 rows · provenance_edges 1375911 rows · metadata.db 412 MiB · total 3.18 GiB
Cross-check: components 13716 (store) == 13716 (in-memory) OK
```

Three details in that sample are load-bearing, and an earlier revision got all
three wrong: **every** delta carries its unit, not only the first row, because
`FormatSignedBytes` always appends one; the block label is the span's **full**
name (`wpa.component.execute`), matching what the renderer prints; and the row
counts are deliberately unlike any figure quoted elsewhere in this document,
because an earlier revision reused the two numbers that section 4.2 now records
as a mispaired citation.

### 6.5 Diffability rules

Recorded here so future instruments do not quietly break them:

1. JSON object keys sorted; counters sorted by name.
2. Top-N ordered by `(wall_inclusive desc, label asc)`.
3. Durations are integers; no floats anywhere.
4. **No absolute paths anywhere in the artifact.** `project_root` is
   deliberately excluded; `manifest.json` already carries it. Do not restate
   this as "the only machine-scoped field": the `environment` block is
   machine-scoped throughout (`os`, `arch`, `cpu_model`, `cores`, `ram_bytes`,
   `build_type`, `host_compiler`, `veritas_version`, `git_revision`), so a
   comparison script that wants cross-machine diffs must exclude that block as
   well as the run-scoped coordinates below.
5. The series lives in its own block, so `--metrics-series false` yields a calm
   diff.
6. The `identity` block is separable, so a comparison script can exclude the
   **run-scoped** coordinates without parsing the rest. Exclude those
   *selectively*: only `run_id` and `batch_id` move between two runs of the same
   fixture, while `repository_id`, `revision_id`, `build_variant_id`,
   `projection_id`, `svf_config_hash`, `wpa_config_hash` and
   `engine_toolchain_identity` are content- and config-derived and therefore
   stable. Blanket-excluding the whole block discards exactly the configuration
   comparison it exists to enable — and two runs whose configuration hashes
   disagree did *not* have the same effective configuration.

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
  interval-bearing spans** (tens), never on the 54,864 per-component stages,
  which take `steady_clock` reads only (≈50 ns each, on the order of 14 ms
  across a 421 s CPU run).
- The sampler buffer is pre-allocated at ≈1.5 MiB; the sampler thread never
  allocates.
- The analysis path takes no lock. `RunMetrics` needs no mutex because VERITAS
  is single-threaded at the component level and `SouffleWpaExecutor.cpp:435-437`
  rejects `limits.threads != 1` outright; the sampler is the only other actor,
  and it writes only into its own pre-allocated buffer behind an atomic cursor.

The measurement against this budget is section 12, with both verdicts — the CPU
half met, the memory half exceeded and unresolvable at three runs per series.

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
   **Measured: section 12.** The CPU half of the budget holds; the memory half
   is exceeded by the worst-of-three delta and is not resolvable by this method,
   and section 12 states both without revising the ceiling.

## 12. Verification record (2026-09-26)

The measurement section 8.5 defers to, in the form section 9.6 of the round-3
design specification established: provenance, the measured series, a verdict on
each budget number, and the artifact's own reading of the run.

**Provenance.** Source: branch `claude/analyze-phase-observability-design`,
whose tip at the time of writing is `64d7f44`. Command, one run per output root,
serially on an otherwise idle machine, with `/usr/bin/time -lp` measuring from
outside the process:

```bash
./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output <fresh /tmp dir>
```

Debug, host compiler `/opt/homebrew/opt/llvm@17/bin/clang++` (Clang 17.0.6)
against LLVM 24.x libraries, `VERITAS_WPA_ENGINE=souffle`. Apple M5, arm64,
Darwin 27.0.0, 10 cores, 32 GiB. The six runs used **one binary**, built once
before the first run and unchanged across the series, so the two series differ
by the flag and by nothing else. That binary carries the branch's source: its
diagnostics quote the sub-span and span-coverage fixes, and the only source
commit the branch gained after the build (`64d7f44`) changes comments alone in
`src/tools/veritas-build.cpp`. The artifact's own `environment.git_revision`
reads `91f8dc5`, which is **not** a source revision at all: it is stamped at
CMake configure time (`src/core/Version.cpp.in`) and is stale by design, so the
artifact of a run on this branch reports the revision the build tree was last
configured at. Recorded here so that a reader diffing an artifact against
`git log` is not misled by it.

The criterion is evaluated on the `/usr/bin/time` figure, because that is the
figure section 7.3 names, and not on the artifact's sampled one.

**The two series.** Metrics off is `--metrics false`; metrics on is the default.
Three runs each; worst of three is what both ceilings are judged on.

| Run | Series | Wall (s) | CPU user+sys (s) | Max RSS (GiB) |
| --- | --- | ---: | ---: | ---: |
| 1 | metrics off | 420.79 | 420.10 | 6.7263 |
| 2 | metrics off | 420.20 | 419.25 | 7.8923 |
| 3 | metrics off | 423.68 | 422.45 | 7.7146 |
| 4 | metrics on | 419.95 | 419.09 | 7.7990 |
| 5 | metrics on | 417.49 | 416.91 | 8.2212 |
| 6 | metrics on | 418.20 | 417.37 | 7.9441 |

Worst of three per series: wall **423.68 s off / 419.95 s on**; CPU **422.45 s
off / 419.09 s on**; max RSS **7.8923 GiB off / 8.2212 GiB on**.

**CPU — the ≤0.5 % ceiling is met, and the instrument is not detectable.** The
worst-of-three CPU delta is **−3.36 s, −0.80 %**: the metrics-on series is
*lower* than the metrics-off series. That is not an improvement and must not be
read as one — measurement cannot make analysis faster — it is the statement that
this series cannot see the instrument at all. The reason is the resolution: CPU
spread **within** the metrics-off series alone is **3.2 s** (422.45 − 419.25),
the same order as the delta between the series, so a 0.80 % movement is inside
this fixture's own run-to-run variation. The honest form is **no measurable CPU
overhead at this resolution**, which satisfies the ≤0.5 % ceiling: the measured
difference is inside the fixture's own variation, so it is not evidence of a
cost, and no direction of it may be claimed as a benefit.

**Max RSS — the ≤0.05 GiB ceiling is exceeded by the worst-of-three delta, and
the criterion is not resolvable by this method.** The numbers as measured are
**7.8923 GiB off / 8.2212 GiB on**, a delta of **+0.3289 GiB (+4.17 %)**, which
is 6.6× the 0.05 GiB budget. **The criterion is not met on this evidence, and it
is also not decided by it**, and both statements belong in the record together:
the metrics-off series' own run-to-run spread is **1.166 GiB** (7.8923 − 6.7263),
which is **3.5× the observed delta** and **23× the entire budget**. A threshold
one twenty-third the size of the series' own noise is not decidable from three
samples per side, so the measurement does not settle whether a real increase
exists.

Two things can be said about it beyond that. The first is negative and it is
useful: the recorder's own state is on the order of **2 MiB** — a 1.5 MiB
pre-allocated series buffer plus transient accumulators, both bounded at design
time (section 7.3) — so a genuine 0.3 GiB shift would **not** be attributable to
the recorder's data structures. Nothing in this design allocates in proportion
to run length, which means the delta, if real, comes from somewhere the
instrument perturbs only indirectly, and an instrument whose own cost is
bounded in MiB does not become a 300 MiB cost by running on a larger fixture.
The second is the honest reading: **consistent with a small real increase, not
distinguishable from noise at n = 3.** The ceiling is **not revised** here, and
the criterion is **not** reported as met. This is the same class of honest
negative as round-3 section 9.4's "the wall gate is not measurable in this
environment", and it is recorded for the same reason: a criterion that the
method cannot decide must say so rather than borrow confidence from a
measurement that cannot support it.

**The artifact's own reading of the run.** The artifact does not record which
`/usr/bin/time` figure belongs to it, so run 4 is identified by its root wall
time: the three metrics-on artifacts order exactly as the external walls order
runs 4–6, each 0.63–0.64 s below the wall that `/usr/bin/time` reports for it.
Run 4's top-level rows with the figures as the artifact records them — count,
wall inclusive, wall self and CPU inclusive. The rows are ordered by wall time
for reading; the artifact itself carries siblings in span-name order, which
section 6.5's sorted-key rule produces:

| Span | count | wall (s) | self (s) | CPU (s) |
| --- | ---: | ---: | ---: | ---: |
| run (root) | 1 | 419.316 | 8.136 | 418.363 |
| wpa.orchestrate | 1 | 157.643 | 15.768 | 158.017 |
| facts.publish | 1 | 119.914 | 0.001 | 119.077 |
| m5.svf | 1 | 74.419 | 0.827 | 74.243 |
| facts.batch_assemble | 1 | 32.713 | 32.713 | 32.697 |
| m2m3.publish_summaries | 1 | 11.749 | 11.749 | 11.652 |
| m4.local_analysis | 1 | 10.646 | 10.646 | 10.518 |
| m6.cpg_projection | 1 | 2.142 | 2.142 | 2.139 |
| m5.merge_svf_facts | 1 | 1.576 | 1.576 | 1.575 |
| wpa.graph_build | 1 | 0.355 | 0.355 | 0.353 |
| cli.ingest | 1 | 0.010 | 0.010 | 0.006 |
| wpa.scc_state_flush | 1 | 0.004 | 0.004 | 0.003 |
| m1.ingest | 1 | 0.004 | 0.004 | 0.004 |
| m5.model_bundle_load | 1 | 0.003 | 0.003 | 0.000 |
| facts.store_open | 1 | 0.001 | 0.001 | 0.001 |

The four per-component spans under `wpa.orchestrate` — `canonicalize` 66.315 s,
`execute` 42.096 s, `materialize` 33.177 s, `cache_lookup` 0.287 s — each carry
**count 13,716** and no CPU column, which is section 7.3's design decision
visible in the artifact: `getrusage` is taken only on interval-bearing spans, so
the per-component stages show `-` rather than a fabricated zero.

Two checks against the external instrument, and both agree: the artifact's root
CPU figure is **418.363 s** against `/usr/bin/time`'s **419.09 s** for the same
run, the 0.73 s being process start-up, teardown, and the report's own rendering
and writing, which the `run` span deliberately excludes (section 6.4); and its
root wall is **419.316 s** against a wall of **419.95 s**, a 0.63 s gap of the
same composition.

The artifact's sampled peak for run 4, **8,317,222,912 B**, is not offered as a
third agreement. It sits beside the criterion's own **7.7990 GiB** for that run
rather than replacing it, and it is the lower of the two by design: the sampler
observes at a 250 ms interval, so its maximum is a lower bound on the true one
and can miss the instant the peak occurs. `/usr/bin/time`'s `ru_maxrss` remains
the figure section 7.3's ceiling is judged on, in both series equally.

**The first finding the instrument was built to surface.** Section 10 records
that `veritas-build analyze` ingests the project twice and that this design
measures the duplication instead of repairing it. It is now a measured pair in
the artifact:

| Run | `cli.ingest` (ms) | `m1.ingest` (ms) |
| --- | ---: | ---: |
| 4 | 10.0 | 4.0 |
| 5 | 9.8 | 3.8 |
| 6 | 11.0 | 4.0 |

The analyzer's repeat is the smaller of the two — **3.8–4.0 ms** — while the
CLI's own ingest, which also writes the diagnostic manifest (section 10), is
**9.8–11.0 ms**. The duplication is therefore **confirmed as a fact and measured
as immaterial on this fixture**: the repeated work is ≈4 ms of a 419 s run,
three orders of magnitude below the ±15 s of phase-boundary uncertainty that
round 3 section 2.2 recorded and that section 1 cites as this design's first
motivation. That is the form this design predicted: the inference "the project
is ingested twice, so roughly half the ingest cost is wasted" had never been
timed, and timing it puts the waste at ≈0.001 % of the run rather than at half
of a visible phase. Whether to still repair it is a question about the
CLI/library boundary, not about cost.

**Refuted, and corrected before it shipped.** The design's first draft
instructed the SVF session to emit `svfg_edges` from
`SVFG::getTotalEdgeNum()`, having verified that the accessor is reachable
through `SVFG → VFG → GenericGraph`. Existence was not the question: the method
reads `GenericGraph::edgeNum`, and nothing increments that field for a VFG or
SVFG, so it returns 0 for every SVFG ever built. The implementation record has
the measurement: **0** reported against `svf.svfg_nodes` **24** on the test
fixture, where SVF's own SVFG statistic reports **19 edges** for the same graph.
The implementer reported that rather than substituting a plausible number, the
counter was removed, and **a plan that cited that line as "verified" would have
shipped an always-zero field in place of a measurement**. Section 6.3 carries
the field's disposition; section 10 of the round-3 specification carries the
general rule. A second correction of the same class is worth recording, because
it was a name promising more than the code did: the `wpa.graph_build` span
initially did not cover the graph construction its name names, and was widened
to open above `CallGraph::FromSummaries` and close at the frozen expected set.
The rule the two produced — a count travels as a recorder counter added at the
producing site, never through a second plumbing path — is why a count with no
producer is now visibly absent rather than plausibly zero.

**One artifact-format change lands after this measurement, and the artifacts
above predate it.** The three inventory counts with no producer —
`inventory.output.svfg_edges`, `inventory.incrementality.summaries_recomputed`
and `...summaries_reused` — were omitted-as-keys rather than emitted as `0` after
the series was taken, per section 6.3. The six artifacts under `/tmp/vm-*`
therefore carry those three keys at `0` with a `metrics note:` line naming each,
and this record's figures are unaffected by the change: it moves no span, no
counter and no duration. The reader's guide
(`docs/guides/analyze-phase-report-guide.md`) states the omission contract and
records that an artifact produced before it exists.

**What this record does not establish.** It does not establish that the memory
budget is met — it does not establish that it is missed either, and that
indecision is the finding. It makes no wall-time or performance claim in either
direction, which section 3's first non-goal forbids; the −0.80 % CPU figure is a
statement about the instrument's detectability and not about the analysis. And
it is one machine, one fixture, three runs per series: only the CPU verdict is
strong enough to carry beyond this host.
