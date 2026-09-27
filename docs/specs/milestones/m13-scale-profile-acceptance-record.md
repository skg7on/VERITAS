# M13 acceptance record — the store-equivalence instrument

This is the measurement record Task 7 of
[the M13 implementation plan](../../plans/milestones/m13-scale-profile-store-equivalence-instrument-implementation-plan.md)
requires: the projection that makes `veritas-store-diff` trustworthy, derived
from two real runs and then asserted rather than recorded in prose.

It is the artefact M14's harness and later milestones cite. It is a
*measurement record*, a document class this repository has no earlier instance
of; see "Where this record is indexed" at the end.

Everything below was measured on the build this task produced. The commit of
the tree under measurement is the M13 branch head, and the two `semantic_zoo`
runs used for the recorded sweep were re-run on the frozen binary after the last
relink, so the runs, the measurement, and the comparison all come from one
build tree.

## 1. The runs

### 1.1 Fixture runs

The plan's Step 1 command cannot run as written. Every fixture's
`compile_commands.json` carries a literal `@PROJECT_ROOT@` that only the test
harness substitutes (`tests/support/ProjectFixture.cpp`), so the project root
named on the command line is not the project root the manifest describes. The
fixture was materialized into `/private/tmp/m13-task7-fixture` with the
placeholder substituted exactly as the harness does it, and both runs used that
one materialized root:

```bash
./build/bin/veritas-build analyze --project /private/tmp/m13-task7-fixture \
    --output /tmp/m13-a/store
./build/bin/veritas-build analyze --project /private/tmp/m13-task7-fixture \
    --output /tmp/m13-b/store
```

Both exited 0, at 13.15 MiB of `metadata.db` each and the same row counts in
the published tables (`analysis_facts` 3,193 · `provenance_nodes` 1,964 ·
`provenance_edges` 3,045 · `run_fact_bindings` 1,964).

### 1.2 LevelDB runs

The real acceptance case, run once each, sequentially:

```bash
./build/bin/veritas-build analyze --project /Users/skg7on/Workspace/Projects/leveldb \
    --output /tmp/m13-leveldb-a/store
./build/bin/veritas-build analyze --project /Users/skg7on/Workspace/Projects/leveldb \
    --output /tmp/m13-leveldb-b/store
```

Both exited 0. Run A took 7 min 9 s, run B 7 min 13 s. The published row counts
are `analysis_facts` 1,249,792 · `run_fact_bindings` 752,076 ·
`provenance_nodes` 752,076 · `provenance_edges` 1,375,911. The first and last
match round 3 section 9.1's stated counts exactly.

## 2. Which columns actually move — the measurement

Plan Step 2's command, run against each pair:

```bash
for t in $(sqlite3 /tmp/m13-a/store/metadata.db \
             "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"); do
  cols=$(sqlite3 /tmp/m13-a/store/metadata.db "PRAGMA table_info($t)" | cut -d'|' -f2)
  for c in $cols; do
    a=$(sqlite3 /tmp/m13-a/store/metadata.db "SELECT $c FROM $t ORDER BY rowid" | shasum -a 256)
    b=$(sqlite3 /tmp/m13-b/store/metadata.db "SELECT $c FROM $t ORDER BY rowid" | shasum -a 256)
    [ "$a" = "$b" ] || echo "MOVES: $t.$c"
  done
done | sort
```

### 2.1 Output, verbatim — the `semantic_zoo` pair

```
MOVES: build_variants.created_at
MOVES: repositories.created_at
MOVES: revisions.created_at
MOVES: summary_bindings.publication_epoch
MOVES: summary_objects.created_at
MOVES: translation_units.created_at
MOVES: wpa_analysis_runs.completed_at
MOVES: wpa_analysis_runs.started_at
MOVES: wpa_component_states_v2.updated_at
MOVES: wpa_component_states.updated_at
MOVES: wpa_sccs.created_at
```

### 2.2 Output, verbatim — the LevelDB pair

The same command with the `metadata.db` paths of the LevelDB stores:

```
MOVES: build_variants.created_at
MOVES: repositories.created_at
MOVES: revisions.created_at
MOVES: summary_bindings.publication_epoch
MOVES: summary_objects.created_at
MOVES: translation_units.created_at
MOVES: wpa_analysis_runs.completed_at
MOVES: wpa_analysis_runs.started_at
MOVES: wpa_component_states_v2.updated_at
MOVES: wpa_component_states.updated_at
MOVES: wpa_sccs.created_at
```

The two outputs are **byte-identical** (`diff` reports no difference). Eleven
columns over ten tables, and every one of the eleven is a wall-clock column:
each is a SQLite `DEFAULT (strftime('%s', 'now'))` or the equivalent, so it
records *when* a row was written and not *what* was written.

### 2.3 Why the LevelDB pair had to be measured at all

