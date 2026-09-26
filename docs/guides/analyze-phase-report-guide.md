# Reading the `veritas-build analyze` Phase Report

`veritas-build analyze` produces two observability outputs: a human-readable
phase report appended to stdout, and a machine-readable `run-metrics.json`
written beside `manifest.json` in the output directory. This guide explains
how to read them, what an absent row or an absent key means, and which
properties of the artifact are a contract that a future change must not
quietly break.

The authoritative content is the
[phase observability design specification](../specs/veritas-build-analyze-phase-observability-design-spec.md).
Where this guide and that specification disagree, the specification wins and
this guide is updated in the same change.

## 1. The two outputs

| Output | Location | Reader |
| --- | --- | --- |
| Phase report | stdout, after the existing `Analysis complete` block | a human |
| `run-metrics.json` | `<output>/run-metrics.json`, sibling to `manifest.json` | a script, or a later comparison |

`manifest.json` stays byte-identical across two runs of the same fixture, and
its canonical-bytes path is used for hashing. Timings can never satisfy that,
so the measurement was given its own file rather than a new key in the
manifest (spec §6.1).

Both outputs are **write-only**. No analysis code path opens
`run-metrics.json`; there is no baseline store, no trend file, and no CI
regression gate. Comparing two runs means writing a small script, and
[section 8](#8-the-diffability-contract) lists the rules that make that
script straightforward. `--metrics false` suppresses both outputs entirely:
no report block, no artifact, and no sampler thread.

The report is emitted **after** the run, never during it. There is no live
progress display.

## 2. The columns

The report's rows are an indented tree. There is **no header row**; the
columns, left to right, are:

```
Analysis phase report
  run                                          572.820s    0.101s  421.670s   8.59 GiB  +8.59 GiB
  ├─ cli.ingest                                  1.234s    1.100s    1.234s   0.89 GiB  +0.12 GiB
  ├─ m5.svf                                    105.100s  105.100s   52.910s   3.52 GiB  +2.34 GiB
  │  ├─ m5.svf.andersen                         20.300s   20.300s   20.300s   2.90 GiB  +1.10 GiB
  │  └─ m5.svf.svfg                             32.610s   32.610s   32.610s   3.52 GiB  +0.62 GiB
  └─ wpa.orchestrate                           254.900s   41.200s  186.400s   6.92 GiB  +3.40 GiB
```

The tree above is abridged and the figures show layout only. They are not
measurements of anything; [section 4](#4-the-span-inventory) has the full set
of rows a real run produces.

| Column | Field | Meaning |
| --- | --- | --- |
| wall | `wall_inclusive` | total wall time inside the span, children included |
| self | `wall_self` | `wall_inclusive` minus the direct children's inclusive time |
| CPU | `cpu_inclusive` | process CPU consumed across the span — see [section 3](#3-cpu_inclusive-and-its-one-caveat) |
| peak | `memory.peak_within` | highest resident-set sample inside the span's window |
| delta | `memory.rss_delta` | `rss_end − rss_start`, signed, because a phase may release more than it allocates |

**Self time is the column that answers "where did the time go".** A parent's
inclusive time is mostly the time its children took, so reading inclusives
tells you the same thing at every level of the tree. `wall_self` is what is
left when the direct children are subtracted: the span's own work plus any
stretch inside it that no child accounted for. Summing self times across the
siblings at one level partitions that level's time, and `run`'s self time is
the command's unaccounted-for remainder.

The subtraction is the sum of the direct children's *inclusive* times, so it
assumes children do not overlap. Sequential phases satisfy that; a span that
ever ran concurrently with a sibling would make its parent's self time an
under-estimate, clamped at zero rather than reported negative.

**`-` means nothing measured that column, not zero.** The three trailing
columns can show it. The CPU column is `-` on any span whose mode does not
record CPU time (the **four** `wpa.component.*` spans), and the peak and delta
columns are `-` on any span that has no memory window — which today is those
same four, plus every bearing span on a run with no memory samples. The wall
and self columns always carry a duration. A *measured* zero prints as a value:
`0ns` for a duration, `0 B` for a byte count. The distinction is deliberate
and is the subject of
[section 6](#6-the-two-rules-that-make-absence-visible).

**Rows are ordered by name, not by execution time.** Siblings are sorted
alphabetically so that two runs of the same pipeline render identically, which
means a reader looking for the order phases ran in should follow the pipeline
description in the design spec (§4.6) rather than the row order.

Three further blocks follow the tree, in this order. Each is omitted rather
than zeroed when nothing was recorded, so an absent block means *not measured*,
not *measured zero*:

- the per-component distribution block — for each distributed span, one line
  naming the span in full (`wpa.component.execute p50 1.9ms · p95 14.2ms ·
  p99 41.0ms · max 2.31s`), then a `slowest:` line with the occurrences
  retained by `--metrics-top-n` (spec §4.3);
- the component totals — `WPA components: <expected> expected, <reused>
  reused, <executed> executed`. `expected` is the sum of the per-kind expected
  counts the run recorded; `reused` and `executed` are the component loop's own
  counters. It is omitted when no per-kind count and neither counter was
  recorded (spec §6.4);
- `Store:` — row counts per published table and byte sizes per store file,
  then `Cross-check:` — a count the pipeline held in memory against the same
  count read back from the store (spec §4.7).

## 3. `cpu_inclusive` and its one caveat

`cpu_inclusive` is the **process** CPU delta across the span's window: every
thread's work in that window, not just the thread inside the span. Its value is
**per-phase** attribution — which phase inside the run consumed the CPU — and
that is what a process-wide figure cannot give: a run-level CPU number says
the run was CPU-bound but never says where the CPU went. It is *not* there to
close a run-level wall-versus-CPU gap, which on a single-threaded CPU-bound run
is small: round 3's record puts wall minus CPU at 0.07–1.24 s across the runs
of its post-round series. And it is the wrong number to read as "this span's
own cost" if the codebase ever becomes multi-threaded.

Today the distinction is academic: VERITAS is single-threaded at the component
level, the Soufflé executor rejects `limits.threads != 1`, and the only other
thread is the memory sampler, which does no CPU work worth counting. If
concurrency is added, this field must be renamed or re-derived and its
documented meaning updated (spec §11.3). Until then, read it as "process CPU
during this span", which is what it says.

CPU is also recorded only on spans that keep an interval, never on the
13,716-iteration component loop: `getrusage` costs microseconds against the
tens of nanoseconds of a clock read, and paying it per component would
contaminate the measurement it is there to take (spec §7.3). That is why
those rows show `-`.

## 4. The span inventory

A phase that runs without a span produces **no row**. That is the right
failure mode — a missing row rather than a wrong one — but it is only
*visible* to a reader who knows the row should have been there. The list below
is the full set a successful run should produce; if one is missing, the
instrument lost a site, and the time it covered is silently inside its
parent's self time.

The list is maintained by hand. A new phase added without a span will not
appear (spec §11.4).

| Span | Opened in |
| --- | --- |
| `run` | `src/tools/veritas-build.cpp`, wrapping the whole command |
| `cli.ingest` | `src/tools/veritas-build.cpp` — resolve, load manifest, write `manifest.json` |
| `m1.ingest` | `src/analysis/ProjectAnalyzer.cpp` — the analyzer re-resolves and re-loads |
| `m4.local_analysis` | `ProjectAnalyzer.cpp` — `RunLocalAnalysis` |
| `m5.svf` | `ProjectAnalyzer.cpp` — `SvfAnalysisStage::Analyze` |
| `m5.svf.module_set` | `src/analysis/svf/SvfSession.cpp` — `buildSVFModule` |
| `m5.svf.svfi` | `SvfSession.cpp` — `SVFIRBuilder::build` |
| `m5.svf.andersen` | `SvfSession.cpp` — `createAndersenWaveDiff` |
| `m5.svf.svfg` | `SvfSession.cpp` — `buildFullSVFG` |
| `m5.svf.map_facts` | `SvfSession.cpp` — the callback into `MapSvfFacts` |
| `m5.model_bundle_load` | `ProjectAnalyzer.cpp` |
| `m5.merge_svf_facts` | `ProjectAnalyzer.cpp` — `MergeSvfFactsV2` |
| `m6.cpg_projection` | `ProjectAnalyzer.cpp` — `BuildThinCpg` |
| `m2m3.publish_summaries` | `ProjectAnalyzer.cpp` — persist context + publish summaries and CPG |
| `wpa.graph_build` | `src/wpa/WpaOrchestrator.cpp` — call/SCC graph and the frozen expected set |
| `wpa.orchestrate` | `WpaOrchestrator.cpp` — the component loop, children included |
| `wpa.component.materialize` | `WpaOrchestrator.cpp` — distributed |
| `wpa.component.cache_lookup` | `WpaOrchestrator.cpp` — distributed |
| `wpa.component.execute` | `WpaOrchestrator.cpp` — distributed |
| `wpa.component.canonicalize` | `WpaOrchestrator.cpp` — distributed |
| `wpa.scc_state_flush` | `WpaOrchestrator.cpp` — the run's convergence-state commit |
| `facts.batch_assemble` | `ProjectAnalyzer.cpp` — `MakeAnalysisFactBatch` |
| `facts.store_open` | `ProjectAnalyzer.cpp` — `FactStore::Open` |
| `facts.publish` | `ProjectAnalyzer.cpp` — `AnalysisFactBus::Publish` |
| `facts.publish.validate` | `src/facts/AnalysisFactBus.cpp` — `Validate` |
| `facts.publish.sink.fact-store` | `AnalysisFactBus.cpp` — the `FactStore` sink, named for its sink id |

The nesting is worth noting because it is not the same as the file boundary:
`wpa.graph_build` and `wpa.scc_state_flush` are siblings of `wpa.orchestrate`
under `run`, not children of it. Three of the `facts.*` spans likewise hang
directly off `run` rather than under the WPA spans that precede them in wall
time — `facts.batch_assemble`, `facts.store_open` and `facts.publish` — while
the remaining two, `facts.publish.validate` and `facts.publish.sink.fact-store`,
are children of `facts.publish`: `AnalysisFactBus::Publish` opens both inside
the `facts.publish` span.

The four `wpa.component.*` spans are **distributed**: they run once per
component, so instead of one row per occurrence they report one aggregate row
plus a percentile block. The four component spans hang under
`wpa.orchestrate`, and repeated occurrences fold into a single node by
name path: `wpa.component.execute` is one node with a `count` of however many
components ran, not one node per component (spec §4.2).

**A note on `m5.svf.andersen`.** SVF self-reports its own Andersen timing. The
span measures the call directly, so the two numbers should agree in magnitude
without agreeing exactly. Where they diverge, the span is the measured one:
no SVF timing is captured into a VERITAS type today (spec §4.6).

## 5. The memory join

A sampler records `(t_since_run_start, rss_bytes, footprint_bytes)` on a timer
— 250 ms by default. It knows nothing about spans. Spans record their
`[start, end]` interval, and the **report** joins the series to those
intervals afterwards. Post-hoc joining is what makes nested spans get correct
peaks, and it keeps the sampler independent of the analysis (spec §4.5).

The join rules:

- **Both ends of the window are inclusive.** A sample exactly at a span's
  `t_start` belongs to the span, and so does one exactly at `t_end`.
- `rss_start` and `rss_end` are the oldest and newest in-window samples,
  `peak_within` is the maximum in-window sample, and `delta` is
  `rss_end − rss_start` — signed, because a phase can release memory.
- For a span that ran more than once, the window is the **union** of its
  occurrences, `[first_start, last_end]`. The figures describe that union, not
  any single occurrence and not a per-occurrence average.
- Physical footprint is recorded alongside the resident set, because on this
  fixture the difference is large: much of what `/usr/bin/time -lp` calls peak
  is memory the kernel may already have reclaimed (spec §4.5).

Two bounds on the series are worth knowing when you read it:

- The buffer holds 65,536 samples. On overflow it does **not** ring-buffer —
  a ring would discard the early curve, which is where the SVF burst lives.
  It drops every other retained sample and doubles `series_decimation`. The
  capacity is unchanged: it is the *retained* series that halves, so the buffer
  then refills to capacity and thins again, and again, keeping the whole
  timeline at coarser resolution rather than losing its start. The first and
  newest samples survive each thinning. The factor is written into the artifact;
  a decimation above 1 means the series is thinner than the sample interval
  suggests. It doubles until it saturates at the largest value the field holds,
  never wrapping to 0: `series_decimation: 0` would read as "never thinned",
  which is the opposite of what it would mean.
- **The `run` row excludes the report's own rendering and writing.** The
  `run` span closes before either the report or the artifact is produced, so
  the number the whole report is anchored to is the analysis command's time and
  not that time plus the cost of describing it.

## 6. The two rules that make absence visible

This is the feature's reason for existing. A missing measurement must read as
missing rather than as a zero, because an all-zero block is indistinguishable
from a measured zero, and a difference of zero is indistinguishable from
"unchanged" — the exact comparison failure an artifact meant for byte-diffing
must not have (spec §4.5, §6.3).

**Rule 1 — the `memory` block is present only when something was measured.**
It applies at both levels:

- a **span's** block is present only when at least one sample landed inside its
  window;
- the **run-level** block, including its `peak`, is present only when the
  series holds at least one sample.

With no series — which is what `--metrics-interval-ms 0` produces — or with a
window that no sample lands in, the key is **absent**, never zeroed. A present
block therefore means "measured", and its figures are real. A run with no
sampler at all carries no `memory` key in `run-metrics.json`.

The presence test is "was it measured", never "was it emitted". Those are
different questions: `--metrics-series false` decides whether the samples are
*published*, not whether they were collected. Keying presence on the published
copy would make the artifact contradict itself, since the per-span blocks
would still carry real measured figures beside a run level that claimed no
measurement — and the peak would be silently lost (spec §6.3). So a run with
the series suppressed still carries `memory.peak` and `series_decimation`, and
no `series`.

**Rule 2 — an `identity` field with no value omits its key entirely.** `""` is
a *value*: an empty `wpa_config_hash` compares equal against a run that was
configured differently and reads as "unchanged", which is precisely the
comparison failure this artifact exists to prevent. In practice all nine
fields are populated on any successful run, because a WPA failure returns
before the report is built; the omission rule is the guard for the case where
that stops being true (spec §6.3).

Only two of those nine fields, though, are *run-scoped*: `run_id` and
`batch_id`. The other seven — `repository_id`, `revision_id`,
`build_variant_id`, `projection_id`, `svf_config_hash`, `wpa_config_hash` and
`engine_toolchain_identity` — are content- and config-derived, so two runs of
one fixture agree on every one of them. Excluding the whole block to calm a
diff therefore throws away exactly the configuration comparison the block
exists for: two runs whose `wpa_config_hash` values differ did *not* have the
same effective configuration. Exclude the two run-scoped keys, not the block
(section 8, rule 6).

The same discipline shows up in the text report as the `-` in
[section 2](#2-the-columns), and in the JSON as keys that appear only on the
spans they describe: `cpu_inclusive_ns` only on interval-bearing spans,
`distribution` and `top_n` only on distributed spans. It applies to **numeric**
inventory fields too, and section 9 lists the three where the key's absence is
today the whole report: an unproduced count is omitted, because `0` is a value
that diffs as "unchanged".

## 7. The stderr prefixes

Every metrics line on stderr carries one of two prefixes. They are two
different claims about the run:

| Prefix | Means | Effect on the artifact |
| --- | --- | --- |
| `veritas-build: metrics note:` | a field was **never produced** — its producer did not run | none; `complete` is unchanged |
| `veritas-build: metrics degraded:` | something **failed**, or the artifact contradicts itself | `"complete": false`, plus an entry in `diagnostics` |

Notes are expected on a healthy run — today three of them appear on every run
(section 9) — while a degradation is not. The two are told apart on stderr
because an operator needs that distinction at a glance. The split exists so
that a run full of unfilled fields does not wear out the words that have to
mean something on the run where the write really did fail.

**The artifact does not carry that distinction.** Both kinds are entries in one
`diagnostics` array, and `complete` is a single top-level boolean: it tells you
whether the run had *any* degradation, not which entries are notes. On a run
with both kinds — the only run where the question arises — you cannot classify
the entries from the artifact at all; the per-entry discriminator is the stderr
prefix, so a script that wants the classification must read stderr.

One degradation can never be classified from the artifact even in principle.
The unwritable-artifact case below is appended to `diagnostics` *after* the
artifact has been rendered and written, and the artifact is not rendered again,
so that `degraded` line cannot appear inside an artifact — only on stderr.

Two checks are enough to judge a run: no `metrics degraded:` line on stderr,
and `"complete": true` in the artifact. The absence of the artifact means
something else again, and has three causes: `--metrics false`; an analysis that
failed before the report was built; or an artifact path that could not be
written. The last is reported on stderr and does not change the exit code,
because by that point the summaries, facts, CPG, and manifest are already
durably committed and failing the command would report completed work as
failed. Metrics never fail the analysis (spec §7.1).

## 8. The diffability contract

These six rules are the contract that makes `run-metrics.json` diffable. They
fix ordering and representation, not values: timings, counters, `run_id` and
`batch_id` all still move, so the `diff` of two real runs is non-empty by
design. What the rules guarantee is that everything in it is a real difference
rather than a re-ordering or a float's text form. They are recorded so that a
future instrument does not quietly break them (spec §6.5); changing one is an
artifact-format change, not an internal detail.

1. JSON object keys are sorted; counters are sorted by name, so instrumentation
   order cannot leak into the artifact.
2. Top-N entries are ordered by `(wall_inclusive desc, label asc)`. The explicit
   label tiebreak makes the artifact reproducible when two durations collide.
3. Durations are integers — nanoseconds, never floating point. A float's text
   form is a diff-noise and portability hazard. The text report performs the
   human conversion.
4. **No absolute paths anywhere in the artifact.** `project_root` is
   deliberately excluded, and `manifest.json` already carries it. That does
   *not* make it the only machine-scoped content: the `environment` block is
   machine-scoped throughout — `os`, `arch`, `cpu_model`, `cores`, `ram_bytes`,
   `build_type`, `host_compiler`, `veritas_version`, `git_revision` — so a
   script that wants cross-machine diffs must exclude that block as well as the
   run-scoped coordinates of rule 6. Where a store read-back failure would have
   quoted a path, the artifact describes the failure instead.
5. The memory series lives in its own block, so `--metrics-series false` yields
   a calm diff.
6. The `identity` block is separable, so a comparison script can exclude the
   **run-scoped** coordinates without parsing the rest of the artifact. Exclude
   them selectively: only `run_id` and `batch_id` move between two runs of one
   fixture (section 6, rule 2).

## 9. Fields reported as not recorded today

Three inventory fields have no producer that can fill them. They are not
measurements, and the artifact says so in the strongest form it has: **the key
is absent.** Key absent means "not measured"; a present key means "measured" —
the same mechanism [section 6](#6-the-two-rules-that-make-absence-visible)
describes for `memory`, applied to a numeric field. It has to be the key,
because `0` is a *value*: a script diffing two artifacts would otherwise read
`svfg_edges: 0` in both and conclude it was unchanged, when in fact neither run
measured it.

| Field | Why it is empty |
| --- | --- |
| `inventory.output.svfg_edges` | the accessor exists and is reachable, but reads a field nothing writes |
| `inventory.incrementality.summaries_recomputed` | no producer anywhere in the pipeline |
| `inventory.incrementality.summaries_reused` | no producer anywhere in the pipeline |

Each still carries its `metrics note:` line on stderr, so the absence is named
as well as visible (spec §6.3).

> **This is the contract a current artifact implements.** The three keys are
> absent, and the `metrics note:` lines above are a named second signal rather
> than the only one. An artifact produced before the omission landed carries the
> three keys at `0`, and for those, stderr alone distinguishes "not measured"
> from "measured zero".

`svfg_edges` is the instructive one. A source appeared to exist:
`SVFG::getTotalEdgeNum()` is reachable through `SVFG → VFG → GenericGraph`,
so an earlier revision of the design instructed the SVF session to emit it.
It would have put a structurally-zero figure in the artifact in place of a
measurement — a falsehood indistinguishable from a measured zero, and so the
precise reading this design exists to prevent. The accessor reads
`GenericGraph::edgeNum`, and nothing increments that field for a VFG/SVFG:
`incEdgeNum()` is called only from SVF's CDG and statement code, while
`VFG::addVFGEdge` maintains only the endpoint nodes' edge lists. The
implementer checked it against a real graph — 0 reported against 24 nodes —
and reported rather than substituting a plausible number.

The general rule: **verify that a source carries the value, not merely that it
exists.** Checking that an accessor is reachable is not checking that anything
writes what it reads.

The same discipline applies to counters generally. Output-scale counts
(`svfg_nodes`, `rooted_input_facts`, `canonical_facts`, the component reuse and
execute counts, the per-kind expected counts) travel as recorder counters added
at the producing site, so a counter that was never recorded leaves its report
field at zero **and** records a note naming it. There is no second plumbing
path that could supply a plausible default. These fields differ from the three
above in that a producer exists and normally runs: for them the note, not the
key's absence, is what marks the difference between a measured zero and an
unfilled field, because the key is present either way.

## 10. The flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--metrics true\|false` | `true` | record and report; `false` restores the pre-change behaviour exactly, with no thread |
| `--metrics-interval-ms N` | `250` | sampler period |
| `--metrics-top-n K` | `10` | slowest occurrences retained per distributed span |
| `--metrics-series true\|false` | `true` | publish the memory series array |
| `--metrics-path <path>` | `<output>/run-metrics.json` | override the artifact location |

Two of these have a meaning that is easy to get wrong:

- **`--metrics-interval-ms 0` disables the series entirely** — no sampler
  thread and no samples. It does *not* mean "sample at span boundaries". A
  zero interval asks for no sampling at all, so the `memory` block is absent
  rather than zeroed (section 6), while per-span wall, self and CPU time are
  still recorded because those need no sampler. Span-boundary sampling is a
  degradation path reachable only through a `pthread_create` failure, and it
  sets `complete: false` when it happens (spec §7.4, §8.4).
- **`--metrics-series false` suppresses only the curve.** `memory.series` is
  omitted; `memory.peak` and `series_decimation` remain, because the peak is
  the report's headline memory figure and suppressing the curve is not a
  reason to withhold it.

The flag spelling is `--flag value` and not `--no-flag`: the argument parser
rejects any value beginning with `-`, so `--metrics false` is the only
well-formed spelling of the negative (spec §6.2).

The library default is different from the CLI default. `AnalyzeProject`'s
recorder parameter defaults to `nullptr`, so a caller that does not ask for
metrics gets no thread, no clock reads, and no behaviour change (spec §6.2).

## 11. What the report does not show

Being explicit about this is part of the instrument's contract:

- **No target and no verdict.** The design sets no wall-time, CPU, or memory
  objective, and the report claims none. The overhead budget (≤0.5 % CPU and
  ≤0.05 GiB peak resident, worst of three runs, spec §7.3) is a ceiling on the
  instrument, not a performance result.
- **No cross-run comparison.** No baseline, no trend store, no gate. Two
  artifacts and a script are the supported workflow.
- **No live view.** The report is emitted after the run.
- **Nothing reads the artifact back.** It is written by the CLI after
  `AnalyzeProject` returns, and `observability` ships no analysis-side reader,
  so "the metrics changed the result" is not a thing that can happen quietly.
- **The duplicated ingest is measured, not repaired.** The CLI resolves and
  loads the project, then the analyzer resolves and loads it again; that is why
  both `cli.ingest` and `m1.ingest` exist. The duplication is a finding the
  instrument is expected to show, and fixing it is a separate change (spec
  §10).
- **No golden file with real timings** in the test suite, which would be
  brittle by construction. The versioned-schema test carries the contract
  instead, and the overhead figure is established by measurement and recorded
  in the specification's verification record rather than asserted in CI
  (spec §8.3, §8.5).
- **The store block is the newest coupling and can degrade alone.** A schema
  drift or a locked database omits the `store` block and leaves the rest of the
  artifact intact, with a diagnostic (spec §4.7, §7.1).

## Normative references

- [`veritas-build analyze` phase observability design specification](../specs/veritas-build-analyze-phase-observability-design-spec.md)
- [Round-3 performance design specification](../specs/veritas-build-analyze-round3-performance-design-spec.md) — the measurement method this instrument replaces, and the failure mode it exists to prevent
- [Generating and inspecting SummaryDB](summarydb-generation-manual.md) — the pipeline whose phases the report names
