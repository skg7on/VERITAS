# `veritas-build analyze` Round 3 Performance Design Specification

**Status:** Draft for review

**Extends:**
[`veritas-build-analyze-performance-design-spec.md`](veritas-build-analyze-performance-design-spec.md)
(the round-1/round-2 spec). This design reopens two of that spec's section 5
non-goals, named in section 5 below.

**Tracking issue:** to be opened as the successor to
[#133](https://github.com/skg7on/VERITAS/issues/133).

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
2. a fresh compiled Soufflé program instantiated per component;
3. redundant re-derivation of fact identity across validation and publication;
4. per-component SQLite transactions, one commit each;
5. a string-keyed comparison sort over 1.38M witness edges;
6. a resident set that never returns the memory it abandons.

Every one of these produces no semantic content. This round removes them
without trading accuracy, and without weakening any validation.

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
| 360–395 s | ~35 s (6%) | batch assembly | `MakeAnalysisFactBatch` (70% of main thread at t=360 s) |
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

**The SVF floor is partly dead memory.** `vmmap -summary` against the live
process at t≈180 s reports a physical footprint of 2.1 GiB against 3.5 GiB of
resident writable regions, with 0 K swapped. The region table attributes
**703.7 MiB resident and only 18.8 MiB dirty to `Malloc Small (empty)`
regions** — allocator arenas that have been fully emptied and retain 685 MiB of
dead pages rather than returning them to the OS. `Malloc Small` shows a further
2.3 GiB resident against 1.7 GiB dirty. A measurable share of the post-SVF floor
is reclaimable without changing any analysis behaviour.

**The floor is real regardless.** After the t=78 s burst, RSS never returns
below 2.95 GiB for the remainder of the run. Any design for a 4 GiB peak must
account for a ~3.4 GiB post-SVF floor, of which ~0.7 GiB is provably dead at the
170–190 s mark and an unknown further share is dead later.

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
cases and records one unconfirmed assumption in it that the acceptance task must
close.

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

`veritas_souffle_run` (`src/wpa/SouffleRunner.cpp:43`) calls
`ProgramFactory::newInstance(program_name)` on every invocation. Each call builds
a fresh program object with its own relation tables, symbol table, and
interpreter state, then `delete`s it. Construction cost is paid 13,716 times to
produce 13,716 independent evaluations.

### 3.3 Fact identity is re-derived once per fact and twice per witness edge

`AnalysisFactBus::Validate` (`src/facts/AnalysisFactBus.cpp:289`) is a genuine
integrity check and is retained in full. It begins by recomputing
`DeriveBatchId(batch)` — a hash over every fact and witness row in the batch —
and then calls `DeriveFactId` once for each of 1,249,792 facts (line 319) and
**twice for each of 1,375,911 witness edges**, once for the result row (line 348)
and once for the input row (line 352). That is approximately 4.0M SHA-256
computations over encoded rows.

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

`MakeAnalysisFactBatch` (`src/facts/AnalysisFactBus.cpp:170`) builds a
`KeyedWitness` per surviving edge carrying `result_key` and `input_key` as
`std::string`, encoded through `AppendSemanticKey`. Across 1,375,911 edges that
is ~2.75M string allocations held simultaneously, and the vector is then sorted
with a comparator over `std::tie(result_key, rule_id, input_key, input_ordinal)`
— lexicographic string comparison through a 1.38M-element comparison sort. This
is the 70%-of-main-thread frame at t=360 s.

The same allocation is the largest single contributor to the memory step
between the WPA window and batch assembly: run 3 shows RSS rising 1.12 GiB
across the 345 s → 375 s boundary, immediately before the sort completes.

### 3.6 Abandoned memory is not returned

`Malloc Small (empty)` holding 703.7 MiB resident against 18.8 MiB dirty at
t≈180 s shows the allocator retaining fully-emptied arenas. Nothing in the
pipeline asks for that memory back at a phase boundary where it is known to be
dead — in particular after the SVF stage completes and after the component
payloads are consumed.

## 4. Goals

1. Execute each WPA component through the compiled program's in-memory
   relations, with no intermediate directory, file, or CSV text.
2. Instantiate each component's compiled program once per run, not once per
   component, resetting relations between components.
3. Derive each distinct fact identity once per validation pass and once per
   publication pass, without deleting or weakening any check.
4. Commit component cache state in batches rather than once per component.
5. Order facts and witnesses with keys whose comparison is an order-isomorphism
   of the current string comparison, at a fraction of the memory and with no
   per-edge allocation.
6. Release provably dead memory at phase boundaries.
7. Reduce retained component payload during the WPA window.
8. Complete the motivating command within 375 s and at no more than 4 GiB peak
   resident set on the reference machine and fixture.

## 5. Non-goals

This design reopens one round-2 non-goal and keeps every other.

**Reopened:** two items that round-2 section 5 excluded are in scope, because the
4 GiB acceptance criterion cannot be met without them:

- *releasing memory after SVF completes* — goal 6, designed in section 7.6;
- *not retaining WPA payloads until assembly* — goal 7, designed in section 7.7,
  and also excluded by round-2 section 9.5.4.

SVF's *analysis* is untouched. Round-2 section 5's "Do not change SVF
construction, Andersen analysis, MemorySSA, or SVFG construction" is **not**
reopened; only the lifetime of memory SVF has already allocated is.

**Still excluded, and this round must not change them:**

- No accuracy trade of any kind. Epistemic states, fact sets, witness selection,
  and the set of executed `(SccId, WpaComponentKind)` components are unchanged.
- No weakening of `Validate`, `ValidateSemanticRow`, or any identity check. The
  redundancy in section 3.3 is removed by memoization and cheaper encoding, never
  by deleting a check.
- No change to SVF analysis configuration, `summary.v2`, `relations.v2`, the
  typed relation registry, rule bundles, model bundles, or fact identity.
- No batching of multiple SCCs into one Soufflé execution, and no parallel
  component execution. Goal 2 reuses *one program object* across sequential
  components; it does not merge components into one evaluation.
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

### 7.2 One program instance per run

`veritas_souffle_run` gains a run-scoped entry point that obtains one program
instance, runs it for each component, and resets state between components via
`purgeInputRelations`, `purgeOutputRelations`, and `purgeInternalRelations`.
The existing one-shot entry point may remain as a thin wrapper; the executor
uses the scoped form.

The reset must be proven to restore a state indistinguishable from a fresh
`newInstance`. Section 9.3 makes this a differential test rather than an
assumption.

### 7.3 Memoized fact identity in validation and publication

`Validate` keeps every check it performs today. Two changes remove the
redundancy:

1. **Memoize row identity within the pass.** A row's fact identity is a pure
   function of the row, so a single `row → fact_id` memo serves the per-fact
   loop and both endpoints of every witness edge. Distinct rows across the batch
   are 1,249,792 facts plus the rooted inputs; the witness endpoints are drawn
   from exactly that set. The memo therefore collapses ~4.0M derivations to
   roughly the fact count, with no change to any comparison performed.
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

### 7.6 Memory relief at phase boundaries

Introduce one narrowly-scoped helper that asks the platform allocator to return
emptied arenas — `malloc_zone_pressure_relief(nullptr, 0)` on Darwin,
`malloc_trim(0)` elsewhere — and call it at the boundaries where the pipeline
knows a large working set has just become dead:

1. after the SVF stage completes and its results have been extracted;
2. after WPA result payloads have been consumed;
3. after publication completes.

This changes no data structure and no result. The measured basis is the
`Malloc Small (empty)` region: 703.7 MiB resident against 18.8 MiB dirty at
t≈180 s. The call is cheap and its effect is directly measurable as an RSS step
at each boundary, which is how section 9.4 accepts it.

### 7.7 Retained component payload

Round-2 section 9.5.4 identified this and excluded it. It is required for the
4 GiB goal and is included now.

The obstacle is real and must not be designed around carelessly: canonical owner
selection among duplicate facts requires processing components in canonical
component-key order, while execution must remain reverse-topological. Those two
orders differ, so assembly cannot begin until every component has executed, and
`SuccessorSupport` may read any completed component's facts until the last
predecessor has run.

The design therefore does not fold components into the batch as they complete —
that would change owner selection. Instead:

1. A completed component's full result is written to the content-addressed store
   as it is today, so it is durable and reloadable.
2. In memory, the orchestrator retains only the subset `SuccessorSupport` can
   actually consume: rows whose relation is the `derived` relation of a domain
   carrying `support.has_value()` for any component kind. This is a small
   fraction of a component's facts and none of its witnesses.
3. During assembly, each component's result is reloaded from the
   content-addressed store through the existing `LoadReusableComponent`
   deserialization and revalidation path, and released again once its facts and
   witnesses have been keyed.

This preserves execution order, failure isolation, component caching, owner
selection, every hash, and every published row, because assembly consumes the
same bytes that were stored.

**Cost, stated plainly:** 13,716 content-addressed reads plus deserialization
move into the assembly window, trading wall time for memory. This round is
constrained on both. The mitigation is that section 7.1 and section 7.2 remove
far more per-component wall time than these reads add, and section 9.4 measures
both limits together rather than one at a time. If measurement shows the read
cost dominates, the fallback is to retain facts in memory and reload only
witnesses, which is the larger structure per edge; that variant is a
measurement decision, not a contract change.

## 8. Error handling

- An in-memory relation lookup for an expected relation name returns
  `Internal("compiled program has no relation <name>")`.
- A dense id in a derived relation that the input mappings cannot resolve keeps
  returning the existing `InvalidArgument`/`FailedPrecondition` from the mapping
  `ToStable` paths.
- A program reset that leaves observable state behind is a conformance failure,
  not a silent fallback: the differential test in section 9.3 must fail.
- Reloading a component result during assembly that fails deserialization or
  revalidation returns the existing `FailedPrecondition` from
  `LoadReusableComponent`; it is not retried and not silently skipped.
- Batched cache commits preserve the existing rollback behaviour: a failed
  transaction rolls back its batch and the run marks incomplete through the
  orchestrator's existing failure path.
- Allocator relief is best-effort and never fails a run.

## 9. Verification strategy

### 9.1 The equivalence instrument

Published content is compared through ordered dumps of the four published tables,
hashed with `sqlite3 … "SELECT * FROM <table> ORDER BY …" | shasum -a 256`. The
ordering was determined by measurement, not assumed: `ORDER BY rowid` is
insertion order and does not reproduce round 2's published digests; ordering by
the primary key does.

| Table | Ordering | This round | Round-2 section 9.5.2 |
| --- | --- | --- | --- |
| `analysis_facts` | `fact_id` | `452a850ec90ab192…` | `452a850ec90ab192…` — **equal** |
| `analysis_facts` | `rowid` | `6b0aea6381242e30…` | — |
| `run_fact_bindings` | all columns | `f297dcf25ac551dc…` | `bab0a696647ffb25…` |
| `provenance_nodes` | all columns | `a3b8b0f71efcf07e…` | `387d299985baf5e8…` |
| `provenance_edges` | all columns | `3b38fc4b7e5a1712…` | `da4829f280ade0b8…` |

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

**A caveat the acceptance task must close.** The exclusion list below is a
hypothesis, not a verified result. Projections of `run_fact_bindings` that
exclude `run_id` and `analyzer_run_id` alone, and that exclude `binding_id`
separately, were tried against round 2's digest and **none reproduced it**. The
identity-column attribution is therefore unconfirmed, and the acceptance task
must determine the actual exclusion set rather than trusting this list.

The comparison proceeds in two steps.

**Step 1 — did the toolchain identity move?** Compare the published
`engine_toolchain_identity` before and after. Every edit in this design except
the encoding change of section 7.3 should leave it untouched, since only files
compiled into the functor library feed it.

**Step 2a — identity unchanged (expected).** Require all four digests equal under
the orderings above, and require `BatchId`, `FixpointHash`, `ExternalHash`, and
`LogicalInputHash` equal directly.

**Step 2b — identity moved.** Require `analysis_facts` equality, equal row counts
for all four tables, and equality of the identity-bearing tables under a
projection excluding the run-scoped columns, with that exclusion set
**determined by the task**, per the caveat above. Sections 7.1, 7.2, and 7.5
claim no *row* can change; this form tests exactly that while conceding that
identity columns legitimately move.

The pre-change member of the pair is produced by building the pre-change revision
in the same build tree, as round 2 did, so the comparison is not confounded by
the compiler.

### 9.2 Unit and integration tests

- A materializer/executor test proves in-memory execution and file-based
  execution produce identical `RawWpaEvaluation` row sets for a fixture
  component, and identical `LogicalInputHash`, `FixpointHash`, and
  `ExternalHash`.
- A test proves one reused program instance across two sequential components
  yields the same results as two fresh instances, including the case where the
  first component is non-empty and the second's relations must start empty.
- `AnalysisFactBusTest` proves the packed-rank ordering reproduces the string
  ordering, including ties on `result_key` with differing `rule_id`, differing
  `input_key`, and differing `input_ordinal`, and including the `std::unique`
  boundary.
- A validate test proves the memoized derivation performs the same number of
  checks and rejects the same tampered batches as before, by mutating a fact id,
  a witness result row, and a witness input row in turn.
- `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts` and the
  `wpa-qualification` differential corpus must pass unchanged, and the M9 entry
  gate must report all ten criteria passing.
- No test may be skipped; a `GTEST_SKIP` reports as passed to CTest, so the
  existing no-skips check remains mandatory.

### 9.3 Program-reset differential test

Section 7.2's reset is accepted only by measurement: for a set of components
including one that derives no facts following one that derives many, the reused
instance must produce results byte-identical to fresh instances, and a component
run twice on a reused instance must produce identical results both times.

### 9.4 Performance acceptance

Both limits are measured together, on the reference machine and fixture, with
`/usr/bin/time -lp` against a fresh output directory:

- wall time at most 375 s;
- maximum resident set size at most 4 GiB.

Additionally, because section 7.6 claims a reclaimable floor, the run must show a
measurable resident-set step at each allocator-relief boundary, recorded in the
spec's implementation-status section. A relief call that releases nothing is a
finding to report, not a success to assume.

Three consecutive runs are recorded, since section 2.1 shows 0.72 GiB of
run-to-run spread on identical inputs; the acceptance claim uses the worst of the
three.

### 9.5 Pre-push verification

Per `.claude/rules/pre-push-verification-policy.md`: clean build from scratch,
full CTest suite with no skips, `git diff --check`, license-header check, clean
working tree. This round's changes are C++ and CMake, so the license-header
check is in scope for every modified file.

## 10. Open risks

1. **The 4 GiB limit may not be reachable even with all seven designs.**
   Section 2.3 establishes a ~3.4 GiB post-SVF floor of which only ~0.7 GiB is
   proven dead at t≈180 s. Section 7.7 removes the retained payload and
   section 7.6 returns emptied arenas, but the residual live SVF footprint is
   not measured and may exceed 4 GiB minus publication working set on its own.
   If measurement shows this, the honest outcome is to report the wall-time
   result, report the measured memory floor with its attribution, and reopen the
   4 GiB criterion as a separate decision rather than to weaken accuracy
   silently to reach it.
2. **The program-reset semantics of the pinned Soufflé revision are not yet
   verified.** `purgeInternalRelations` must restore a state equivalent to a
   fresh instance for these four programs. Section 9.3 tests it, and if it
   cannot be shown, section 7.2 is dropped and the file round trip of section 7.1
   still stands on its own.
3. **Wall time and memory pull against each other in section 7.7.** The
   reload-for-assembly trade adds reads to the window this round is trying to
   shorten.
4. **The 0.72 GiB run-to-run spread** means acceptance needs three runs and the
   machine must be quiet during them; macOS background daemons were previously
   observed to stretch this project's heaviest integration tests.