The plan's Step 1 chooses `semantic_zoo` "because it exercises all four WPA
domains and therefore every published table". That is true of the published
tables and false of the store. Ten of the store's 37 tables are empty under
`semantic_zoo`, and six of them carry a wall-clock column that therefore cannot
be seen to move:

```
       0  analysis_configurations
       0  analyzer_runs          (started_at)
       0  component_deltas
       0  function_bodies        (created_at)
       0  function_symbols       (created_at)
       0  function_variants      (created_at)
       0  reverse_dependency_index
       0  source_anchors
       0  summary_deltas         (created_at)
       0  summary_dependencies   (created_at)
```

The LevelDB runs were required to close that gap, and they closed it in the
opposite direction from the one feared: the same ten tables are empty under
LevelDB too. `analyzer_runs`, the three `function_*` tables, `summary_deltas`
and `summary_dependencies` are not written by either measured input, which is
consistent with their being schema ahead of the code that will fill them. The
LevelDB measurement is therefore not redundant: it is what turned "the fixture
leaves these ten tables empty" into "the two inputs leave them empty", and the
two differ by three orders of magnitude — 3,193 facts against 1,249,792 — so
the emptiness is a property of the pipeline and not of the fixture's size.

That is two inputs, not every input, and the limit is worth stating rather than
papered over: a wall-clock column in a table this build never writes cannot be
*seen* to move, so the honest reading of "no entry is recorded for any of them"
is that no entry is *needed for them as measured*. An entry would become
necessary the day one of them is populated, and no guard would say so: the
`DumpStore` stale-exclusion check catches an exclusion whose column has
*disappeared*, and a table with no entry has no exclusions to go stale — it has
the opposite problem. What catches it is running the column-by-column step above
again on a pair of stores whose tables are no longer empty, which is a step
M14's corpus will re-run with a larger input. That is why this section records
the method alongside the result.

## 3. The measured projection

Fifteen entries. `order_by` is the sort key the rows are digested in;
`excluded_columns` are omitted from the digest.

| Table | `order_by` | Excluded columns |
| --- | --- | --- |
| `analysis_facts` | `fact_id` | — |
| `run_fact_bindings` | `rowid` | `run_id`, `analyzer_run_id`, `binding_id` |
| `provenance_nodes` | `rowid` | `run_id` |
| `provenance_edges` | `rowid` | `run_id` |
| `wpa_component_states` | `rowid` | `updated_at` |
| `wpa_component_states_v2` | `rowid` | `run_id`, `result_cache_key`, `result_object_key`, `updated_at` |
| `wpa_component_result_cache_v2` | `rowid` | `engine_toolchain_identity`, `result_cache_key`, `result_object_key` |
| `build_variants` | `rowid` | `created_at` |
| `repositories` | `rowid` | `created_at` |
| `revisions` | `rowid` | `created_at` |
| `summary_bindings` | `rowid` | `publication_epoch` |
| `summary_objects` | `rowid` | `created_at` |
| `translation_units` | `rowid` | `created_at` |
| `wpa_analysis_runs` | `rowid` | `started_at`, `completed_at` |
| `wpa_sccs` | `rowid` | `created_at` |

Every table with no entry gets the default `{"rowid", {}}`, which is correct for
it: a table with no moving column needs no entry. `sqlite_sequence` is the one
that looks like it needs one and does not — its `seq` column records
AUTOINCREMENT high-water marks, which are a function of row counts and were
measured equal in both pairs. Note also what the table above is *not*: nothing
is excluded wholesale. `DumpStore` refuses a table whose every column is
excluded, so `sqlite_sequence` could not have been handled that way even if its
`seq` had moved.

### 3.1 How the fifteen were determined

The fifteen were determined two different ways, and the difference matters
because one of the two methods is blind to some of them.

**Seven by reproducing round 3 section 9.1's recorded digests.** Section 9.1
recorded a digest for each of seven tables. Each was recomputed on the LevelDB
store, with round 3's own instrument (`sqlite3 … | shasum -a 256`) over exactly
the retained columns this projection keeps, in declaration order, in the
recorded ordering. All seven reproduce byte for byte — the table below has eight
rows because section 9.1 recorded `analysis_facts` under both orderings:

| Table | Section 9.1's recorded digest | Reproduced |
| --- | --- | --- |
| `analysis_facts` (`fact_id`) | `452a850ec90ab192a2e5cde28269067f06f4618338675c26c870cac466e41cda` | equal |
| `analysis_facts` (`rowid`) | `6b0aea6381242e301ddfb993c7048b213037f9aafc18c7acd3e23ac894ab8ed1` | equal |
| `run_fact_bindings` | `d732ec43ca21a5967170139f69f3688145c2771b7a079b78ef7b26a6eddf3b63` | equal |
| `provenance_nodes` | `d8410e270ed19257491a830e9debb666299be54365482b83236ac06cbacdf321` | equal |
| `provenance_edges` | `6691f96f5b5f137e29f9ce0aeb4ddf4380c2da4f15ec3041a6f5344daa98a038` | equal |
| `wpa_component_states` | `bc3a24c41499841110c8d35c4195da04da800f28ef1240d85c0dc34e63e6782b` | equal |
| `wpa_component_states_v2` | `93c3aef64efecb3a07a8ae7c0408bcee86a4db9aaffbf198f664601e1f936fb1` | equal, with `updated_at` also excluded — see 3.2 |
| `wpa_component_result_cache_v2` | `bf38b36262c8c8091c119f2950bf8acc9eecca018027b1d64b75e6e86e6eb9bb` | equal |

