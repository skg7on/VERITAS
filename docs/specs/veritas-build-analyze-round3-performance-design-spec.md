# `veritas-build analyze` Round 3 Performance Design Specification

**Status:** Draft for review

**Extends:**
[`veritas-build-analyze-performance-design-spec.md`](veritas-build-analyze-performance-design-spec.md)
(the round-1/round-2 spec). This design reopened two of that spec's section 5
non-goals and dropped both on measurement, so the round as it lands reopens
none of them; section 5 below names both and section 9.6 records the drops.

**Tracking issue:** [#136](https://github.com/skg7on/VERITAS/issues/136), the
successor to [#133](https://github.com/skg7on/VERITAS/issues/133), which remains
open.

**Motivating command:**

```bash
./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb
```

## 1. Purpose

Rounds 1 and 2 removed avoidable overhead from the summary index, digest
rendering, WPA result ownership, and fact publication. They reduced peak memory
from 9.5 GB to 6.85 GiB and left wall time near 600 s. Both acceptance limits
were still missed by a wide margin: 375 s and 4 GiB.

This round re-profiles the same command on current `main` and attacks what the
measurement actually shows. The dominant costs are not in the paths rounds 1
and 2 optimized. They are:

1. a filesystem round trip inside every WPA component execution;
2. a fresh compiled Soufflé program instantiated per component — **listed here
   by reading the code, then measured at 0.037 % of wall time and dropped; see
   sections 3.2, 7.2, and 9.6**;
3. redundant re-derivation of fact identity across validation and publication;
4. per-component SQLite transactions, one commit each;
5. a string-keyed comparison sort over 1.38M witness edges — **kept for the
   memory it removes, not for time: 6.8 s of a 421 s CPU run, section 3.5**;
6. a resident set that never returns the memory it abandons — **refuted on
   measurement and dropped; there was no abandoned memory to return, sections
   3.6, 7.6, and 9.6**;
7. a retained component payload held from each component's execution until
   batch assembly — **refuted on measurement and dropped: the retained set is
   ≈60 % of the published facts and the reload cost +109 s CPU and +0.30 GiB,
   sections 7.7 and 9.6**.

Every item this round keeps produces no semantic content. This round removes
them without trading accuracy, and without weakening any validation. Two of the
seven are the exception that proves the rule, and both were settled the same
way — by measuring the quantity before building the remedy: item 2's
construction is too small to be worth its correctness risk (section 3.2), and
items 6 and 7's costs are not costs at all (sections 3.6, 7.7). Three of this
round's seven suspected root causes were refuted on measurement; section 9.6
records all four refutations, including section 3.5's, as the round's headline
finding.

## 2. Measured baseline

### 2.1 Measurement provenance

Four runs of the motivating command were taken in a dedicated worktree on the
current development machine, Debug configuration, host compiler
`/opt/homebrew/opt/llvm@17/bin/clang++` (clang 17.0.6) against LLVM 24.0.0git
libraries, each with a fresh output directory under `/tmp` and `/usr/bin/time
-lp`. Binary: fresh clean build of `62bc573`.

| Run | Instrumentation | Wall | Max RSS |
| --- | --- | ---: | ---: |
| 1 | RSS every 5 s, 5 stack samples | 587.11 s | 7.84 GiB |
| 2 | RSS every 5 s, 8 stack samples | 576.86 s | 7.89 GiB |
| 3 | RSS every 30 s, 18 stack samples | 583.57 s | 8.55 GiB |
| 4 | RSS every 3 s, no stack samples | 572.82 s | 8.56 GiB |

**Comparability caveats.** Max RSS varies by 0.72 GiB across four runs of
byte-identical inputs, so the peak should be treated as roughly 8.2 ± 0.4 GiB,
not a point value. None of these runs reproduces the 6.85 GiB recorded in the
round-2 spec section 9.5.1, and the gap is not explained. The LevelDB checkout
is also not pristine: it carries a modified `CMakeLists.txt` and untracked
`compile_commands.json` and `.veritas/` entries, and this build reports 187,055
SVFG nodes and 246,002 edges where round-2 section 2 recorded 187,139 and
246,104.
Absolute values should therefore be read as same-machine, same-fixture
measurements, and only comparisons within this document are load-bearing.

### 2.2 Wall-time distribution

Stack samples from run 3, eight seconds each, classified by the dominant
function on the main thread's heaviest path:

| Window | Duration | Phase | Dominant frames |
| --- | ---: | --- | --- |
| 0–105 s | ~105 s (18%) | ingest, local summaries, SVF | `SvfAnalysisStage::Analyze`; SVF self-reports 20.30 s Andersen and 32.61 s MemorySSA |
| 105–360 s | ~255 s (44%) | WPA component loop | `WpaOrchestrator::Run`, then `SouffleWpaExecutor::Execute` (19–28%), then `RelationIo::ReadOutput` (9–13%) |
| 360–395 s | ~35 s (6%) | batch assembly | `MakeAnalysisFactBatch` — 70 % of the main thread **in that one 5 s sample**; the whole ordering step measures **6.8 s of a 421 s CPU run**, 1.6 % (section 3.5) |
| 395–583 s | ~190 s (33%) | publication | `AnalysisFactBus::Publish`, `Validate` (46%), `FactStore::Publish` (47%) |

Leaf functions by self samples inside those windows:

- WPA loop at t=135 s and t=165 s: `__open_nocancel`, `__unlinkat` — the
  per-component temporary-directory traffic.
- WPA loop at t=195–345 s: `veritas::core::MakeStableId`,
  `veritas::core::ComputeSHA256`, `StableId::operator<=>`, and
  `std::__compressed_pair`/`std::__tree` — identity derivation and map building
  during input materialization.
- Publication at t=500 s: `SHA256Hasher::Update` (12.4%), `MakeStableId`,
  `StableId::operator<=>`, `std::reverse_iterator<std::byte*>`.
- Publication at t=555–570 s: `guarded_pwrite_np` (16.6%),
  `fsync` (8.3%), `pread` (8.0%) — real file I/O from the SQLite and
  content-addressed stores.

The percentage figures come from a parser over the macOS `sample` tree output
and sum above 100% across depths, so they are indicative ranks, not exact
shares. The phase boundaries carry roughly ±15 s of uncertainty at a 30 s
sampling interval. The named hot functions reproduced identically across runs 1,
2, and 3.

### 2.3 Resident-set distribution

Run 4, clean, 3 s granularity:

| Elapsed | RSS | Phase |
| ---: | ---: | --- |
| 30 s | 0.89 GiB | ingestion, local summaries |
| 60 s | 1.18 GiB | local summaries completing |
| 78 s | 3.39 GiB | **+1.66 GiB burst in one interval** — SVF construction |
| 120 s | 3.52 GiB | SVF complete; floor established |
| 361 s | 6.92 GiB | WPA payload accumulating across 13,716 components |
| 545 s | 8.56 GiB | **peak — during publication** |
| 572 s | 6.69 GiB | release at exit |

Three attribution facts follow.

**The peak is late, not mid-run.** The round-2 spec section 9.5.4 holds that the
peak occurs "at about 60% of wall time, while WPA results accumulate and before
any row is written" and that "nothing that reduces assembly intermediates,
duplication, or statement overhead can move it." Run 4 puts the peak at 545 s of
572.82 s — 95% of wall time, inside publication — and shows the resident set
climbing a further ~1.5 GiB after batch assembly began. Assembly intermediates
and publication working set are therefore both peak components, contrary to
round-2 section 9.5.4.

**The SVF floor was read as partly dead memory — that reading was wrong, and
section 3.6 records the refutation.** `vmmap -summary` against the live
process at t≈180 s reports a physical footprint of 2.1 GiB against 3.5 GiB of
resident writable regions, with 0 K swapped. The region table attributes
**703.7 MiB resident and only 18.8 MiB dirty to `Malloc Small (empty)`
regions**. That row was taken here as allocator arenas retaining 685 MiB of dead
pages rather than returning them to the OS, and **it is not**: the `resident`
column was read where `dirty` was meant, and `free()` had already released those
pages — freeing 800 MiB of small-zone allocations in an isolated control dropped
the physical footprint 787 → 292 MiB at an unchanged 787 MiB resident set
(section 3.6). `Malloc Small` shows a further 2.3 GiB resident against 1.7 GiB
dirty by the same reading. **There is no reclaimable share of the post-SVF floor
to design against, and the ~0.7 GiB this paragraph once offered is not
available.**

**The floor is real regardless.** After the t=78 s burst, RSS never returns
below 2.95 GiB for the remainder of the run. Any design for a 4 GiB peak must
account for a ~3.4 GiB post-SVF floor, and the measurement of this round says
none of it is reclaimable dead memory: the round's two reductive memory designs
were both dropped on measurement (section 9.6), and the worst-of-three peak is
8.5945 GiB against a 4 GiB criterion (section 9.4). What the gap between resident
set and physical footprint does mean is recorded in section 3.6: 1.42–1.45 GiB of
the peak `/usr/bin/time -lp` reports is memory the kernel may already take back.

### 2.4 Published content and the equivalence baseline

The published tables were counted and dumped directly from the run-4 store, so
these figures are this round's own measurement rather than inherited from
round 2:

| Table | Rows |
| --- | ---: |
| `analysis_facts` | 1,249,792 |
| `run_fact_bindings` | 752,076 |
| `provenance_nodes` | 752,076 |
| `provenance_edges` | 1,375,911 |

`wpa_component_states_v2` holds exactly 13,716 rows, confirming the component
count independently of the tool's own summary output.

The row counts equal those round-2 section 9.5.2 recorded. Stronger evidence is
available for one table: `analysis_facts` carries no run-scoped identity, and
dumped in `fact_id` order from this round's run it hashes to
`452a850ec90ab192…`, **byte-identical to round 2's published digest** for the same
table at the same row count. The published fact set has therefore not changed
between round 2's build and this one.

The remaining three tables carry the run id and its dependents, which move with
the toolchain identity, so their digests cannot be compared across builds of the
Soufflé functor library. Section 9.1 defines the instrument that handles both
cases; the exclusion set it once held as an unconfirmed assumption was
**determined** by the acceptance task and is recorded there, with Step 2b as the
applicable form.

## 3. Root causes

### 3.1 Every WPA component execution round-trips through the filesystem

`SouffleWpaExecutor::Execute` (`src/wpa/SouffleWpaExecutor.cpp:53`) performs, per
component: `mkdtemp` a fresh directory, `RelationIo::WriteInput` writing one
`.facts` file per EDB relation (including empty ones, because the compiled
bundle's `.input` directive loads each unconditionally), `veritas_souffle_run`
invoking `runAll(input_dir, output_dir, performIO=true)`, `RelationIo::ReadOutput`
parsing the result `.csv` files through `std::getline` and `SplitRow`, and a
recursive `remove_all`.

That is 13,716 directory creations, 13,716 recursive deletions, and a full
serialize/parse round trip of every EDB and IDB row. The samples place
`RelationIo::ReadOutput` at 9–13% of the main thread during the WPA window, with
`__open_nocancel`, `__unlinkat`, and `fsync` as the top leaves.

### 3.2 A compiled Soufflé program is instantiated per component

**Measured and negligible. This cost is not removed: section 7.2 is dropped.**

`veritas_souffle_session_open` (`src/wpa/SouffleRunner.cpp:186`) calls
`ProgramFactory::newInstance(program_name)` on every component execution, as
`veritas_souffle_run` (`src/wpa/SouffleRunner.cpp:155`) does on the file-backed
path. Each call builds a fresh program object with its own relation tables,
symbol table, and interpreter state, then `delete`s it. Construction cost is
paid 13,716 times to produce 13,716 independent evaluations.

That 13,716 multiplier is why this section was filed as a root cause, and it is
why the cost had to be measured rather than inferred: a small per-call cost
looks large when multiplied by the component count. It is not. Timing
`open`+`close` — which is exactly `newInstance`, `setNumThreads`, and `delete`,
and therefore the whole quantity a reuse design would remove — 4,000 times per
component kind, in the baseline's own Debug configuration, gives:

| Component kind | Registered program | `open`+`close` |
| --- | --- | ---: |
| reachability | `v2_reach` | 10.9 µs |
| memory-effects | `v2_memory_effects` | 19.4 µs |
| flow | `v2_global_flow` | 14.8 µs |
| effects | `v2_effects` | 18.5 µs |

The mean is **15.9 µs**. Against a measured **0.40 ms** per component execution,
the fixed cost is 2.6–2.8 % of the smallest component this repository can build,
and **0.218 s** across all 13,716 instances — **0.037 %** of the 587 s baseline
and **0.10 %** of the ~212 s gap to the 375 s acceptance limit.

The cost is also **workload-independent**, which is what makes 2.6 % a ceiling
rather than an average: the four bundles are build-time artefacts generated from
`logic/`, so the same four programs are instantiated whatever repository is
analyzed. Timing a synthetic reachability component of `n` call edges confirms
it — the fixed column is flat to within 7 % while the full session path grows
~800×:

| n edges | Derived rows | `open`+`close` | Full session path | Fixed share |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 4 | 10.7 µs | 79.8 µs | 13.4 % |
| 64 | 64 | 10.8 µs | 946.6 µs | 1.1 % |
| 512 | 512 | 10.8 µs | 7.91 ms | 0.14 % |
| 4096 | 4096 | 11.4 µs | 65.3 ms | 0.017 % |

Round 3 therefore leaves this cost in place and section 3.1's removal of the file
round trip stands on its own, which is the outcome section 10 risk 2 anticipated.
The measured figures are recorded as this round's implementation status in
section 9.6, so the acceptance report can cite the decision rather than report a
deliberate non-build as an omission.

### 3.3 Fact identity is re-derived once per fact and twice per witness edge

`AnalysisFactBus::Validate` (`src/facts/AnalysisFactBus.cpp:289`) is a genuine
integrity check and is retained in full. It begins by recomputing
`DeriveBatchId(batch)` — a hash over every fact and witness row in the batch —
and then calls `DeriveFactId` once for each of the 752,076 derived facts (line
319 — the `batch.facts` loop, which is not the 1,249,792 rows of
`analysis_facts`, a count that also carries their rooted inputs) and **twice for
each of 1,375,911 witness edges**, once for the result row (line 348) and once
for the input row (line 352). That is **3,503,898** SHA-256 computations over
encoded rows in this one pass, and **9,561,990** across the process, which
derives the same rows again in publication.

The same rows were already derived during canonicalization, and `FactStore`'s
publication path derives them again. No derivation result is carried forward:
each is computed, compared, and discarded.

`DeriveFactId` in turn reaches `AppendCell` (`src/facts/AnalysisFact.cpp:206`),
which grows a `std::vector<std::byte>` one `push_back` at a time. At `-O0` this
is the single heaviest leaf in the publication window: 249 of 6,140 samples sat
in `std::vector<std::byte>::push_back` beneath `AppendLenPrefixed` alone.

### 3.4 Each component commit is its own transaction

`WpaRunRepository::StoreSuccessfulComponent`
(`src/wpa/WpaRunRepository.cpp:566`) serializes the result, puts it to the
content-addressed store, then opens a SQLite transaction, executes two
`INSERT OR IGNORE`/`INSERT OR REPLACE` statements, and commits. Run once per
component, that is 13,716 commits and 13,716 durability barriers. The `fsync`
leaf visible at t=570 s is this path.

### 3.5 Witness ordering keys are two heap strings per edge

**The structure is real; the magnitude first recorded here was not, and
measurement refuted it. Section 7.5 survives for the memory it removes, not for
the time.**

`MakeAnalysisFactBatch` (`src/facts/AnalysisFactBus.cpp:170`) builds a
`KeyedWitness` per surviving edge carrying `result_key` and `input_key` as
`std::string`, encoded through `AppendSemanticKey`. Across 1,375,911 edges that
is ~2.75M string allocations held simultaneously, and the vector is then sorted
with a comparator over `std::tie(result_key, rule_id, input_key, input_ordinal)`
— lexicographic string comparison through a 1.38M-element comparison sort.

**Measured share: the whole ordering step is 6.8 s of a 421 s CPU run — 1.6 %,
not 70 %.** The 70 % figure was one five-second stack sample at t=360 s projected
onto the whole run, and a projection is not a measurement; the same projection
appears in section 2.2's "70% of main thread at t=360 s" row. After section
7.5's packed ranks land, ~3.15 s of comparison work remain, so the design is
worth **≈3.65 s of CPU** — and it was kept because of the memory it removes
(peak RSS **−0.249 GiB**, CPU-neutral end to end), not because of that.

The same allocation is the largest single contributor to the memory step
between the WPA window and batch assembly: run 3 shows RSS rising 1.12 GiB
across the 345 s → 375 s boundary, immediately before the sort completes.

### 3.6 Abandoned memory is not returned — **Refuted on measurement**

**Refuted (2026-09-25). There is no reclaimable dead memory to return, and
section 7.6 is dropped. The revert is `8227176`.**

This section read the `Malloc Small (empty)` row of `vmmap -summary` — "703.7 MiB
resident against 18.8 MiB dirty at t≈180 s" — as allocator arenas retaining
~685 MiB of dead pages. **It misread the `resident` column, and the column that
means "actually held" says 18.8 MiB.** `free()` had already released those pages:
in an isolated control, freeing 800 MiB of small-zone allocations dropped the
physical footprint 787 → 292 MiB **while the resident set stayed at exactly
787 MiB**, so `resident` is the column that does not move when pages become
reclaimable and `dirty` is the column that does. The same gap is present in situ
at every boundary of a real run — post-SVF 2,999 MiB resident against 1,920 MiB
footprint, post-publication 3,534 / 2,445, post-WPA 7,143 / 2,722 — and in the
spec's own capture, 3.5 GiB of resident writable regions against a 2.1 GiB
physical footprint.

Two consequences outlive the design and belong to the round's record:

- **Nothing was available to reclaim, and no platform call can reclaim it.**
  Three in-situ calls to `malloc_zone_pressure_relief(nullptr, 0)` released
  **0 MiB on both instruments at all three boundaries, in two runs**, and the
  API's own return value was **0 bytes** in situ and in three isolated controls
  (800 MiB small-zone, 800 MiB large-zone and 200 MiB tiny-zone allocations
  freed, including an explicit 1 GiB goal and a direct call on
  `malloc_default_zone()`). The zone vtable entry is present and the API is
  documented on this SDK, so it is neither a missing symbol nor a wrong call:
  the call is a no-op for this process's heap on Darwin. `malloc_trim(0)` — the
  glibc branch, never exercised on this machine — was not the load-bearing half.
- **The acceptance criterion counts memory the kernel may already take back.**
  At the peak, physical footprint is 1.42–1.45 GiB below `/usr/bin/time -lp`'s
  maximum resident set size, and section 9.4's criterion is the latter. That gap
  is a real part of this round's 4 GiB miss and it is not memory pressure;
  section 9.4's attribution should say so.

## 4. Goals

1. Execute each WPA component through the compiled program's in-memory
   relations, with no intermediate directory, file, or CSV text.
2. **Dropped (2026-09-25).** Instantiate each component's compiled program once
   per run, not once per component, resetting relations between components.
   Section 3.2 measured the cost this goal would remove at 0.218 s — 0.037 % of
   the baseline and 0.10 % of the gap to the acceptance limit — and the reset it
   needs carries section 10 risk 2, whose failure mode is an accuracy trade that
   section 5 forbids. The goal text is retained rather than renumbered so the
   drop stays visible; the design is dropped in section 7.2.
3. Derive each distinct fact identity once per validation pass and once per
   publication pass, without deleting or weakening any check.
4. Commit component cache state in batches rather than once per component.
5. Order facts and witnesses with keys whose comparison is an order-isomorphism
   of the current string comparison, at a fraction of the memory and with no
   per-edge allocation. **Retained, and for memory only.** The comparison step it
   replaces measures 6.8 s of a 421 s CPU run, so the time it buys is ≈3.65 s;
   it earns its place on peak RSS (**−0.249 GiB**), and it is CPU-neutral end to
   end (section 3.5).
6. **Dropped (2026-09-25).** Release provably dead memory at phase boundaries.
   There is no such memory on the acceptance platform: the `resident` column was
   read where `dirty` was meant, and `malloc_zone_pressure_relief` released
   **0 bytes at every call** and is a no-op on Darwin (section 3.6). The design is
   dropped in section 7.6 and reverted in `8227176`; the goal text is retained
   rather than renumbered so the drop stays visible.
7. **Dropped (2026-09-25).** Reduce retained component payload during the WPA
   window. The retained set measures **748,647 of 1,249,792 published facts, ≈60 %**
   — not the "small fraction" the design assumed — and the reload it needs costs
   **+109 s CPU (+13.8 % instructions retired) and +0.30 GiB peak RSS**
   (section 7.7). Dropped and reverted in `deaead8`, with the goal text retained
   rather than renumbered.
8. Complete the motivating command within 375 s and at no more than 4 GiB peak
   resident set on the reference machine and fixture. **Missed: the memory
   criterion is missed by 2.15× and the wall criterion is not measurable in this
   environment and is provably missed on CPU grounds (section 9.4).**

## 5. Non-goals

This design reopened two of round-2 section 5's non-goals, designed both,
measured both, and dropped both (section 9.6). **The round as it lands reopens
none of them, and keeps every other non-goal.**

**Reopened, then dropped on measurement.** Both items below were brought into
scope because the 4 GiB acceptance criterion cannot be met without them, and
neither survived its measurement:

- *releasing memory after SVF completes* — goal 6, designed in section 7.6:
  measured at **0 bytes released at every boundary**, because the ~685 MiB it
  targeted had already been released by `free()` and the relief API is a no-op on
  Darwin (section 3.6); reverted in `8227176`.
- *not retaining WPA payloads until assembly* — goal 7, designed in section 7.7
  and also excluded by round-2 section 9.5.4: the retained set measured at
  **≈60 % of the published fact set**, not the small fraction the design assumed,
  and the change cost **+109 s CPU and +0.30 GiB** (section 7.7); reverted in
  `deaead8`.

SVF's *analysis* is untouched. Round-2 section 5's "Do not change SVF
construction, Andersen analysis, MemorySSA, or SVFG construction" is **not**
reopened; only the lifetime of memory SVF has already allocated is — and with
both of the items above dropped, nothing this round keeps changes that lifetime
either.

**Still excluded, and this round must not change them:**

- No accuracy trade of any kind. Epistemic states, fact sets, witness selection,
  and the set of executed `(SccId, WpaComponentKind)` components are unchanged.
- No weakening of `Validate`, `ValidateSemanticRow`, or any identity check. The
  redundancy in section 3.3 is removed by memoization and cheaper encoding, never
  by deleting a check.
- No change to SVF analysis configuration, `summary.v2`, `relations.v2`, the
  typed relation registry, rule bundles, model bundles, or fact identity.
- No batching of multiple SCCs into one Soufflé execution, and no parallel
  component execution. Dropping goal 2 does not relax this: it forbids merging
  components into one evaluation. Had goal 2 been built, it would have reused
  *one program object* across sequential components and still not merged them.
- No new CLI flag and no process-global cache.

## 6. Preserved contracts

1. Every expected `(SccId, WpaComponentKind)` remains independently materialized,
   cached, executed, canonicalized, and published, in the same
   reverse-topological order.
2. `LogicalInputHash`, `FixpointHash`, `ExternalHash`, and the canonical
   `BatchId` are byte-identical for unchanged input.
3. Canonical fact and witness order, canonical-owner selection among duplicate
   facts, and the single retained witness derivation per fact are unchanged.
4. Published table contents are byte-identical. Section 9.1 defines the
   instrument.
5. Duplicate facts are still owned by the first component in canonical
   component-key order.
6. `AnalysisFactBatch` continues to carry exact expected/completed component
   equality, rooted inputs, finite witnesses, diagnostics, and a
   content-addressed `BatchId`.
7. Error paths continue to use `Status` and `StatusOr`; no RTTI and no
   exceptions (`.claude/rules/cpp-compilation-policy.md`).

## 7. Design

### 7.1 In-memory component execution

The vendored Soufflé at the pinned revision exposes the interfaces this needs:
`SouffleProgram::getRelation(name)`, `Relation::insert`, relation iteration,
`Relation::purge`, `SouffleProgram::purgeInputRelations`,
`purgeOutputRelations`, `purgeInternalRelations`, `setNumThreads`, and a
`run()` that performs no I/O. All are declared in
`third_party/Souffle/src/include/souffle/SouffleInterface.h`.

`SouffleWpaExecutor::Execute` is reimplemented to:

1. Insert every `input.edb` row into its relation by name, using the dense
   `uint64_t` and symbol forms `relations.v2.dl` declares, instead of writing
   `.facts` files.
2. Invoke the program with I/O disabled.
3. Iterate each derived relation named by `ComponentDomains(component)` and
   rebuild `facts::SemanticRow` values through the existing dense-to-stable
   mapping, replacing the CSV text path.
4. Iterate the witness relation to rebuild `facts::WitnessEdge` values, replacing
   the `RowFromKey`-per-cell path.

No temporary directory is created, no file is written, and no text is parsed.
`RelationIo`'s text encoding and decoding are no longer on this path; whether the
file-backed implementation is retained for any other caller is decided during
implementation, and if it becomes unreachable it is deleted.

Because the `.input` directive's "every EDB relation must have a file" constraint
disappears with the files, empty relations no longer need a placeholder write.

**The C ABI boundary is load-bearing and must be respected.** `souffle::tuple`,
`souffle::Relation`, and iteration over them require RTTI and exceptions.
`souffle/SouffleInterface.h` is included by exactly one translation unit,
`src/wpa/SouffleRunner.cpp`, which is compiled with them enabled and reached
through the C ABI in `SouffleRunner.h`; VERITAS builds with both disabled
(`.claude/rules/cpp-compilation-policy.md`). The in-memory path therefore cannot
be implemented in `SouffleWpaExecutor.cpp`, which is RTTI-free.

The runner's C ABI is extended into a session interface — open a component
session, insert a relation's rows, run, scan a relation's rows out, close — with
all Soufflé types confined to the runner's side and only flat scalar buffers
crossing the boundary. (`reset` was part of that signature while section 7.2 was
in the design; it exists as a stub returning `kNotImplemented` and is not used.
See section 7.2.) `SouffleWpaExecutor` then owns the translation between
`facts::SemanticRow` values and those buffers, and keeps its existing dense-ID
mapping and `ValidateSemanticRow` calls. The plan fixes the exact signatures; this
section fixes the constraint that no Soufflé type may appear in them.

### 7.2 One program instance per run — **Dropped**

**Dropped (2026-09-25), ratified on measurement. Not built.**

Section 3.2 measured the quantity this design removes — `newInstance`,
`setNumThreads`, and `delete`, which is exactly one `open`+`close` pair — at
10.9–19.4 µs per component, 2.6–2.8 % of the smallest fixture component's 0.40 ms
execution and **0.218 s** across all 13,716 instances: 0.037 % of the 587 s
baseline and 0.10 % of the ~212 s gap to the 375 s acceptance limit. It is also
workload-independent, because the four bundles are build-time artefacts.

The risk trade is unfavourable at that size. Accepting 0.218 s means accepting
section 10 risk 2, whose reset semantics are unverified in this pinned revision,
and the failure mode is silent rather than loud: a reused session whose EDB
relations are neither purged nor replaced presents a previous component's rows
to the next component, which derives facts from them. That is an accuracy trade,
and section 5 forbids accuracy trades outright. A design that must not fail
silently in order to save 0.037 % of wall time is not worth building.

The design is recorded rather than deleted, so that the measurement which
rejected it stays attached to it:

> `veritas_souffle_run` gains a run-scoped entry point that obtains one program
> instance, runs it for each component, and resets state between components via
> `purgeInputRelations`, `purgeOutputRelations`, and `purgeInternalRelations`.
> The existing one-shot entry point may remain as a thin wrapper; the executor
> uses the scoped form.
>
> The reset must be proven to restore a state indistinguishable from a fresh
> `newInstance`. Section 9.3 makes this a differential test rather than an
> assumption.

**State left behind.** `veritas_souffle_session_reset`
(`src/wpa/SouffleRunner.cpp`) remains the documented stub returning
`kNotImplemented`. No session is reused: every component execution opens and
closes its own, so session memory is released per component today and section 7.6
has nothing to add here. Section 7.1's in-memory execution is unaffected and
stands. Section 9.3 is dropped with this section, and the reuse bullet in section
9.2 is dropped with it.

One follow-up belongs to whoever next touches that file: the stub's comments
(`src/wpa/SouffleRunner.cpp:302` and `:48`, `include/veritas/wpa/SouffleRunner.h:62`
and `:128`) still attribute the function to "Task 3", and its `kNotImplemented`
return is still annotated as temporary. After this drop it is not pending work
but a deliberate standing decision. The comments should say so, and this round's
documentation-only scope did not include editing them, so the spec records the
discrepancy here rather than leaving a reader to reconcile the two.

### 7.3 Memoized fact identity in validation and publication

`Validate` keeps every check it performs today. Two changes remove the
redundancy:

1. **Memoize row identity within the pass.** A row's fact identity is a pure
   function of the row, so a single `row → fact_id` memo serves the per-fact
   loop and both endpoints of every witness edge. Distinct rows across the batch
   are the 1,249,792 rows of `analysis_facts` — the 752,076 derived facts plus
   their rooted inputs; the witness endpoints are drawn from exactly that set.
   The memo therefore collapses the 3,503,898 derivations section 3.3 counts to
   those 1,249,792 distinct rows, with no change to any comparison performed.
2. **Make the encoding cheap.** `AppendCell` and `AppendLenPrefixed` reserve the
   encoded length once and write into the buffer directly, instead of appending
   a byte at a time. `DeriveFactId` keeps deriving from the row rather than
   trusting a caller-supplied identity, so the check retains its meaning.

`FactStore`'s publication path derives identities for the same rows. Where it
re-derives a value the batch already carries and has already validated, it
consumes the carried value; where it independently needs an identity it derives
it once.

### 7.4 Batched component cache commits

`StoreSuccessfulComponent` currently commits per component. Component cache
state is a cache: its loss costs a recomputation, never a semantic result. This
round accumulates cache and state rows into a batcher and commits every N
components with a final flush before the run completes, using the existing
`BulkInsertBatcher` where its statement shape fits.

**This changes a durability window and the design must say so plainly:** a crash
mid-batch loses the cached results of the components in that batch, which are
recomputed on the next run. It does not lose facts, provenance, or the run
receipt, and it does not change what a completed run publishes. The content
addressed result put stays per component, so the objects a committed cache row
references are always present.

### 7.5 Order-preserving packed ordering keys

Replace the `std::string` keys of `KeyedFact` and `KeyedWitness` with dense
ranks over the distinct encoded semantic keys.

Construction: encode each distinct semantic key once, sort the distinct set by
byte order, and assign each a `uint32_t` rank in that order. A `KeyedWitness`
then carries `(result_rank, rule_rank, input_rank, input_ordinal)` as four
`uint32_t`s, and a `KeyedFact` carries its rank.

The ordering claim is exact. The current comparator is
`std::tie(result_key, rule_id, input_key, input_ordinal)`, compared
lexicographically, with string comparison byte-wise through
`std::char_traits<char>`. Ranking each component by ascending byte order is an
order-isomorphism on that component's distinct values, so rank-lexicographic
order is identical to string-lexicographic order, ties included. `std::unique`
afterwards removes the same elements, so `batch.facts` and `batch.witnesses`
hold the same values in the same order as before, and `DeriveBatchId` over them
is unchanged.

Consequence: the ~2.75M simultaneous strings and the per-comparison string
dereference both disappear, and the observed ~1.1 GiB step at the assembly
boundary with them.

**Retained for memory, not for time — measured.** Peak RSS fell **−0.249 GiB**
across this design's own before/after pair, and end-to-end CPU is neutral. The
comparison work it replaces is the 6.8 s of section 3.5, of which ~3.15 s
remains, so it is worth ≈3.65 s of a 421 s CPU run. It is kept because the
round's peak-memory problem is real and this is the one reductive change that
survived; a reader who weighs 3.65 s against 0.249 GiB should read section 3.5
first, where the 70 %-of-wall-time figure that once justified it is refuted.

### 7.6 Memory relief at phase boundaries — **Dropped**

**Dropped (2026-09-25), ratified on measurement. Built, measured, reverted in
`8227176`. Not in the tree.**

Section 3.6 measured the quantity this design would return and found **zero**:
three in-situ calls released **0 MiB on both instruments at all three
boundaries, in two runs**, and `malloc_zone_pressure_relief`'s own return value
was **0 bytes** in situ and in three isolated controls after freeing 800 MiB of
small-zone, 800 MiB of large-zone and 200 MiB of tiny-zone allocations, including
with an explicit 1 GiB goal and called directly on `malloc_default_zone()`. The
API is present and documented on this SDK, so this is not a wrong call — it is a
no-op for this process's heap on Darwin. The ~685 MiB the design targeted is
memory `free()` had already released, visible as a resident-versus-footprint gap
rather than as withheld memory.

There is no effect to keep, so the design is recorded rather than retained, and
the measurement that rejected it stays attached to it:

> Introduce one narrowly-scoped helper that asks the platform allocator to
> return emptied arenas — `malloc_zone_pressure_relief(nullptr, 0)` on Darwin,
> `malloc_trim(0)` elsewhere — and call it at the boundaries where the pipeline
> knows a large working set has just become dead:
>
> 1. after the SVF stage completes and its results have been extracted;
> 2. after WPA result payloads have been consumed;
> 3. after publication completes.
>
> This changes no data structure and no result. The measured basis is the
> `Malloc Small (empty)` region: 703.7 MiB resident against 18.8 MiB dirty at
> t≈180 s. The call is cheap and its effect is directly measurable as an RSS step
> at each boundary, which is how section 9.4 accepts it.

**State left behind.** Nothing. `git diff 0ae4ec6 8227176` is empty, so the tree
is byte-identical to the pre-task revision: `include/veritas/core/AllocatorRelief.h`,
`src/core/AllocatorRelief.cpp` and `tests/unit/core/AllocatorReliefTest.cpp` do
not exist, and no source, header or CMake file references `ReleaseFreedMemory` or
`AllocatorRelief`. The suite count returned 812 → 811 by exactly the one test
that was removed. Section 9.4's resident-set-step clause is moot with this
section and is marked so there.

**What this round keeps instead.** The measurement did leave one finding worth
carrying: at the peak, physical footprint is 1.42–1.45 GiB below the maximum
resident set size that section 9.4 measures, so part of the 4 GiB miss is memory
the kernel may already take back. It is recorded in section 3.6 and section 9.4,
and it is not a design.

### 7.7 Retained component payload — **Dropped**

**Dropped (2026-09-25), ratified on measurement. Built, measured, reverted in
`deaead8`. Not in the tree.**

The design was implemented, proven content-identical on the whole workload
(§9.1 Step 2a, the strong form, all four digests equal under both orderings — the
reload publishes the same bytes), and then measured. **It is a regression on both
axes, so it comes out:**

| Instrument | Before → After | |
| --- | ---: | --- |
| CPU (user+sys) | 429.77 → **538.78 s** | **+109.01 s** |
| instructions retired | 7.34e12 → **8.35e12** | **+13.8 %** (load-independent) |
| peak RSS | 7.99 → **8.29 GiB** | **+0.30 GiB** |
| peak footprint | 6.02 → 6.72 GiB | +0.70 GiB |

Neither run thrashed (`swaps 0`, CPU at 98.8 % and 98.7 % of wall), so the CPU
sign is not a swap artefact, and the instruction counter is a per-process
hardware count that contention cannot produce. The RSS sign alone would be
under-determined against this fixture's run-to-run spread; the CPU sign is not.

**The premise was measured and it is false.** The design says the retained subset
is "a small fraction of a component's facts and none of its witnesses". It is
**748,647 of the 1,249,792 published facts — ≈60 %** (`GlobalFlow` 225,223 +
`MayRead` 213,027 + `MayWrite` 206,849 + `ReachableCall` 76,316 + `UnknownEffect`
27,232, by `SELECT relation_name, COUNT(*) FROM analysis_facts GROUP BY
relation_name`), because the retained relations are the components' own derived
relations. Most facts therefore stay resident, and what is actually released is
the witnesses — paid for by `LoadReusableComponent`'s **full revalidation**,
every fact identity re-derived and both canonical hashes recomputed, 13,716
times. That is where the +109 s and the +1.01e12 instructions come from. The
prescribed fallback (retain facts, reload only witnesses) pays the same
per-component revalidation while releasing less, so it cannot be better on either
axis and was not built.

**Two hazards the design created, recorded because they are properties of the
design and not of any fix.** Both are mooted by the revert; neither is work to
keep.

1. **The one-argument `MakeAnalysisFactBatch` silently published an *empty*
   batch for a run whose payloads had been released, and `AnalysisFactBus::Validate`
   accepted it.** An empty batch has no witness to fail its closed-witness check
   and its expected/completed component sets still match, so the run would have
   reported success having published no facts at all. It was caught by the
   *existing* `WpaFactBusHandoffTest`, not by the new work — which is the point:
   the design moved a whole run's failure mode from "wrong content" to "no
   content, no error", and only an unrelated test stood in the way.
2. **The guard against it was an `assert`, so `NDEBUG` would have lost it
   entirely.** The release flag and the documented contract would have survived a
   Release build; the check would not. A design that needs a runtime guard to
   avoid publishing nothing, and can only afford a debug-only one, carries
   structural risk independent of its measured cost.

The design is recorded rather than deleted, so the measurement that rejected it
stays attached to it:

> Round-2 section 9.5.4 identified this and excluded it. It is required for the
> 4 GiB goal and is included now.
>
> The obstacle is real and must not be designed around carelessly: canonical
> owner selection among duplicate facts requires processing components in
> canonical component-key order, while execution must remain reverse-topological.
> Those two orders differ, so assembly cannot begin until every component has
> executed, and `SuccessorSupport` may read any completed component's facts until
> the last predecessor has run.
>
> The design therefore does not fold components into the batch as they complete —
> that would change owner selection. Instead:
>
> 1. A completed component's full result is written to the content-addressed
>    store as it is today, so it is durable and reloadable.
> 2. In memory, the orchestrator retains only the subset `SuccessorSupport` can
>    actually consume: rows whose relation is the `derived` relation of a domain
>    carrying `support.has_value()` for any component kind. This is a small
>    fraction of a component's facts and none of its witnesses.
> 3. During assembly, each component's result is reloaded from the
>    content-addressed store through the existing `LoadReusableComponent`
>    deserialization and revalidation path, and released again once its facts and
>    witnesses have been keyed.
>
> This preserves execution order, failure isolation, component caching, owner
> selection, every hash, and every published row, because assembly consumes the
> same bytes that were stored.
>
> **Cost, stated plainly:** 13,716 content-addressed reads plus deserialization
> move into the assembly window, trading wall time for memory. This round is
> constrained on both. The mitigation is that section 7.1 removes far more
> per-component wall time than these reads add, and section 9.4 measures both
> limits together rather than one at a time. Section 7.2 was listed here as a
> second mitigation until it was dropped on measurement, so this trade rests on
> section 7.1 alone.

**State left behind.** Nothing. `git diff 8227176 deaead8 --stat` is empty, so
the revert is exact and the ten files are restored byte-for-byte: no reference
to `ComponentReloader`, `ReloadStoredComponent`, `component_payloads_released` or
`AssembleBatch` survives in `src`, `include`, `tests` or `tools`. The two tests
the design added (`ReloadedAssemblyMatchesRetainedAssembly`, in
`WpaOrchestratorTest` and `AnalysisFactBusTest`) are gone with it, and section
9.2 records their removal rather than promising them. The M9 handoff test is back
to its pre-change form, which is the form that catches the empty-batch hazard
above when a design reintroduces it.

## 8. Error handling

- An in-memory relation lookup for an expected relation name returns
  `Internal("compiled program has no relation <name>")`.
- A dense id in a derived relation that the input mappings cannot resolve keeps
  returning the existing `InvalidArgument`/`FailedPrecondition` from the mapping
  `ToStable` paths.
- A program reset that leaves observable state behind is a conformance failure,
  not a silent fallback: the differential test in section 9.3 must fail. Section
  7.2 is dropped, so no reset exists and this rule guards nothing today; it is
  kept because it states the standard any future reuse must meet.
- Reloading a component result during assembly that fails deserialization or
  revalidation returns the existing `FailedPrecondition` from
  `LoadReusableComponent`; it is not retried and not silently skipped. Section
  7.7 is dropped, so assembly does not reload and this rule guards nothing today;
  it is kept because it states the standard any future reload must meet.
- Batched cache commits preserve the existing rollback behaviour: a failed
  transaction rolls back its batch and the run marks incomplete through the
  orchestrator's existing failure path.
- Allocator relief is best-effort and never fails a run. Section 7.6 is dropped
  and reverted, so there is no relief call in the tree and this rule guards
  nothing today; it is kept with the same reading as the two bullets above.

## 9. Verification strategy

### 9.1 The equivalence instrument

Published content is compared through ordered dumps of the four published tables,
hashed with `sqlite3 … "SELECT * FROM <table> ORDER BY …" | shasum -a 256`. Both
the projection and the ordering are determined by measurement rather than
assumed, and the determined form is the second table below: `analysis_facts`
reproduces under `fact_id` **and** under `rowid`, while the three identity-bearing
tables require `rowid` — the physical order the batch hands to `FactStore`, which
is the stronger instrument.

| Table | Ordering | This round | Round-2 section 9.5.2 |
| --- | --- | --- | --- |
| `analysis_facts` | `fact_id` | `452a850ec90ab192…` | `452a850ec90ab192…` — **equal** |
| `analysis_facts` | `rowid` | `6b0aea6381242e30…` | — |
| `run_fact_bindings` | all columns | `f297dcf25ac551dc…` | `bab0a696647ffb25…` |
| `provenance_nodes` | all columns | `a3b8b0f71efcf07e…` | `387d299985baf5e8…` |
| `provenance_edges` | all columns | `3b38fc4b7e5a1712…` | `da4829f280ade0b8…` |

The last three rows of that table are the **pre-determination attempt**: every
column included, under an ordering this section did not record, and their
mismatch with round 2 is what made the exclusion set a hypothesis. The determined
projection below supersedes them for every acceptance comparison; they are kept
as the record of what was tried.

The first row is a real result: `analysis_facts` is the only one of the four
carrying no run-scoped identity — its columns are `fact_id`, `relation_name`,
`cells_hex` — and under the correct ordering it reproduces round 2's published
digest **exactly**, at the same 1,249,792 rows. The published fact set has
therefore not changed between round 2's build and this one.

The other three tables carry `run_id`; `run_fact_bindings` also carries
`analyzer_run_id` and `selected_witness_id`, and the provenance tables carry
`witness_id`. The run id incorporates the toolchain identity, which incorporates
the compiled Soufflé functor library's hash (round-2 section 9.5.3), so those
columns move between any two builds of that library and no projection of them
matches across such a pair.

**The exclusion set is determined, not hypothesised — the caveat this section
carried is closed.** The list above was a hypothesis and none of its projections
reproduced round 2's digest. The acceptance task then determined the instrument
by reproducing the recorded digests under each candidate projection, and the
result is below. A set/digest mismatch is exactly what this section exists to
prevent, so the determined form is stated in full rather than summarised:

| Table | Exclusion | Ordering | Digest (equal in both stores) |
| --- | --- | --- | --- |
| `analysis_facts` | **none** | `fact_id` **or** `rowid` | `452a850ec90ab192a2e5cde28269067f06f4618338675c26c870cac466e41cda` / `6b0aea6381242e301ddfb993c7048b213037f9aafc18c7acd3e23ac894ab8ed1` |
| `run_fact_bindings` | `run_id`, `analyzer_run_id`, **`binding_id`** | `rowid` | `d732ec43ca21a5967170139f69f3688145c2771b7a079b78ef7b26a6eddf3b63` |
| `provenance_nodes` | `run_id` | `rowid` | `d8410e270ed19257491a830e9debb666299be54365482b83236ac06cbacdf321` |
| `provenance_edges` | `run_id` | `rowid` | `6691f96f5b5f137e29f9ce0aeb4ddf4380c2da4f15ec3041a6f5344daa98a038` |

Three properties of it are load-bearing and each was checked, not assumed:

- **`analysis_facts` needs no exclusion at all, and reproduces under both
  orderings.** Equality under `rowid` as well as `fact_id` says the published
  fact *set* and the physical insertion order the batch hands to `FactStore` are
  both unchanged; `rowid` is the stronger instrument for that reason, and it is
  the ordering the three identity-bearing tables use.
- **`run_fact_bindings` requires `binding_id` excluded *and* `rowid` ordering.**
  Keeping `binding_id` gives `11b357b279e251d7607092e0fc0b0f40109676779b2753f3ca60fe460f0358c9`
  and ordering by `fact_id` gives `c52d97bea12d97b5…`; neither reproduces the
  recorded digest. This is the correction the acceptance task made to the list
  the dispatch carried, and it matters for the record even though the verdict is
  insensitive to it.
- **`provenance_nodes` requires `run_id` excluded *and* `rowid` ordering.**
  Ordering by `output_fact_id, witness_id` gives `31567c611093d076…`.

The identity columns this section listed as stable — `witness_id`,
`selected_witness_id`, `producer_id`, `summary_id`, `source_anchor_id` — were
held **included** throughout and no mismatch arose, so witness identity and
provenance structure are confirmed stable across the round. `binding_id` is
stable too, which is why the abbreviated set is harmless for this verdict even
though it does not reproduce the recorded hash.

The comparison proceeds in two steps.

**Step 1 — did the toolchain identity move?** Compare the published
`engine_toolchain_identity` before and after. Every edit in this design except
the encoding change of section 7.3 should leave it untouched, since only files
compiled into the functor library feed it. **In this round it moved**:
`souffle-e4135d90a5f5d329…` → `souffle-0ef51c2207f7a5aa…`, which is why the
acceptance comparison is Step 2b and not Step 2a.

**Step 2a — identity unchanged.** Require all four digests equal under the
orderings above, and require `BatchId`, `FixpointHash`, `ExternalHash`, and
`LogicalInputHash` equal directly. **Not achievable across revisions, and
superseded: the run id binds the provenance digest of the linked binaries, so
`run_id` and `engine_toolchain_identity` move between any two revisions built in
this Debug configuration — no projection that keeps those columns can match
across them.** It remains the applicable form for a pair built in the same tree,
which is how the section 7.7 before/after pair qualified.

**Step 2b — identity moved, and the applicable form for this round.** Require
`analysis_facts` equality, equal row counts for all four tables, and equality of
the identity-bearing tables under the **determined** projection above. Sections
7.1 and 7.5 claim no *row* can change; this form tests exactly that while
conceding that identity columns legitimately move. Dropping sections 7.2, 7.6 and
7.7 removes claimants, not the requirement.

The pre-change member of the pair is produced by building the pre-change revision
in the same build tree, as round 2 did, so the comparison is not confounded by
the compiler. Section 9.6 records the verdict.

### 9.2 Unit and integration tests

- A materializer/executor test proves in-memory execution and file-based
  execution produce identical `RawWpaEvaluation` row sets for a fixture
  component, and identical `LogicalInputHash`, `FixpointHash`, and
  `ExternalHash`.
- **Dropped with section 7.2.** A test proving one reused program instance
  across two sequential components yields the same results as two fresh
  instances, including the case where the first component is non-empty and the
  second's relations must start empty. There is no reused instance to test.
  Noted for the record: that last case is the one that makes the drop the safe
  outcome rather than merely the cheap one, because a reused session's EDB
  relations may be neither purged nor replaced.
- `AnalysisFactBusTest` proves the packed-rank ordering reproduces the string
  ordering, including ties on `result_key` with differing `rule_id`, differing
  `input_key`, and differing `input_ordinal`, and including the `std::unique`
  boundary.
- **Removed with sections 7.6 and 7.7 — no longer promised by this section.**
  `ReloadedAssemblyMatchesRetainedAssembly` (in `WpaOrchestratorTest` and
  `AnalysisFactBusTest`) proved that reloaded assembly produced the same batch as
  retained assembly; it was added by `5128e32` and removed by the revert
  `deaead8`. `AllocatorReliefTest` covered the relief helper's
  nothing-to-release path; it was added by `c03ddd2` and removed by the revert
  `8227176`. Neither exists in the tree, the suite count returned to its
  pre-change size, and the M9 handoff test (`WpaFactBusHandoffTest`, exercised
  through `WpaEndToEndTest`) is back to its pre-change form — which is the form
  that caught the empty-batch hazard the dropped design created (section 7.7).
  Their absence is the acceptance task's confirmation that the reverts are
  complete, not a coverage gap: they tested paths that are no longer in the
  tree.
- A validate test proves the memoized derivation performs the same number of
  checks and rejects the same tampered batches as before, by mutating a fact id,
  a witness result row, and a witness input row in turn.
- `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts` and the
  `wpa-qualification` differential corpus must pass unchanged, and the M9 entry
  gate must report all ten criteria passing.
- No test may be skipped; a `GTEST_SKIP` reports as passed to CTest, so the
  existing no-skips check remains mandatory.

### 9.3 Program-reset differential test — **Dropped**

**Dropped (2026-09-25) with section 7.2.** There is no reset to test.

Recorded for the record, because it is the test the drop avoided having to pass.
Section 7.2's reset would have been accepted only by measurement: for a set of
components including one that derives no facts following one that derives many,
the reused instance must produce results byte-identical to fresh instances, and a
component run twice on a reused instance must produce identical results both
times. The section 9.6 drop record cites how much wall time that test was
guarding.

### 9.4 Performance acceptance

Both limits are measured together, on the reference machine and fixture, with
`/usr/bin/time -lp` against a fresh output directory:

- wall time at most 375 s;
- maximum resident set size at most 4 GiB.

~~Additionally, because section 7.6 claims a reclaimable floor, the run must show
a measurable resident-set step at each allocator-relief boundary, recorded in the
spec's implementation-status section.~~ **Moot: section 7.6 is dropped and
reverted in `8227176`, so there is no boundary, no relief call and no step to
require.** For the record, the clause's own rule — "a relief call that releases
nothing is a finding to report, not a success to assume" — is exactly what
happened: **0 MiB on both instruments at all three boundaries in two runs, and 0
bytes from the API** (section 3.6, section 7.6). No acceptance clause depends on
it now.

Three consecutive runs are recorded, since section 2.1 shows 0.72 GiB of
run-to-run spread on identical inputs; the acceptance claim uses the worst of the
three. The runs below were taken on `deaead8`; section 9.6 records them.

**Acceptance verdicts.**

- **CPU — the round's measured and only gain.** Worst of three post-round runs is
  **421.67 s** (421.67 / 416.95 / 415.54) against **573.93 s** worst of three
  pre-round on unmodified `62bc573` (573.93 / 569.80 / 567.49): **−152.26 s,
  −26.5 %** (−26.7 % against the true worst pre-round run, 575.10 s).
- **375 s wall gate — NOT MEASURABLE in this environment, and provably missed.**
  The host carried a standing ~6.0–6.2 GiB of swap (6,157 MiB of 7,168 MiB used)
  before, during and after the series, so no wall figure taken here can be
  certified against the limit and none is quoted as a gate result. The gate is
  nevertheless **missed on CPU grounds alone**: CPU time is a lower bound on wall
  time for any process, and 421.67 s exceeds 375 s, so the wall time could not
  have met the gate whatever the swap conditions. A certified wall number
  requires an unswapped machine. (These particular runs were in fact 99.7 %
  CPU-bound — wall minus CPU was 0.07–1.24 s — so the recorded walls are not
  swap wait either; they are simply not usable as a gate measurement.)
- **4 GiB memory gate — MISSED, by 2.15×.** Worst-of-three peak RSS is
  **9,228,288,000 B = 8.5945 GiB** against the 4,294,967,296-byte criterion, an
  excess of 4.5945 GiB.
- **Round memory objective — NOT met.** Worst-of-three peak RSS moved
  **8.5625 → 8.5945 GiB (+0.032 GiB, +0.37 %)**, far inside the 0.72 GiB
  run-to-run spread this fixture shows. That is neither a regression nor an
  improvement, and it is the expected outcome with sections 7.6 and 7.7 dropped.
  Attribution should carry section 3.6's footprint-versus-RSS gap: 1.42–1.45 GiB
  of the miss is memory the kernel may already take back, and the criterion
  counts it.

### 9.5 Pre-push verification

Per `.claude/rules/pre-push-verification-policy.md`: clean build from scratch,
full CTest suite with no skips, `git diff --check`, license-header check, clean
working tree. This round's changes are C++ and CMake, so the license-header
check is in scope for every modified file.

### 9.6 Implementation status (2026-09-25)

This subsection records decisions the acceptance report must cite, so that a
design deliberately not built is not later read as an omission.

**1. Section 7.2 (one program instance per run) — dropped, ratified on
measurement. Not built.** Section 3.2 filed "a fresh compiled Soufflé program
instantiated per component" as a root cause, on the strength of the 13,716
multiplier. The multiplier is real; the cost is not. Measured in the baseline's
own Debug configuration on `claude/profile-analyze-bottlenecks`, 4,000 iterations
per component kind:

| Component kind | Registered program | `open`+`close` |
| --- | --- | ---: |
| reachability | `v2_reach` | 10.9 µs |
| memory-effects | `v2_memory_effects` | 19.4 µs |
| flow | `v2_global_flow` | 14.8 µs |
| effects | `v2_effects` | 18.5 µs |

`open`+`close` is exactly `ProgramFactory::newInstance` + `setNumThreads` +
`delete`, so it is the whole quantity the dropped design would have removed, not
a proxy for it. Against a measured **0.40 ms** per component execution on the
pinned fixture, the fixed cost is **2.6–2.8 %** of the smallest component this
repository can build, and **0.218 s** across all 13,716 instances. That is
**0.037 %** of the 587 s baseline (section 2.1) and **0.10 %** of the ~212 s gap
to the 375 s limit.

Two properties of the measurement decided the outcome, and both were checked
rather than argued:

- **It is workload-independent.** The four bundles are build-time artefacts
  generated from `logic/`, so the same four programs are instantiated whatever
  repository is analyzed. Timing a synthetic reachability component shows the
  fixed column flat to within 7 % (10.7 → 11.4 µs) while the full session path
  grows ~800× (79.8 µs → 65.3 ms) from 4 to 4,096 edges. The 2.6 % figure is
  therefore a ceiling, not an average.
- **The comparison is like-for-like.** The 587 s baseline is a Debug build with
  `/opt/homebrew/opt/llvm@17/bin/clang++` against LLVM 24.0.0git (section 2.1);
  the measurement above ran in the same configuration, confirmed against
  `build/CMakeCache.txt`, so the share is not a build-skew artefact.

The drop is a decision about risk as much as about size. Accepting 0.218 s means
accepting section 10 risk 2, whose reset semantics are unverified in this pinned
revision and whose failure mode is silent: a reused session whose EDB relations
are neither purged nor replaced hands a previous component's rows to the next
component, which derives facts from them. That is an accuracy trade, and section
5 forbids accuracy trades. Had the saving been material the trade might have been
worth making; at 0.037 % of wall time it is not.

**2. Section 7.3 (memoized fact identity) — built, measured, and the memo's memory left to
section 9.4.** Measured on the same fixture and configuration as this round's baseline: wall
**520.92 s → 428.19 s (−92.7 s, −17.8 %)**, published content byte-identical, and `DeriveFactId`
**119.33 s → 29.80 s** over **9,561,990 → 5,792,572** calls, the memo collapsing section 3.3's
**3,503,898** derivations in `Validate` to its **1,249,792** distinct rows. Of the 92.7 s, 69.1 s is the
cheaper encoding at an unchanged call count and 34.3 s is fewer derivations, less 4.8 s for the
memo's own 4,879,809 lookups and the run-to-run remainder; the memo proper is ≈ 10.7 s of it.
The memo's price is **0.68 GiB of peak RSS** against section 9.4's 4 GiB criterion, so
keep-or-drop is a section 9.4 decision re-taken on the whole workload: if the wall gate lands
with margin, dropping the memo banks the 0.68 GiB for free. The encoding half — 69 s of the
92.7 s for no measurable memory — is not droppable under any reading.

**Ruling on record 2 (2026-09-25): the memo is KEPT, and the round did not trade
memory away for it. Say this plainly, because an earlier draft of this round's
notes implied the opposite.** The 0.68 GiB is real, but it is a **delta measured
against a tree without the memo** — Task 4's own controlled A/B pair,
8,864,382,976 → 9,589,686,272 B — and not a net cost against the pre-round
baseline. On one base the chain reads baseline **8.56 GiB → 8.52 GiB after
section 7.5's ranks landed with the memo present (−0.04 GiB)**, and the
acceptance measurement agrees at the round level: worst-of-three **8.5945 GiB
against the pre-round worst-of-three 8.5625 GiB, +0.032 GiB, inside the 0.72 GiB
run-to-run spread** (section 9.4). The memo costs ≈10.7 s of CPU inside a −152 s
reduction and buys the round's largest single gain; peak memory is flat, not
sacrificed. Adding Task 4's +0.68 GiB to section 7.5's −0.249 GiB to a baseline
is **invalid arithmetic**, because those two deltas were measured against
different bases — section 7.5's before-run already contained the memo. The
decision point is closed; nothing here needs re-measuring.

**3. Section 7.4 (batched component cache commits) — built; batch size and crash window
stated plainly.** `StoreSuccessfulComponent` still writes its content-addressed result object
per component, unchanged and first, so a committed cache row always references an object the
store already holds; the cache row and the run-state row are queued and committed by
`FlushComponentCache`, which the repository calls once a batch reaches
`WpaRunRepository::kComponentCacheBatchSize` — **256 components** — and once more inside
`CompleteRun`, before the run is marked complete. A crash before that final commit therefore
costs **at most the last 256 components' cached results**, which the next run recomputes; no
published fact, no provenance edge, and not the run receipt are affected, and a completed run
publishes exactly what it published before. The removed per-component commit measured
**4,589.3 ms** in the baseline's WPA window (0.335 ms × 13,716 components, 1.08 % of a 424.79 s
wall), and batching it is **1.1 % of wall time** — smaller than this machine's run-to-run
spread, so no wall-time movement is claimed for section 7.4 in either direction; the isolated
measurement of exactly the removed work is the load-bearing evidence, not the end-to-end
total. `SccStateRepository::StoreState` commits per component in the same window at a
comparable measured cost and is deliberately **not** batched here: section 7.4's design is
scoped to the component result cache, and that repository's rows are the incremental
scheduler's convergence state, which it reads back inside its own transaction.

**4. Section 7.6 (memory relief at phase boundaries) — dropped, ratified on
measurement. Built, measured, reverted in `8227176`. Not in the tree.**
Section 3.6 filed a `vmmap` row as ~685 MiB of withheld allocator memory. It is
not: the row's `resident` column was read where `dirty` (18.8 MiB) was meant, and
`free()` had already returned those pages — an isolated control freed 800 MiB of
small-zone allocations and dropped the physical footprint 787 → 292 MiB at an
unchanged 787 MiB resident set. The design's own acceptance signal measured the
same way: **0 MiB released on both instruments at all three boundaries, in two
runs**, and **0 bytes** from `malloc_zone_pressure_relief` in situ and in three
isolated controls (800 MiB small-zone, 800 MiB large-zone, 200 MiB tiny-zone
freed, plus an explicit 1 GiB goal and a direct call on `malloc_default_zone()`).
The API is present and documented on this SDK, so it is a no-op for this
process's heap on Darwin, not a wrong call; the unverified `malloc_trim(0)`
branch is not the part that mattered. One finding survives the drop and belongs
to section 9.4's attribution: at the peak the physical footprint is **1.42–1.45
GiB below the maximum resident set size the criterion measures**, so part of the
4 GiB miss is memory the kernel may already take back. Revert verified exact:
`git diff 0ae4ec6 8227176` is empty, the three added files are gone, no source,
header or CMake file references `ReleaseFreedMemory` or `AllocatorRelief`, and the
suite count returned 812 → 811 by exactly the one removed test.

**5. Section 7.7 (retained component payload) — dropped, ratified on measurement.
Built, measured, reverted in `deaead8`. Not in the tree.**
The design is correct and content-identical (§9.1 Step 2a, the strong form: all
four digests equal under both orderings, unchanged `engine_toolchain_identity` and
`run_id`) and it is a **regression on both axes**: CPU 429.77 → **538.78 s
(+109.01 s)**, instructions retired **+1.01e12 (+13.8 %, load-independent)**, peak
RSS 7.99 → **8.29 GiB (+0.30)**, peak footprint +0.70 GiB. Neither run thrashed
(`swaps 0`; CPU at 98.8 % and 98.7 % of wall), so the CPU sign is not a swap
artefact, and the instruction counter cannot be contention. The premise is
refuted by measurement: the retained subset is **748,647 of the 1,249,792
published facts ≈60 %** (`GlobalFlow` 225,223 + `MayRead` 213,027 + `MayWrite`
206,849 + `ReachableCall` 76,316 + `UnknownEffect` 27,232), not "a small
fraction", so most facts stay resident and the reload pays `LoadReusableComponent`'s
full revalidation — every fact identity re-derived, both canonical hashes
recomputed — 13,716 times. The prescribed fallback (retain facts, reload
witnesses) pays the same revalidation for less release and was not built, which
is the right call rather than an omission. **Two hazards the design created are
recorded as properties of the design, not as work to keep:** the one-argument
`MakeAnalysisFactBatch` silently published an *empty* batch for a released run
and `AnalysisFactBus::Validate` accepted it, caught only by the unrelated existing
`WpaFactBusHandoffTest`; and the guard against it was an `assert`, so `NDEBUG`
would have lost it. Revert verified exact: `git diff 8227176 deaead8 --stat` is
empty and no reference to the dropped path survives anywhere in `src`, `include`,
`tests` or `tools`.

**What this changes elsewhere.** Section 4 goals 2, 6 and 7, section 5's reopened
set, section 7.2, sections 7.6 and 7.7, section 8's reload and relief rules,
section 9.2's reuse and reload test bullets, and section 9.3 are marked dropped
rather than deleted, and nothing is renumbered. Section 7.1 — the in-memory
execution that removed the per-component file round trip — is unaffected, is
already implemented, and is the part of section 7 this round relies on. Section
7.7 loses section 7.2 as a secondary mitigation and, being dropped itself, no
longer rests on anything. Section 7.6 needs nothing for sessions: every component
execution still opens and closes its own, so session memory is already released
per component.

**Accepted consequence.** The round's wall-time work had to find the whole ~212 s
among sections 7.1, 7.3, 7.4, 7.5, 7.6 and 7.7, and the measurement says where it
came from: section 7.3's identity work, plus the commit batching of section 7.4.
Section 3.3 — the redundant SHA-256 derivations `Validate` performs over encoded
rows — was the largest single item and is now removed; what remains of it is the
one `ComputeSHA256` per distinct row inside `MakeStableId`, which is a finding
for a future round rather than a defect in this one.

#### Round 3 final status (2026-09-25)

The round's acceptance run is the revision `deaead8`, on the reference machine
and fixture, Debug, host compiler `/opt/homebrew/opt/llvm@17/bin/clang++` against
LLVM 24.x, `VERITAS_WPA_ENGINE=souffle`. Every number below is a measurement
taken in this round and nothing is chained onto another task's delta.

**Surviving designs, and what each delivered.**

| Design | State | What it delivered |
| --- | --- | --- |
| Section 7.1 in-memory component execution (Tasks 1–2) | built | removes the per-component file round trip; the round trip measured ≈2.1 ms against a 0.40 ms component. The end-to-end effect on the 13,716-component workload is **not** separately measured and is not claimed here. |
| Section 7.3 memoized fact identity (Task 4) | built, kept | the round's one large win: `DeriveFactId` **119.33 → 29.80 s**, and `Validate`'s **3,503,898** derivations collapsed onto its **1,249,792** distinct rows. The encoding half is 69.1 s of the 92.7 s and is not droppable under any reading. |
| Section 7.4 batched component cache commits (Tasks 5 and 5b) | built | removes **4,589.3 ms** of per-component commits from the WPA window, and batches the convergence-state commits from the same window. Stated durability window: at most the last 256 components' cached results. |
| Section 7.5 packed ordering ranks (Task 6) | built, kept for memory | peak RSS **−0.249 GiB**, CPU-neutral end to end, against a comparison step measured at 6.8 s of a 421 s CPU run (section 3.5). |
| Sections 7.2, 7.6, 7.7 (Tasks 3, 7, 8) | **dropped** | records 1, 4 and 5 above. Task 3 was dropped before being built, so `edc253c` is documentation only; Task 7 and Task 8 were built and reverted, in `8227176` and `deaead8`, and both reverts are exact inverses. |

**Content identity — the round's central claim, and it holds.** A fresh
`deaead8` run against the pre-change baseline store built from `62bc573`, through
the instrument section 9.1 now determines (§9.1 **Step 2b**):

- all four published row counts equal — `analysis_facts` **1,249,792**,
  `run_fact_bindings` **752,076**, `provenance_nodes` **752,076**,
  `provenance_edges` **1,375,911**;
- `analysis_facts` **byte-identical under both `fact_id` and `rowid`**
  (`452a850ec90ab192…`, `6b0aea6381242e30…`), so the published fact set and the
  physical order the batch hands to `FactStore` are both unchanged;
- `run_fact_bindings` `d732ec43ca21a5967170139f…`, `provenance_nodes`
  `d8410e270ed19257491a830e…`, `provenance_edges` `6691f96f5b5f137e…`, all
  byte-identical under the determined exclusion set;
- the only differences anywhere are `run_id` and `engine_toolchain_identity`
  (`souffle-e4135d90…` → `souffle-0ef51c22…`), which the design predicts must
  move because the provenance digest binds the linked binaries. Neither is
  published analysis content, and `witness_id`, `selected_witness_id`,
  `producer_id`, `summary_id` and `source_anchor_id` all match while included.

**CPU — the round's measured gain.**

| Series | Runs (user+sys) | Worst |
| --- | --- | ---: |
| post-round, `deaead8` | 421.67 / 416.95 / 415.54 | **421.67 s** |
| pre-round, unmodified `62bc573` | 573.93 / 569.80 / 567.49 | **573.93 s** |

**573.93 s → 421.67 s = −152.26 s, −26.5 %.** Like-for-like alternatives agree:
best-vs-best **−26.78 %**, three-run mean **−26.71 %**, and against the fourth
pre-round run on disk (575.10 s, the true worst) **−26.68 %**. The direction and
magnitude are robust to which pre-round run is used as the reference.

**Peak RSS — the memory objective was not met.** Worst-of-three **8.5945 GiB**
against the pre-round worst-of-three **8.5625 GiB**: **+0.032 GiB (+0.37 %)**,
inside the 0.70 GiB spread this fixture shows across four pre-round runs. Peak
memory is flat — not improved, and not regressed. The 4 GiB gate is **missed by
2.15×**, and the wall gate is **not measurable in this environment and provably
missed on CPU grounds**; section 9.4 carries both verdicts and the swap state
they were taken under.

**Tests, gates and cleanliness (all at `deaead8`).** Clean build from scratch,
`rc=0`, 632/632 targets, no ccache in the toolchain; **811/811 tests pass with 0
skips**, verified against the registry's expected name set rather than the report
(a `GTEST_SKIP` reports as passed, so the summary alone is not evidence); the M9
entry gate **10/10** criteria; `git diff --check` clean; license headers clean;
working tree clean. Task 7's and Task 8's residue is **zero** — both revert pairs
are exact inverses.

**The round's headline finding: four refuted magnitudes, one failure mode.**

| Section | The claim | What measurement says | How the error was made |
| --- | --- | --- | --- |
| 3.2 | a compiled Soufflé program per component is a root cause | **0.218 s**, 0.037 % of wall | read the code, multiplied by 13,716, never timed it |
| 3.5 | witness ordering keys are 70 % of the main thread | **6.8 s of a 421 s CPU run** | one five-second stack sample projected onto the whole run |
| 3.6 | ~685 MiB of dead allocator memory is reclaimable | **0 bytes**; the API is a no-op on Darwin | misread a `vmmap` column as withheld memory |
| 7.7 | the retained support set is "a small fraction" of a component's facts | **≈60 %** of the published fact set | read the code, never measured the ratio |

The named hot functions in section 2.2's profile are **real and reproducible
across runs** — the profile is not the problem. What was wrong, every time, was
the magnitude attached to them, and it was wrong in the same direction: a cost
that looked large once multiplied by 13,716, or once a single sample was
extrapolated, was recorded as a root cause without ever being timed. Three of the
four errors were the author's, and each was caught by the task that had to
measure its own target before building it. **The instruction that caught all four
— "quantify your target before building it" — is this round's most transferable
result, and it is a standing warning that this document's root-cause magnitudes
are hypotheses until a task measures them, not findings.**

## 10. Open risks

1. **The 4 GiB limit may not be reachable even with every reductive design this
   round keeps — and it was not reached.** Four of the seven designs in section 7
   survive; sections 7.2, 7.6 and 7.7 are dropped (section 9.6), and with them go
   the round's two reductive memory designs, so the risk is now **realized**: the
   measured worst-of-three peak is **8.5945 GiB, 2.15× the limit** (section 9.4).
   Section 2.3 establishes a ~3.4 GiB post-SVF floor; the ~0.7 GiB that section
   read as provably dead at t≈180 s is **not** dead memory (section 3.6), so the
   floor is higher than this section assumed, and the residual live SVF footprint
   is still not measured and may exceed 4 GiB minus publication working set on
   its own. The honest outcome this risk anticipated is the one section 9.4
   reports: the CPU result recorded, the memory floor recorded with its
   attribution — including the 1.42–1.45 GiB of footprint-versus-RSS gap the
   criterion counts — and the 4 GiB criterion reopened as a separate decision
   rather than weakened by an accuracy trade.
2. **The program-reset semantics of the pinned Soufflé revision are not
   verified — and no longer gate anything.** `purgeInternalRelations` must
   restore a state equivalent to a fresh instance for these four programs.
   Section 9.3 would have tested it. Section 7.2 is instead dropped on
   measurement (section 3.2, section 9.6), which is the outcome this risk
   anticipated, and the file round trip of section 7.1 stands on its own. The
   risk is recorded as unresolved and unexercised rather than deleted: it is
   *why* the drop is correct, and it returns as a live blocking risk if the
   design is ever revisited, at which point section 9.3 is the first thing to
   restore.
3. **Wall time and memory pull against each other in section 7.7.** The
   reload-for-assembly trade adds reads to the window this round is trying to
   shorten.
4. **The 0.72 GiB run-to-run spread** means acceptance needs three runs and the
   machine must be quiet during them; macOS background daemons were previously
   observed to stretch this project's heaviest integration tests.