Section 9.1 also records `cpg_nodes` all-columns as `e98d16b3cc6fb642…` for its
completeness sweep; that reproduces too
(`e98d16b3cc6fb642ffef2375a1308af08b9a1d0eeef0ab4878f85498341a71e8`), which is
independent evidence that the published CPG content is unchanged.

**Eight by the column-by-column comparison of section 2**: `build_variants`,
`repositories`, `revisions`, `summary_bindings`, `summary_objects`,
`translation_units`, `wpa_analysis_runs`, `wpa_sccs`. Section 9.1 records no
digest for these, so there was nothing to reproduce; each exclusion above is a
wall-clock column measured to move in **both** pairs.

### 3.2 The correction to round 3's prose, and one correction withdrawn

**Retraction: section 9.1's exclusion list for `wpa_component_states_v2` is
incomplete, and the list it gives does not reproduce its own recorded digest.**

Section 9.1 says that table is identical "excluding `run_id`,
`result_cache_key`, and `result_object_key`". Computed that way on the LevelDB
store the digest is

```
3a03a0bab9a1ac4a99341873083bbb1ee1931e4ade4d6b10cd5557777616630f
```

which is **not** the `93c3aef64efecb3a…` section 9.1 records. Adding
`updated_at` to the exclusion set gives

```
93c3aef64efecb3a07a8ae7c0408bcee86a4db9aaffbf198f664601e1f936fb1
```

which is the recorded value exactly. So section 9.1's *digest* is right and its
*prose* omits one column: the recorded digest was computed with `updated_at`
excluded, and the digest cannot be reproduced any other way. `updated_at` also
moves in the column-by-column measurement of section 2 (43 distinct values in
run A, 42 in run B), which agrees. The projection recorded here is section
9.1's list plus `updated_at`.

There is a plausible reconstruction of how the prose lost the column — the
sentence immediately before it describes `wpa_component_states` "excluding the
wall-clock `updated_at`", and the clause may have been meant to carry across —
but the reconstruction is a guess and the measurement is not. The digest is the
evidence, and it says four exclusions.

The `updated_at` finding is the only correction to section 9.1's exclusion lists.
A first draft of this record also claimed that section 9.1 misassigns
`engine_toolchain_identity` to `wpa_component_states_v2`. **That claim was false
and is withdrawn.** Section 9.1
(`docs/specs/veritas-build-analyze-round3-performance-design-spec.md:877-881`)
assigns `run_id`, `result_cache_key` and `result_object_key` to
`wpa_component_states_v2`, and `engine_toolchain_identity` plus those same two
keys to `wpa_component_result_cache_v2` — which is where this record keeps it,
following section 9.1. The column is carried by exactly two tables,
`wpa_component_result_cache_v2` and `wpa_analysis_runs` (checked against the
store's own schema, all 37 tables), and section 9.1 places it on the right one.

The error was mine, not round 3's: a `PRAGMA` probe of the wrong table returned
"no such column", and that was read as round 3 being wrong rather than the probe
being wrong. It is recorded here rather than quietly deleted because this section
is where a reader audits the rest of the record, and a correction that silently
loses a correction is worse than one that shows its own — a reader who checked
only this paragraph and found round 3 right would be entitled to discount the
`updated_at` finding beside it, which is the real one.

### 3.3 What the measurement does **not** decide

**Eleven exclusions across five entries** cannot be confirmed by the
column-by-column comparison of section 2. Named in full, because this is the
list a reader audits the claim "no exclusion was added that no method measured"
against, and a short list would make that claim look stronger than it is:

| Entry | Exclusions section 2 cannot confirm |
| --- | --- |
| `run_fact_bindings` | `run_id`, `analyzer_run_id`, `binding_id` |
| `provenance_nodes` | `run_id` |
| `provenance_edges` | `run_id` |
| `wpa_component_states_v2` | `run_id`, `result_cache_key`, `result_object_key` |
| `wpa_component_result_cache_v2` | `engine_toolchain_identity`, `result_cache_key`, `result_object_key` |

Eleven columns, five entries — not seven of anything. `result_cache_key` and
`result_object_key` appear on two of those entries each and are easy to lose when
the list is summarised as "`run_id` and its derivatives"; `analyzer_run_id` and
`binding_id` are derived identities on the same footing as `run_id`. None of the
eleven appears in section 2's `MOVES` output, so by Step 2's rule alone all
eleven would be removed.

Plan Step 2 says "every column it does not report must not [be excluded]", and
the reason this record keeps them anyway is that the rule was written for a
measurement that cannot see the case they exist for.

`run_id` is a hash of the revision and build-variant ids, the summary, relation,
rule-bundle and model-bundle versions, the two configuration hashes, the engine
tag, and the **engine toolchain identity** — which binds the provenance digest of
the compiled Soufflé runner and functor libraries. The canonical encoding is
`Canonicalize(AnalysisRunDescriptor)` in `src/facts/AnalysisRun.cpp`, whose last
appended field is `engine_toolchain_identity`. Two runs of one input in one build
tree therefore have the *same* `run_id`, which is exactly what section 2
measures: `run_id` is
`run:sha256:a75a950c01963d81ca86e1b4998269a452ebfdfaf8cc98353cf9c080622c30a6` in
both `semantic_zoo` stores. It is not that `run_id` is stable; it is that this
pair cannot move it. Two *builds* of one input do move it, and section 9.1 says
so directly
(`docs/specs/veritas-build-analyze-round3-performance-design-spec.md:914-921`;
the sentence quoted below is at `:918-920`): "`run_id` and
`engine_toolchain_identity` move between any two revisions built in this Debug
configuration — no projection that keeps those columns can match across them".
That is the comparison the instrument exists
for: the roadmap's section 6.2 makes `baseline` "the differential conformance
oracle" against `scaled`, and section 6.1 states that `scaled` "opts into the
structural changes of Stages 2–6" whose "structural stages change *how* identity
is computed". A projection that kept these columns would report `differ` on the
baseline-versus-scaled pair — the instrument would fail on its primary use case,
and M14's harness would have to ignore it.

#### The eleven are pinned by a reproduction that is itself cross-build

The reproductions of section 3.1 run on the reference LevelDB store this record
measured; round 3 supplies the digest *literal*. What the reproduction pins is
round 3's **exclusion set**: `93c3aef6…` and `bf38b362…` are not reproduced
without the columns above, so round 3's literals were computed with them
excluded. The source is the literal, the object is this store, and the earlier
draft of this paragraph had that backwards.

The reproduction is stronger than "the same content, measured twice", and the
reason is worth stating because it is the property M14's oracle depends on:

> **The store this record measured carries
> `souffle-14d108c0cf82e133b216cabafaa288d16d1a8a5b5e18374ab740d0e826e6d38c`.**
> Round 3's recorded value is `souffle-0ef51c2207f7a5aa…`, with
> `souffle-e4135d90a5f5d329…` before it. They are different toolchain
> identities.

So round 3's digests for `run_fact_bindings`, `provenance_nodes`,
`provenance_edges`, `wpa_component_states_v2` and
`wpa_component_result_cache_v2` reproduce byte for byte on a store built under a
**different toolchain identity**. A digest that retained `run_id` or
`engine_toolchain_identity` could not survive that gap, because both are
functions of the identity that moved; these five do survive it. That is genuine
cross-build evidence, taken from two different revisions of the toolchain rather
than from two runs of one binary, and it is exactly the comparison the M14
harness will make. It is also the reason the same-tree pair of section 2 could
never have decided this question. `--scale-profile` is deliberately kept out of
every canonical encoding, and `run_id` is
`MakeStableId(kAnalysisRun, Canonicalize(descriptor))` — a fixed id kind and a
fixed `veritas.wpa-run.v1` domain tag over ten descriptor fields, none of them
the profile. So a baseline-versus-scaled pair in one build tree shares one
`run_id`, and moving it takes a change to one of those ten. The last subsection
shows how small such a change can be.

The identity value above is a per-build artefact and not a constant — see the
last subsection here for why, and for what that means for M14.

#### The three tables that report `differ` across a build boundary, measured

An earlier draft of this subsection named `wpa_analysis_runs` as "one table out
of 37 where the report is identity rather than content", generalised from the
encoding argument earlier in this section. **That completeness claim was false.**
It is replaced here by an enumeration and two observations rather than by a third
generalisation from one member.

*Enumerated.* Every column of all 37 tables, in **both** reference stores, was
searched for the store's own `run_id`, its `engine_toolchain_identity`, and the
`batch_id` derived from the former — by substring, so an embedded value counts,
and over recorded tables as well as unrecorded ones, because a recorded table can
carry an identity value in a column its projection does not exclude. A name-based
filter would have missed `fact_batch_receipts`' own `wpa_run_id`, which is why
the search is by value. Both stores return the same rows: **twelve columns in
eight tables hold one of the three values; the projection excludes five of those
columns, and the remaining seven — in three tables — reach the digest.**

| Table | Identity columns reaching the digest | Excluded by the projection |
| --- | --- | --- |
| `wpa_analysis_runs` | `run_id`, `engine_toolchain_identity` | no (only `started_at`, `completed_at` are) |
| `fact_batch_receipts` | `run_id`, `batch_id`, `wpa_run_id` | no — not recorded, so nothing is excluded |
| `wpa_fact_bus_deliveries` | `run_id`, `batch_id` | no — not recorded, so nothing is excluded |
| `provenance_edges` | `run_id` | yes |
| `provenance_nodes` | `run_id` | yes |
| `run_fact_bindings` | `run_id` | yes |
| `wpa_component_result_cache_v2` | `engine_toolchain_identity` | yes |
| `wpa_component_states_v2` | `run_id` | yes |

`fact_batch_receipts` and `wpa_fact_bus_deliveries` are absent from `kRecorded`,
so `ResolveTableProjection` gives both `{"rowid", {}}` and their identity values
reach their digests. The five excluded columns are the reason the five
identity-bearing published/WPA tables do **not** appear in the cross-build
`differs` list below — which is the projection doing its job, and the
discriminating control that shows the list is not merely "every table with a
run id in it".

Two further notes so a reader running a *name* filter lands in the same place.
`wpa_analysis_runs` also has `stale_base_run_id`, of the same identity type; it is
NULL in both measured stores, so the value search yields no hit for it, and it is
in a table already listed, so it changes no count. And `analyzer_runs` and
`analysis_configurations` match a name filter on `analyzer_run_id` — a local
AUTOINCREMENT, not a run- or toolchain-derived value, in tables that are empty,
so mutating it on the measured pair leaves the pair equivalent. They are named
because the answer is "not in the class", not "in the class but quiet".

*Observed.* The enumeration was then checked against real build boundaries rather
than argued from them. Relinking the compiled Soufflé functor library moves
`engine_toolchain_identity`, which moves `run_id` — the case this subsection is
about. Two independent such pairs were compared, and both report the identical
three tables:

```
$ ./build/bin/veritas-store-diff /tmp/m13-a/store /tmp/m13-crossbuild/store
stores differ
  differs: fact_batch_receipts (left 1 rows, right 1 rows)
  differs: wpa_analysis_runs (left 1 rows, right 1 rows)
  differs: wpa_fact_bus_deliveries (left 1 rows, right 1 rows)
```

So **three tables out of 37**, not one. The other **34 compare equal across a
genuine toolchain-identity change** — every published table among them, and
fourteen of the fifteen tables the projection records, `wpa_analysis_runs` being
the exception. That is the property M14's oracle needs, and it is now measured
rather than asserted. M14's harness should expect exactly these three.

*Why the seven are left un-excluded rather than added to the projection.* Not
oversight, and not deference to section 9.1: exclusion cannot express what two of
the three tables need. `fact_batch_receipts` has exactly three columns — `run_id`,
`batch_id`, `wpa_run_id` — and all three are identity, so excluding the identity
would exclude every column, which `DumpStore` refuses by design
(`FailedPrecondition`, "every column of <table> is excluded"). Its digest has no
content in it to compare. `wpa_fact_bus_deliveries` is nearly the same shape:
`run_id`, `batch_id`, and a constant `sink_id`. Only `wpa_analysis_runs` could
carry a meaningful reduced projection, and excluding for it alone would buy an
asymmetric instrument that hides the run identity for one table while its two
neighbours report it — a reader would have to know which table was special and
why. Reporting all three is the disposition that needs no such knowledge, and
this subsection is what makes the report actionable.

#### Rebuilds move the identity too, which makes this the everyday case

Section 9.1 says "an edit moves it only if it changes one of the inputs
`cmake/WriteSouffleProvenance.cmake` hashes". That is a necessary condition and
it is not wrong, but it is silent on *rebuilds*, and a rebuild is enough:
`functor_library_sha256` is `file(SHA256 …)` over the **compiled library**, and
on this platform the linker stamps a fresh UUID into every link, so the file's
digest changes even when every input byte is identical. Measured:

```
$ touch src/facts/SemanticKeyCodec.cpp      # no content change; git status empty
$ cmake --build --preset default
$ shasum -a 256 build/lib/libveritas-souffle-functors.dylib
262a02dbe85f19539e2a5c694d5801f7ec1ee29cb845b4812b4fdbde04928485   # before
8dad39eaa1cf05f3a780dc36dfacbcd4374e88cbc95362a850f17b2ce711ea84   # after
```

and the identity moved with it, `souffle-c9b98d05…` → `souffle-be74ba86…`. The
practical consequence for M14 is that its baseline-versus-scaled pair will cross
this boundary whether or not any source change is intended — an ordinary rebuild
is sufficient — so the three tables above are the everyday case, not an edge
case, and a harness that treats a non-empty `differs` list as a failure needs to
know them by name.

None of the three is excluded, and that is a disposition rather than an
omission: exclusion cannot express what `fact_batch_receipts` and
`wpa_fact_bus_deliveries` need, because almost all of their columns *are* the
identity, and adding one for `wpa_analysis_runs` alone would hide the run
identity for one table and not its two neighbours. The reasons are given in full
above.

## 4. The comparison, and its exit code

### 4.1 `semantic_zoo`

```
$ ./build/bin/veritas-store-diff /tmp/m13-a/store /tmp/m13-b/store
stores are equivalent (37 tables compared)
$ echo $?
0
```

### 4.2 LevelDB

```
$ ./build/bin/veritas-store-diff /tmp/m13-leveldb-a/store /tmp/m13-leveldb-b/store
stores are equivalent (37 tables compared)
$ echo $?
0
```

Both reach the plan's Step 4 requirement. It is worth recording what the same
command reported *before* this task's projection was applied, because it is the
control for the two lines above — and it was measured by rebuilding the tool
with round 3's four published-table entries only, not inferred from the
projection table.

`semantic_zoo`, exit 1:

```
stores differ
  differs: build_variants (left 1 rows, right 1 rows)
  differs: repositories (left 1 rows, right 1 rows)
  differs: revisions (left 1 rows, right 1 rows)
  differs: summary_bindings (left 29 rows, right 29 rows)
  differs: summary_objects (left 29 rows, right 29 rows)
  differs: translation_units (left 6 rows, right 6 rows)
  differs: wpa_analysis_runs (left 1 rows, right 1 rows)
  differs: wpa_component_states (left 112 rows, right 112 rows)
  differs: wpa_component_states_v2 (left 112 rows, right 112 rows)
  differs: wpa_sccs (left 28 rows, right 28 rows)
```

LevelDB, exit 1 — the same ten tables, at that store's counts:

```
stores differ
  differs: build_variants (left 1 rows, right 1 rows)
  differs: repositories (left 1 rows, right 1 rows)
  differs: revisions (left 1 rows, right 1 rows)
  differs: summary_bindings (left 3429 rows, right 3429 rows)
  differs: summary_objects (left 3429 rows, right 3429 rows)
  differs: translation_units (left 39 rows, right 39 rows)
  differs: wpa_analysis_runs (left 1 rows, right 1 rows)
  differs: wpa_component_states (left 13716 rows, right 13716 rows)
  differs: wpa_component_states_v2 (left 13716 rows, right 13716 rows)
  differs: wpa_sccs (left 3429 rows, right 3429 rows)
```

Ten tables, every one of them with equal row counts on both sides — which is the
shape the instrument must report and not suppress: the rows are the same, the
run-scoped column in them is not. The exit code is the deliverable, and the
`differs` list is what it is for.

### 4.3 The third exit code

The two subsections above record two of the contract's three codes. The third —
2, "could not compare" — is the one a caller must not mistake for either of the
others, and it belongs here because a contract quoted in two thirds is a
contract a reader will fill in from expectation.

A store root that holds no store:

```
$ ./build/bin/veritas-store-diff /tmp/m13-a/store /tmp/m13-a/absent
veritas-store-diff: no metadata store at /tmp/m13-a/absent/metadata.db
$ echo $?
2
```

and an argument count that is not two store roots:

```
$ ./build/bin/veritas-store-diff
usage:
  veritas-store-diff --version
  veritas-store-diff <left-store-root> <right-store-root>

Compares the metadata.db of two store roots table by table, using the
determined projection recorded in
docs/specs/milestones/m13-scale-profile-acceptance-record.md:
run-scoped columns are excluded, rows are ordered deterministically, and
each table is digested. Exits 0 when the stores are equivalent, 1 when
they differ, and 2 on error.
$ echo $?
2
```

A missing store is "could not compare" rather than "different" because the two
call for opposite responses: one is a path to fix, the other is a finding. The
message names the path it stat-ed rather than the store root it was given, so
the reader sees which file was absent; and when *both* stores are unreadable it
names the **left** one, because the comparison dumps and checks the left store
before it opens the right, so an unreadable left store is never silently
reported as a comparison of the right store against nothing.

The first line of that usage block is the fifth CLI's `--version`, which exits
0 like its four siblings'. It is answered before the argument-count check
because a differential conformance harness is the most likely thing in this
repository to record a tool's version, and without that branch `--version` is an
argument count that is not two store roots, so a probe reads the usage error as
a comparison error.

These two are not measurements of the tree the rest of this record measured —
they are properties of the argument handling and the read path, and they do not
depend on the projection or on the input. They are asserted, on real
invocations of the built binary, by
`tests/integration/summarydb/VeritasStoreDiffTest.cpp`, which pins all three
codes: 0 for two identical stores, 1 after one cell in one store is changed, and
2 for both of the cases above — plus the left-before-right ordering, which is
asserted by pointing both arguments at different missing roots and requiring the
left one to be named.

## 5. The LevelDB anchor

Plan Step 6 asks whether the published digests match section 9.1's
`452a850ec90ab192…` for `analysis_facts`. That literal came from a LevelDB
store, and `analysis_facts` carries no run-scoped column, so it should reproduce
across revisions. It does:

```
$ sqlite3 /tmp/m13-leveldb-a/store/metadata.db \
    "SELECT * FROM analysis_facts ORDER BY fact_id" | shasum -a 256
452a850ec90ab192a2e5cde28269067f06f4618338675c26c870cac466e41cda  -
```

and identically for `/tmp/m13-leveldb-b`. Under `rowid` it is
`6b0aea6381242e301ddfb993c7048b213037f9aafc18c7acd3e23ac894ab8ed1`, also
section 9.1's recorded value, at section 9.1's recorded 1,249,792 rows.

## 6. The store's own table count

Round 3 section 9.1 says "all **39** tables have equal row counts". Both stores
this build produces report **37**:

```sql
SELECT count(*) FROM sqlite_master WHERE type='table';   -- 37
```

The 37 is accounted for exactly, and cheaply: the schema declares 36 tables
(`v1.sql` 27, `v2.sql` 3, `v3.sql` 4, `v4.sql` 1, plus the runtime
`wpa_fact_bus_deliveries` in `src/facts/AnalysisFactBus.cpp`), and
`sqlite_sequence` is SQLite's own, created because a table declares
AUTOINCREMENT. 36 + 1 = 37.

The difference from 39 is **not** explained. The round-3 stores are not in the
tree, so the two extra tables cannot be identified and this record does not
guess at them: a two-table schema delta, a store carrying SQLite statistics
tables, and two tables removed by a later migration are all consistent with the
evidence and none is measured. What is measured is that the count the store
reports is 37, that the schema's own declaration accounts for all of it, and
that the completeness sweep below therefore covers 37 tables and not 39.

## 7. Other places the measurement contradicts round 3's prose

**Retraction: section 9.1's completeness sweep does not describe this store.**
Section 9.1 states that in its whole-store sweep "Only `wpa_sccs` and
`summary_objects` differ, and only in `created_at`". Both measured pairs report
ten tables differing in a run-scoped column (section 4.1). Eight of the ten are
named nowhere in that sentence, and six of the eight — `build_variants`,
`repositories`, `revisions`, `summary_bindings`, `translation_units`,
`wpa_analysis_runs` — are named nowhere in section 9.1 at all.

The two halves of the sweep sentence that *are* right should be said as well:
`wpa_sccs` and `summary_objects` do differ, and `created_at` is what moves in
both. The sweep was incomplete rather than wrong about what it looked at.

Whether round 3's own pair really showed only two movers could not be
re-measured: `/tmp/veritas-prof/run4/store` and `/tmp/t9a/run1/store` are not in
the tree. The likeliest reconstruction — that the sweep had already dropped
wall-clock columns for the three tables it names explicitly and under-reported
for the rest — is a guess. The measurement on the pairs that do exist is ten
tables, in both of them, and that is what this record states.

Nothing else in section 9.1 is contradicted. Its four published-table
projections, its `wpa_component_states` and `wpa_component_result_cache_v2`
projections, its `analysis_facts` and `cpg_nodes` literals, its row counts for
`analysis_facts` and `provenance_edges`, and its claim that the identity columns
it held included (`witness_id`, `selected_witness_id`, `producer_id`,
`summary_id`, `source_anchor_id`) are stable all reproduce.

## 8. Two accepted deviations from the plan

Both were ruled on before this task ran and are in the code as delivered. They
are recorded here as deviations rather than passed over, because the plan says
otherwise in two places.

**The row stream joins columns with `|`, not `\t`.** Section 9.1's recorded
digests are `sqlite3 … | shasum -a 256` output; `sqlite3`'s default list mode
separates columns with `|`. An instrument joining with `\t` is a different
function that could never reproduce section 9.1 — and section 3.1 above is the
proof that `|` does: all seven of section 9.1's recorded digests reproduce byte
for byte. The tab form would have forced a retraction of prose that is correct.

**Values are escaped (`\` → `\\`, `|` → `\|`, newline → `\n`) rather than
refused.** A census of a real store found embedded newlines in
`cpg_edge_support.provenance_ref` (17,963 rows), `cpg_projections.summary_ids`
(1 row) and `cpg_projections.canonical_hash` (1 row), with zero tabs and zero
pipes anywhere. Re-measured on this task's LevelDB store, the census is exactly
that: `instr(provenance_ref, char(10)) > 0` in 17,963 rows of
`cpg_edge_support`, and 1 row each in `cpg_projections.summary_ids` and
`cpg_projections.canonical_hash`, with `instr(…, '|')` and `instr(…, char(9))`
zero in all three. The plan's refusal design therefore made a real store
undumpable and `veritas-store-diff` unusable on real input.

For tables that carry such characters, section 9.1's own instrument is
**ambiguous by construction**: `sqlite3 … | shasum -a 256` cannot tell a newline
inside a value from a row boundary, so no well-defined section 9.1 digest exists
for them. That is a limit of round 3's instrument and not a retraction of correct
prose — and it is the reason the two `cpg_projections` tables carry no entry
here: for them there is nothing to reproduce, and escaping is what closes the
gap rather than what departs from it.

## 9. Controls

The projection is asserted by two tests, and both were shown to fail before
being shown to pass.

- `StoreEquivalenceTest.TheRecordedProjectionIsExactlyTheMeasuredOne` asserts
  the recorded table name for name against the literal list above, then asserts
  every entry's `order_by` and `excluded_columns`. Two mutations were applied to
  the measured tree and both failed it: setting `revisions`' exclusion to `{}`
  (the loop half) and adding a `summary_components` entry (the size half, which
  the assertion reports as 15 against 16).
- `StoreEquivalenceTest.UnrecordedTablesCarryTheDefaultProjection` asserts the
  other half: **18 of the 22** unrecorded tables — the store has 37 and the
  projection records 15 — must resolve to `{"rowid", {}}`. The four it does not
  name are `analyzer_runs`, `component_deltas`, `function_bodies` and
  `function_variants`; they are covered indirectly rather than not at all, by
  the size and name assertions of the case above, which fail if a sixteenth
  entry appears. Without this half, an entry added for a table the instrument
  cannot determine would be invisible.
- `StoreEquivalenceEndToEndTest.TwoRunsOfOneFixtureAreEquivalent` runs two full
  M1→M4→M5→M3 analyses of `semantic_zoo` in-process, against one materialized
  fixture and two store roots, and requires the two stores to compare
  equivalent. With `revisions`' exclusion set to `{}` it fails and names the
  table: `two runs of one fixture published different content:` /
  `  differs: revisions (left 1 rows, right 1 rows)`. It is registered with an
  explicit `add_test` and `TIMEOUT 300`, not with `gtest_discover_tests`, which
  offers no per-test timeout.

  Two things it needs beyond a bare equality assertion, both because the claim
  is about *published content*:

  - **It asserts the content before it asserts the agreement.** Two
    schema-identical empty stores take the same projection and compare equal, so
    equality alone is satisfiable by a pipeline that publishes nothing. The case
    dumps both stores and requires `analysis_facts` and `provenance_edges` to be
    non-empty in the first. Applied as a control — pointing the row tally at a
    name no table has — it fails with
    `the first run published no facts, so the comparison below is vacuous`.
  - **Its store roots are per-process.** The roots are fixed names in the shared
    temporary directory otherwise, and this repository runs several worktrees at
    once. The control is decisive: with the names shared, two concurrent runs of
    the binary cannot both pass — one dies with
    `Failed to open RocksDB: IO error: While lock file:
    …/veritas_m13_e2e_first/objects/LOCK: Resource temporarily unavailable` in
    under a second. With the names keyed to the process, two concurrent runs both
    pass.

  `StoreEquivalenceTest`'s own temporary paths are keyed the same way for the
  same reason.

## 10. Where this record is indexed

The record is listed under the M13 row of
[the plan matrix](../../plans/README.md), the table that already carries M13,
rather than in `docs/specs/milestones/README.md`. That index holds **approved
design specifications**, one per milestone, with the milestone's implementation
plan and issue beside them; it lists no measurement or acceptance record, and
M13 has no row in it at all. Putting this document in a column headed "Design
specification" would mislabel it, and inventing a column for one document is a
repository decision rather than a milestone decision. The docs-layout policy has
no pattern for a record of this class either, so there was nothing to conform to.

An earlier version of this section drew the wrong conclusion from those correct
facts: it treated "no convention exists" as a reason to add nothing, and said so.
That is the defect the rest of this record exists to catch, in miniature — the
observation was accurate and the consequence drawn from it was not. An artefact
nobody knows how to place is a problem to solve, and leaving it unsolved is paid
for by the next milestone: M14's plan has to cite this record (section 3.3 names
the three tables its harness must expect), and a document reachable only by
grepping for its filename is one the next plan author will paraphrase from
memory instead — which is how this record's own corrections to round 3's prose
would get re-introduced.

In the source tree the record is cited from four places: the comment above
`kRecorded` in `src/summarydb/StoreEquivalence.cpp`, the
`ResolveTableProjection` declaration in
`include/veritas/summarydb/StoreEquivalence.h`, the literal projection asserted
by `StoreEquivalenceTest.TheRecordedProjectionIsExactlyTheMeasuredOne`, and the
usage text of `src/tools/veritas-store-diff.cpp`. The last of those is the one
this round changed: it used to name round 3's section 9.1 as the home of the
projection, which is the list `wpa_component_states_v2`'s correction in section
3.2 above supersedes — so the tool a reader consults to find the projection was
pointing them at the version of it that is one column short.
