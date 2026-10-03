# VERITAS Scaling Milestone Roadmap

> **Review direction (2026-10-03):** Use the review delivery roadmap for proposed product priority. This plan retains its capability-specific history and gates. See the
> [review-driven analysis proposal](../specs/veritas-review-driven-analysis-design-spec.md). Existing runtime, schema and
> acceptance contracts remain unchanged until their implementation amendments
> are reviewed.

Milestone band **M13–M18**, planned against the measured evidence of
`veritas-build analyze` rounds 2 and 3. This roadmap defines stages, gates, and
falsification criteria. It is deliberately **not** an implementation plan: each
stage receives its own design specification and implementation plan at the
granularity of the round-2 and round-3 documents, written when that stage
starts.

Read `docs/specs/veritas-build-analyze-round3-performance-design-spec.md` first
for the measured baseline, `docs/architecture/02-whole-program-analysis-architecture.md`
for the WPA and alias-tier contracts, and
`docs/architecture/03-summarydb-storage-architecture.md` §15 for the storage
sizing claim this roadmap exists to test.

---

# 1. Purpose

The motivating command analyses a ~10,000-line LevelDB checkout in **572–587 s
of wall time** at a **3.4 GiB unreclaimable floor** and an **8.56 GiB peak
resident set**, producing a **4–7 GiB** store. The architecture documents size
the V1 target differently: §15 of the storage architecture estimates
**480 MB – 10 GB total** for *"a laptop-class machine analyzing a million-line
C/C++ codebase."* Per unit of code — 400–700 MiB/KLOC measured against that
model's 0.5–10 MiB/KLOC — the measured footprint is **two to three orders of
magnitude** above the estimate used to declare 1 MLOC feasible.

Rounds 2 and 3 attacked constant factors inside the existing shape — copies,
allocations, per-row statements, per-component temporary directories, fact-id
encoding — and bought a real **−26.5 % CPU** (`573.93 s → 421.67 s`, worst of
three) with published content **proven byte-identical**. That is a 1.3× lever
applied to a 100× problem.

This roadmap exists to state which constraints actually block the 10–100×
range, to say honestly which of them the existing fixture *cannot* distinguish,
and to sequence the work so that the expensive, semantically risky changes are
funded by measurement rather than by arithmetic.

---

# 2. Measured evidence base

Every figure below is cited to its source. Figures marked **(unmeasured)** are
absent from all existing documents and are named here because a stage depends on
them.

## 2.1 The fixture

| Quantity | Value | Source |
| --- | ---: | --- |
| Compilation database entries | 39 | R2 §2 |
| Analyzed function objects | 3,548 | R2 §2 |
| SCCs | 3,429 | R2 §2 |
| WPA domains per SCC | 4 | R2 §2 |
| WPA component executions | 13,716 | R2 §2; R3 §2.4 |
| SVF pointers | 88,563 | R2 §2 |
| SVFG nodes / edges | 187,055 / 246,002 | R3 §2.1 |
| `analysis_facts` rows | 1,249,792 | R3 §2.4 |
| `run_fact_bindings` rows | 752,076 | R3 §2.4 |
| `provenance_nodes` rows | 752,076 | R3 §2.4 |
| `provenance_edges` rows | 1,375,911 | R3 §2.4 |
| Published rows, total | 4,129,855 | derived |
| Store bytes on disk | 4–7 GiB | user's observation; **(unmeasured)** in any spec |

## 2.2 Cost distribution

Wall time, round-3 stack samples (R3 §2.2):

| Window | Duration | Share | Phase |
| --- | ---: | ---: | --- |
| 0–105 s | ~105 s | 18 % | ingest, local summaries, SVF |
| 105–360 s | ~255 s | 44 % | WPA component loop |
| 360–395 s | ~35 s | 6 % | batch assembly |
| 395–583 s | ~190 s | 33 % | publication |

Resident set, round-3 run 4 (R3 §2.3):

| Elapsed | RSS | Phase |
| ---: | ---: | --- |
| 78 s | 3.39 GiB | +1.66 GiB burst in one interval — SVF construction |
| 120 s | 3.52 GiB | SVF complete; floor established |
| 361 s | 6.92 GiB | WPA payload accumulating across 13,716 components |
| 545 s | 8.56 GiB | peak, during publication — 95 % of wall time |

The peak is **late, inside publication**, not mid-run. Post-SVF the resident set
never returns below 2.95 GiB, establishing a **~3.4 GiB floor** that round 3
measured as **not reclaimable** (`malloc_zone_pressure_relief` released 0 bytes
at every call and is a no-op on Darwin; R3 §3.6).

## 2.3 Per-unit costs

| Quantity | Value | Source |
| --- | ---: | --- |
| Wall per WPA component (255 s / 13,716) | 18.6 ms | derived from R3 §2.2 |
| Component body, measured | ~0.40 ms | R3 §3.2 |
| Soufflé session `open`+`close`, mean | 15.9 µs | R3 §3.2 |
| Per-component commit, isolated | 0.335 ms | R3 §9.6 item 3 |
| `DeriveFactId`, before → after round 3 | 8.476 µs → 1.256 µs | R3 §9.6 item 2 |
| `DeriveFactId` calls, before → after | 9,561,990 → 5,792,572 | R3 §9.6 item 2 |
| `DeriveBatchId`, share of assembly window | 50.9 %, and it runs **twice** | PR #137 |
| Publication I/O leaves | `guarded_pwrite_np` 16.6 %, `fsync` 8.3 %, `pread` 8.0 % | R3 §2.2 |
| Publication hashing | `SHA256Hasher::Update` 12.4 % | R3 §2.2 |
| Retained component payload | 748,647 of 1,249,792 facts = **≈60 %** | R3 §7.7 |
| SQLite commits per run, before → after | 13,716 → ~54 per repository | R3 §7.4 |

**All figures are Debug configuration** (R3 §2.1). No release-build measurement
exists anywhere in either spec (**(unmeasured)**).

## 2.4 Extrapolation to 1 MLOC

Scaling the fixture's own constants by 100×, with linear growth assumed:

| Quantity | At 10 kLOC | At 1 MLOC (linear) |
| --- | ---: | ---: |
| Functions / SCCs / components | 3,548 / 3,429 / 13,716 | 355,000 / 343,000 / 1,371,600 |
| Facts / published rows | 1.25 M / 4.1 M | 125 M / 410 M |
| Store | 4–7 GiB | **400–700 GiB** |
| Wall | 572–587 s | **~16 h** |
| Peak RSS | 8.56 GiB | **~850 GiB** |

**The superlinear case cannot be estimated from this fixture, and no attempt is
made here.** The four domains close over *different index sets* —
`ReachableCall` over functions, `GlobalFlow` over value identity
(88,563 SVF pointers), `MayRead`/`MayWrite` over memory references — so their
observed densities are not comparable and no single facts / `F`² ratio is
meaningful. A 1 MLOC estimate would require knowing which domain dominates the
closure at that scale, which is exactly what Stage 1's depth-axis measurement
exists to determine (H-C0, §7.3). The linear row above is a **floor**, not a
prediction.

Both figures are extrapolations of **one fixture**. That is the central caveat
of this document and the reason Stage 1 exists.

---

# 3. Primary constraints

Ranked. Each is stated with the evidence that establishes it, and each names its
growth law. Constraints C0–C3 are **blocking**; C4–C7 are **amplifying**.

## C0 — The analysis materializes a transitive closure

`logic/flow/global_flow.v2.dl:44-47` is the **non-linear** transitive closure
rule:

```prolog
GlobalFlow(s, d, e) :- GlobalFlow(s, m, e1), GlobalFlow(m, d, e2), WeakenEpistemic(e1, e2, e).
```

`logic/reachability/reachability.v2.dl:36-39` is the linear closure step, and
`:43-46` composes it with a **successor's entire published closure** as support:

```prolog
ReachableCall(f, h, e) :- DirectCall(_, f, g, _, e1), SupportReachableCall(g, h, e2), ...
```

A component therefore publishes `{its own members} × {everything transitively
reachable from them}`. Summed over all SCCs that is `Σ_f |reach(f)|`, which is
**Θ(F · reach)** and **Θ(F²) for a call chain**. The published fact set is the
closure, materialized.

The witnesses amplify it. Every transitive fact emits **two** `Witness` tuples
(`global_flow.v2.dl:122-138`), each carrying `dk = cat(a_key, b_key)` — a
concatenation of two canonical semantic keys, and each key is a `cat`-nesting of
79-character `StableId` text (~214 B). Per derived fact the engine therefore
builds and materializes roughly **856 B of concatenated key strings**, which C++
then re-encodes (`src/facts/FactStore.cpp:212`, no memo) and re-hashes twice.

Corroboration: `provenance_edges` / `analysis_facts` = 1,375,911 / 752,076 ≈
**1.83**, consistent with two witnesses per derived fact. Retained component
payload is **≈60 %** of the published set, broken out as `GlobalFlow` 225,223 +
`MayRead` 213,027 + `MayWrite` 206,849 + `ReachableCall` 76,316 + `UnknownEffect`
27,232 (R3 §7.7) — every one of these is a propagation-carried relation.

**Growth law:** Θ(F · reach); Θ(F²) worst case.

## C1 — Run-atomic publication forces whole-run retention

One `AnalysisFactBatch` carries one content-addressed `BatchId` covering every
fact, and `Validate` re-derives that id to check it
(`src/facts/AnalysisFactBus.cpp:427-430`). Consequently every component's payload
is retained from the moment the component completes until the batch is
assembled — `std::vector<WpaComponentCompletion> completed_components`
(`include/veritas/wpa/WpaOrchestrator.h:53`), whose `WpaComponentResult` holds
`std::vector<AnalysisFact> facts` and `std::vector<WitnessEdge> witnesses` for
**every component simultaneously**, pushed unbounded at
`src/wpa/WpaOrchestrator.cpp:281-282`.

Assembly then materializes the same content again in at least four more
whole-run representations: `owned` (`AnalysisFactBus.cpp:300`), `FactRanks`
interned keys (~0.3 GiB, `:216-220`), `keyed_facts`/`keyed_witnesses`
(`:308-309`), and `batch.facts`/`batch.witnesses`. `Validate` adds
`FactIdentityMemo` (~400–450 B per distinct row), `fact_index`, `endpoints` (two
strings per witness edge), `witnessed`, `dependencies`, `input_count`
(`:459-535`). The sink adds another set in
`src/facts/FactStore.cpp:180-237`. `rooted_input_facts` is unbounded with no
dedup and a `RootedInputFact` is an `AnalysisFact` plus **five** `std::string`s
(`include/veritas/facts/Witness.h:70-77`).

**Memory is Θ(total facts) by contract, not by implementation.** Round 3
demonstrated this precisely: its two reductive memory designs (releasing memory
after SVF, and reloading payloads for assembly) were the two it had to drop, and
the second measured the retained set at ≈60 % of the published set rather than
the assumed "small fraction."

**Growth law:** Θ(total facts) resident.

## C2 — Identity is re-derived and re-encoded at every layer boundary

A `core::StableId` is 33 bytes of information — a 4-byte kind tag and a 32-byte
digest — carried as an 80-byte heap block, written into an 88-byte preimage, and
a 79-character text (`include/veritas/core/Ids.h:55-59`;
`src/facts/AnalysisFact.cpp:275-357`). A `ReachableCall` fact's preimage is
**219 B** and its semantic key **214 B**. The same identity is derived afresh at
each boundary it crosses: 9.56 M `DeriveFactId` calls before round 3, the
materializer re-encoding and re-hashing every EDB row per component
(`src/wpa/WpaInputMaterializer.cpp:639-644`, `:757-765`), `DeriveBatchId`
re-encoding every row twice (PR #137), and `EncodeSemanticKey` per witness edge
with no memo (`FactStore.cpp:212`). Round 2 §3.1 counted this defect class at
3,548 summaries × 13,716 components = **48,664,368** avoidable whole-program
identity parses. Round 3 fixed the instance inside `Validate` and left the
materializer's copy untouched.

**Growth law:** Θ(facts × boundaries crossed).

## C3 — Sequential per-component execution, with no core scaling

`component_count = |SCCs| × |domains|` (`src/wpa/WpaOrchestrator.cpp:162-166`;
`src/analysis/ProjectAnalyzer.cpp:248-252`). Execution is a double nested loop,
outer over SCCs and inner over component kinds (`WpaOrchestrator.cpp:172-284`),
strictly sequential. The executor **rejects** anything but one worker:
`"the Souffle WPA executor requires exactly one worker thread"`
(`src/wpa/SouffleWpaExecutor.cpp:435-438`), with the default
`std::uint32_t threads = 1` (`include/veritas/wpa/WpaExecutor.h:43`). There is no
scheduler, work queue, or async anywhere in `src/wpa/`.

**Growth law:** Θ(components) wall time, insensitive to available cores.

## C4 — Monolithic whole-program pointer analysis

SVF builds the full SVFG before any summary exists and is released only at run
end (`docs/architecture/02-whole-program-analysis-architecture.md` §6.3, §7.4:
*"SVF is initialized per analysis run and released at the end"*). This sets the
**~3.4 GiB unreclaimable floor** measured in R3 §2.3, and the residual live SVF
footprint is **explicitly unmeasured** (R3 §10 risk 1: it "may exceed 4 GiB minus
publication working set on its own").

This is the one place VERITAS does **not** apply its own thesis: P6 says WPA
consumes summaries by default, and SVF is the exception that is never summarized.
§6.4 already specifies the remedy — an L3 **demand-driven** tier — but confines
it to per-query refinement inside the Evidence Builder.

**Growth law:** Θ(program), floors memory.

## C5 — Store amplification

| Item | Value | Source |
| --- | --- | --- |
| Row payload, `ReachableCall` | **~641 B** on disk | derived from `FactStore.cpp:144-154` |
| of which hex-encoded protobuf | **542 B** (hex doubles 271 B) | `HexCodec.h:27-34` |
| Journal mode | `delete` (rollback journal), **not WAL** | `MetadataStore.cpp:131` is the only PRAGMA |
| Synchronous | `FULL` (default) — one fsync per commit | same |
| Btrees written per row | 2 (table + implicit PK autoindex) × 4 tables | `schema/v3.sql:27-84` |
| Provenance node / edge row | ≥216 B / ≥298 B of **repeated 75–76-char IDs** | `ProvenanceStore.cpp:62-81` |
| Component CAS payload | cells serialized **twice per witness**, `StableId` as full 79-char text | `WpaRunRepository.cpp:57-96`, `:110-131` |

33 bytes of information become 641 bytes on disk. The `fact_id` foreign key on
`run_fact_bindings` (`schema/v3.sql:48`) is **not** a quadratic factor: SQLite
enforces it on INSERT by probing the *parent* key, which is `analysis_facts`'
primary key and therefore indexed. The absent child index on `fact_id` would
cost on parent DELETE/UPDATE, which the publication path does not perform. It is
recorded here as a minor, not a constraint.

**Growth law:** Θ(rows), with a measured constant of ~641 B for ~33 B of content.

## C6 — Incrementality is computed and then discarded

`SccStateRepository::StoreState` classifies `ExternalChange` by comparing the
committed previous `externally_visible_hash` and calls
`WpaCoordinator::EnqueuePredecessorsIfChanged`
(`src/wpa/WpaOrchestrator.cpp:266-274`). The enqueue lands in a **stack-local**
`runtime::WorklistScheduler` and then in `WpaRunResult::scheduled_predecessors`,
which the repository's own header documents as *"a reported list that no code
executes or persists"* (`include/veritas/wpa/SccStateRepository.h:104-111`).

The consequence is stated honestly: this is a **documented gap, not an
oversight** — the header introduces it while reasoning about lost-batch
tolerance, and the conservative direction (classify as changed) is what it
chooses. But M7's reverse-dependency invalidation does not reach M9's WPA path:
the run computes what to invalidate and throws it away.

Separately, the component cache **does** hit
(`WpaOrchestrator.cpp:222-225` skips execution), but
`LoadReusableComponent` re-derives every fact id and recomputes both canonical
hashes on read (`src/wpa/WpaRunRepository.cpp:572-585`). That revalidation is
what made round 3's reload-for-assembly design cost **+109 s CPU and +0.30 GiB**
(R3 §7.7). A cache that costs as much to verify as to recompute provides no
incrementality.

**Growth law:** reuse ≈ recompute; a one-function change costs a whole-program run.

## C7 — The component store grows on every run

`ObjectStoreRocksDb::PutIfAbsent` is implemented as RocksDB `Merge` with a
verifying merge operator (`src/summarydb/ObjectStoreRocksDb.cpp:146-162`). The
comment states the intent — *"atomically enforce put-if-absent with content
verification"* — and the mechanism has a consequence: `Merge` appends an operand
per call whether or not the key exists, with the invariant enforced lazily at
read or compaction.

`WpaOrchestrator.cpp:250` calls `StoreSuccessfulComponent` **unconditionally**,
on the cache-hit path as well as the miss path. So a run in which nothing changed
still re-writes every component's payload. The logical key set is deduplicated by
the merge operator; the **physical** store is not. Whether this dominates the
4–7 GiB is **(unmeasured)**.

**Growth law:** physical store grows per run, independent of semantic change.

---

# 4. The two regimes, and the honest uncertainty

C0 is **asymptotic**. C1, C2, C5 are **constant-factor**, and this fixture
already exhibits all three at full strength. The two diagnoses demand different
programmes:

* if **C0 binds**, the fix is compositional summaries — a change to the meaning
  of a summary, touching fact identity, witness canonicalization, and every
  Evidence IR consumer;
* if **C1/C2/C5 bind**, the fix is representation and layout — a systems change
  with no semantic risk.

**LevelDB cannot distinguish them.** It is shallow, so its closure is sparse
(10 % of `F`²), and its shallow depth is exactly what hides the quadratic term
while its small size fails to amortise the constants. Choosing between the two
regimes on this fixture would be guessing, and round 3's headline finding is the
warning: *four designs, three of them the author's own, were refuted by measuring
their own target, and every magnitude attached to a real hot spot was wrong until
a task measured it* (R3 §9.6).

Hence Stage 1. It is not ceremony; it is the only thing standing between this
roadmap and a large, well-argued, wrong programme.

Two further unmeasured quantities with the same standing:

* **Debug configuration only.** Every figure is Debug; no release number exists.
  A release build with the optimiser enabled could be a large free factor.
* **Store bytes on disk.** Neither spec quotes one. The 4–7 GiB figure is the
  user's observation, not an instrument reading.

---

# 5. Roadmap overview

| Stage | Milestone | Title | Removes | Gate |
| --- | --- | --- | --- | --- |
| 0 | M13 | Baseline profile and equivalence instrument | — | `baseline` reproduces today's published content byte-for-byte; the harness detects a seeded perturbation |
| 1 | M14 | Scaling corpus and regime determination | — | Each hypothesis in §7.3 falsified or confirmed, with measured exponent and per-KLOC constants |
| 2 | M15 | Compact hierarchical semantic identity | C2 | `baseline`-equivalence on the whole store; peak RSS and batch-id pass measured down |
| 3 | M16 | Incremental execution end-to-end | C6, C7 | A one-function change costs the invalidated cone, not the repo |
| 4 | M17 | Bounded-memory streaming publication | C1, C5 | Peak RSS is `O(largest component) + O(assembly window) + floor`, not `O(total facts)` |
| 5 | M18 | Execution scaling and selective PTA | C3, C4 | Wall scales with cores; the PTA floor stops scaling with program size |
| 6 | M19 (conditional) | Compositional closure | C0 | Fact count grows with exponent ≈ 1.0 in `F` |

Stage 6 is **funded only if Stage 1 confirms C0**. Stages 2 and 3 are
sequenced early because they are the spine (Stage 3) and its prerequisite
(Stage 2), and because Stage 2 is independently justified by the ≈60 %
support-retention measurement even if no other stage is ever funded.

## 5.1 Why 10× and 100× are different problems

* **10× is a representation problem.** C2, C5, C1, C3 — constant-factor, low
  semantic risk, every item already justified by a measurement in hand.
* **100× is an algorithm problem.** At 1 MLOC the closure term (C0) and the PTA
  floor (C4) dominate whatever the constants are. Neither yields to
  representation work.

The staging buys 10× first, cheaply, and makes the 100× spend conditional on
evidence.

## 5.2 Milestone numbering

The band is M13–M18 (plus conditional M19). `docs/architecture/02-whole-program-analysis-architecture.md:435-437`
reserves M13 tentatively for a Soufflé-native-PTA research — *"may research one
only against explicit correctness, precision, model-coverage, and performance
benchmarks, independently of the M9-M12 critical path."* Stage 5 requires
selective/demand-driven PTA work, which is adjacent, so that research is
**folded into Stage 5 as its benchmark gate** rather than displaced by it. Those
four benchmarks are the ones Stage 5 needs anyway.

---

# 6. Stage 0 (M13) — Baseline profile and equivalence instrument

## 6.1 What it delivers

1. A `--scale-profile=baseline|scaled` option on `veritas-build analyze`,
   **defaulting to `baseline`,** so no existing user's behaviour or output
   changes. `baseline` preserves today's semantics exactly: run-atomic batch,
   v1 `BatchId`, flat identities, sequential per-component execution. `scaled`
   opts into the structural changes of Stages 2–6.
2. The store-diff equivalence harness that compares two stores under the
   instrument of §14.1.
3. Perturbation tests for the harness itself: a seeded single-cell change, a
   dropped row, and a reordered row must each be detected. An instrument that
   cannot fail its own negative control is not an instrument.

## 6.2 Why `baseline` is not merely a fallback

It is the **differential conformance oracle**, and this is its more important
role. Structural stages change *how* identity is computed; `baseline` guarantees
they never change *whether* it is checked. That is the same mechanism the
repository already trusts elsewhere: `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts`
guards the Soufflé/C++ pair, the `wpa-qualification` corpus guards the engine
differential, and the retired C++ `FixpointEngine` was retained as a conformance
oracle after its replacement landed.

It is also what makes the "no accuracy trade" non-goal of rounds 2 and 3
enforceable rather than aspirational.

---

# 7. Stage 1 (M14) — Scaling corpus and regime determination

## 7.1 Two axes, because real repositories confound them

**Real fixtures**, each with a pinned revision and a working
`compile_commands.json`:

| Tier | Candidate | Approx. LOC |
| --- | --- | ---: |
| Baseline | LevelDB (existing) | ~10 k |
| Mid | SQLite, Redis, Abseil, protobuf | ~100 k |
| Large | LLVM `lib/Support` + `lib/IR`, PostgreSQL, a Chromium subtree | ~1 M |

**Synthetic generators with parameterized shape.** These are the actual
falsifiers, because only they can hold `F` constant while varying depth, or hold
depth constant while varying `F`:

* an `N`-deep call chain (isolates the C0 quadratic term);
* an `N`-wide fan-out star (isolates per-component fixed cost without depth);
* a layered DAG with controlled width and fan-in (the realistic middle).

A `compile_commands.json` requirement is a hard practical filter on the real
tier; the generators have none, which is why both axes are needed.

## 7.2 Measurements per fixture

Wall and CPU (three runs minimum, one build tree per comparison); peak RSS by
phase; **store bytes on disk**; facts, witnesses, and rows per domain;
`Σ_f |reach(f)|` versus `F` to fit the closure exponent; support-set fraction;
the per-component cost distribution; and **release versus Debug**.

**Much of this substrate already exists and must be extended, not rebuilt.** The
analyze-phase-observability work (PR #139, `main` at `709f2d9`) shipped
`veritas-build analyze --metrics*` and the byte-diffable versioned artifact
`<output>/run-metrics.json` rendered by `RenderRunReportJson`, carrying a
`RunIdentity` block, a `RunEnvironment` block (including `build_type`, which is
what H-DEBUG needs), a `RunInventory` block (including
`components_reused`/`components_executed` — Stage 3's measurement), a
`StoreSummary` with per-table rows and on-disk bytes, and in-process memory
sampling reporting **both** `peak_rss_bytes` and `peak_footprint_bytes`. That last
pair matters: round 3 §3.6 measured a 1.42–1.45 GiB gap between the resident set
and physical footprint, and the acceptance criterion counts the former, so
sampling both in-process is strictly better evidence than `/usr/bin/time -lp`'s
single maximum. M14 adds the corpus, the closure-shape probe, and the exponent
fitter; it adds no C++ metric code.

Its identity doctrine also bears on this roadmap: `RunReport.h:144-149` warns
that a difference inside the identity block means two runs were not
like-for-like. `scale_profile` is uncovered by any configuration hash in M13 so
that adding it moves no identity, which makes two otherwise-identical runs share
a `run_id` unless the value cannot vary. M13 keeps it from varying by rejecting
`scaled`; **M15 must append it to `WpaConfigurationHash` when it gives the value
meaning.**

## 7.3 Falsification criteria, fixed before running

| Hypothesis | Falsified when | Consequence |
| --- | --- | --- |
| **H-C0** — closure blowup binds | fact count grows with exponent < 1.5 in `F` on the depth axis | **Stage 6 dropped entirely** |
| **H-CONST** — representation binds | peak RSS ≪ 0.4 GiB/KLOC or store ≪ 200 MiB/KLOC | re-rank; do not fund Stage 4 first |
| **H-DEBUG** — toolchain config dominates | release/Debug ratio < 2× | the free lever is small; proceed structurally |

## 7.4 Exit gate

A written regime finding recording the measured exponent, the per-KLOC constants,
and a confirmed-or-falsified verdict on each hypothesis — plus, for every claim
in §3 that the corpus contradicts, the correction. **A constraint this stage
refutes is removed from §3 rather than restated.** Rounds 2 and 3 both had to
retract design premises on measurement; the roadmap is held to the same standard
as the work it sequences.

---

# 8. Stage 2 (M15) — Compact hierarchical semantic identity

## 8.1 The change

Identity becomes a run-local **interned handle** plus a digest computed **once**
at creation and memoized; canonical bytes are emitted only at serialization
boundaries. The run-level identity becomes a **Merkle root over per-component
digests in canonical component order** — O(components) instead of O(facts), and
incrementally updatable when one component's digest changes.

Most of the machinery exists. `ResultCanonicalizer` already builds
`external_bytes` and `fixpoint_bytes` over every fact key and every edge key, and
every component already carries `input_hash`, `fixpoint_hash`, and
`externally_visible_hash`. Components already have per-component canonical
hashes over their fact and witness keys. What is missing is a run-level digest
**derived from** those instead of a flat re-hash of every row.

## 8.2 What it buys

* `DeriveBatchId`'s two full-content passes (~33 s of a 421 s CPU run, PR #137)
  become two passes over component digests.
* The precondition for Stage 3's digest-keyed skip and Stage 4's streaming
  publication. Neither is possible while identity is flat and run-wide.
* Collapsing the five simultaneous whole-run representations into one canonical
  byte form plus handles.

## 8.3 Versioning

A hierarchical run identity is not byte-equal to v1 `BatchId`, and `BatchId` is a
semantic identity — changing it is a semantic event under P3. It is therefore
**versioned**: `batch.v2` under `scaled`, v1 under `baseline`. This is what keeps
§14.1's equivalence check meaningful rather than nominal.

**Contract:** structural stages change how identity is computed, never whether
it is checked. Where Stage 3 moves cache-hit verification off the read path,
that is a change in the check's **placement and policing** and must be argued
explicitly at that stage — never absorbed silently into a performance change.

## 8.4 Gate

`baseline`-equivalence across the whole store; per-component hash identity across
all components; measured reduction in peak RSS and in the batch-id pass.

---

# 9. Stage 3 (M16) — Incremental execution end-to-end

The spine. Three repairs, each of which exists in the tree as intent:

1. **Wire `scheduled_predecessors` into real re-execution.** Today
   `src/wpa/WpaOrchestrator.cpp:266-274` enqueues into a stack-local scheduler
   and the header documents the result as never executed
   (`SccStateRepository.h:104-111`).
2. **Make a cache hit a key check, not a payload re-derivation.** The descriptor
   key already covers engine, toolchain identity, logical input hash, scc,
   component, all five schema/bundle versions, SVF config, and WPA config
   (`src/wpa/WpaRunRepository.cpp:422-465`). The payload re-derivation at
   `:572-585` is defence against a corrupt store; it should be a **policed
   mode** — verify on ingest, or on a schedule, or under an explicit flag — not
   the read path.
3. **Key publication by component digest** so unchanged components are never
   re-written, and fix the re-store-on-hit behaviour of C7
   (`WpaOrchestrator.cpp:250` calls `StoreSuccessfulComponent` on the hit path
   too; `ObjectStoreRocksDb.cpp:146-162` appends per call).

## 9.1 Gate

After a one-function change to the 1 MLOC fixture, wall time and peak RSS track
the **invalidated cone**, not the repository. Demonstrated as a flat slope in
repository size and a proportional slope in cone size, measured across 10 k /
100 k / 1 M.

---

# 10. Stage 4 (M17) — Bounded-memory streaming publication

Publish as components complete, replacing the terminal run-atomic batch. Only
possible once identity is hierarchical (Stage 2). Collapse the five
whole-run representations and release payloads as they are consumed.

Store work, in order of measured payoff:

| Change | Mechanism |
| --- | --- |
| Drop hex encoding | `cells_hex` is 542 of every 641 B/row (`FactStore.cpp:148-154`) |
| WAL + batched sync | today `journal_mode=delete`, `synchronous=FULL` (`MetadataStore.cpp:131`) |
| Intern IDs in fact tables | keep the digest table once instead of per-row 75–76-char repeats |
| Provenance by reference | `ProvenanceStore.cpp:62-81` repeats full IDs per row |
| Bounded `rooted_input_facts` | unbounded, undeduped, five strings per element |

## 10.1 Gate

Peak RSS is `O(largest component) + O(assembly window) + floor` rather than
`O(total facts)`; measured bytes/KLOC reduction; `baseline`-equivalence holds.

---

# 11. Stage 5 (M18) — Execution scaling and selective PTA

## 11.1 Execution

* **SCC-batch execution** sized to amortise per-component fixed cost, while
  **preserving per-SCC semantic boundaries** — the distinction that keeps
  batching compatible with the incremental spine instead of in tension with it.
  Round 3's non-goal forbade merging SCCs into one evaluation; this stage must
  argue that distinction explicitly rather than treat the non-goal as void.
* **Parallel execution** over the condensation DAG's ready set, reopening the
  single-worker rejection at `SouffleWpaExecutor.cpp:435-438`.
* Round 3 measured per-component program instantiation at 0.218 s total
  (0.037 % of wall) and dropped program reuse on that basis — so reuse is
  **not** a lever here and must not be re-proposed without new measurement.

## 11.2 Selective PTA

Promote `docs/architecture/02` §6.4's L3 demand-driven tier, and/or per-module
summarized PTA, so the 3.4 GiB floor stops being Θ(program). This is where
M13's tentatively reserved Soufflé-native-PTA research folds in, gated by
§6.3's four named benchmarks: correctness, precision, model coverage, and
performance.

## 11.3 Gate

Wall time scales with cores; the PTA floor no longer scales with total program
size; `baseline`-equivalence holds.

---

# 12. Stage 6 (M19, conditional) — Compositional closure

**Funded only if Stage 1 confirms H-C0.** Replaces all-pairs closure facts with
boundary summaries composed on demand, at the fact and witness layer, matching
the philosophy already specified for L3. This is the only change that bounds the
**store** at 1 MLOC, and by far the largest semantic change: it touches fact
identity, witness canonicalization, and every Evidence IR consumer.

Gate: published fact count grows with exponent ≈ 1.0 in `F`; store bounded by
summaries rather than by closure.

---

# 13. Acceptance envelope

Round 3 showed absolute wall numbers are unmeasurable on a swapped machine — its
host carried 6.2 GiB of 7 GiB swap and wall time measured swap, so only CPU was
reportable. The acceptance form is therefore **per-KLOC constant plus exponent**,
with absolutes derived once against a named machine at the Stage-1 gate.

Stated per KLOC, the ask is precise and it is **not** uniform across metrics:

| Metric | Today, per KLOC | Target, per KLOC | Factor | Bound by |
| --- | ---: | ---: | ---: | --- |
| Peak RSS | **876 MiB** | ≤ 32 MiB | **27×** | C1, and C0/C4 if they bind |
| Store on disk | **400–700 MiB** | ≤ 60 MiB | **7–12×** | C5, and C0 if it binds |
| Cold wall | **58 s** | ≤ 29 s | **2×**, plus core scaling | C3 |
| Incremental, one function | whole-program | change-proportional | **qualitative** | C6 |

**Memory is the hard wall.** It is the only metric needing more than one order of
magnitude, it is the one that makes a 1 MLOC run impossible rather than merely
slow, and it is the one no constant-factor work can move — round 3 proved that by
having to drop its two reductive memory designs.

**Store needs the exponent kept honest.** The 7–12× per-KLOC reduction is
reachable from C5 alone (hex encoding is 542 of every 641 B). Whether it is
*sufficient* depends on Stage 1: a **linear** fact count already puts 1 MLOC at
400–700 GiB before reduction, and any superlinear closure term makes that worse
by an amount this fixture cannot bound (§2.4). If Stage 1 finds a superlinear
exponent, Stage 6 is not optional and this criterion is what forces it.

**Cold wall needs only ~2×, and parallelism supplies the rest.** The naive 16 h
projection assumes one core; the point of Stage 5 is that it need not be one core.

**Incrementality is where the 100× actually lives.** Cold-run work is inherently
proportional to program size — the win there is bounded by constants and cores.
A one-function change on a 1 MLOC repository costs a whole-program run today;
making it cost the invalidated cone is a factor no constant-factor programme can
reach.

Resulting absolutes, a **straw man to be fixed at the Stage-1 gate**:

| Scale | Cold wall | Peak RSS | Store | Incremental (one function) |
| --- | --- | --- | --- | --- |
| 10 kLOC | 572–587 s today | 8.56 GiB today | 4–7 GiB today | — |
| 100 kLOC | ≤ 50 min | ≤ 3 GiB | ≤ 6 GiB | ≤ 60 s, ≤ 2 GiB |
| 1 MLOC | ≤ 8 h | ≤ 32 GiB | ≤ 60 GiB | ≤ 5 min, ≤ 8 GiB |

The memory targets are the ones that determine whether Stages 4 and 6 are funded;
the wall targets are the ones that will be hardest to certify on an uncontrolled
machine, and §16 risk 5 is the reason.

---

# 14. Verification strategy

## 14.1 Instrument

R3 §9.1's **determined** instrument, not an assumed one: ordered table dumps,
an explicitly determined exclusion set (`run_id`; plus `analyzer_run_id` and the
rowid alias `binding_id`), ordered by the same key the writer uses. Applied as a
**whole-store sweep**, not a four-table check — round 3 extended to 39 tables and
found `cpg_nodes` (100,859 rows) byte-identical, which is strictly stronger
evidence.

## 14.2 Per-component equivalence

`wpa_component_states` identical across every component. This is the instrument
that makes batching, parallelism, and any identity change safe to judge — round 3
used it to prove the in-memory executor reproduced the file-backed one, per
component, across all 13,716 components.

## 14.3 Test-discipline rules

* `GTEST_SKIP` reports to CTest as a pass, so a green suite is **not** evidence a
  gate ran. Use round 3's JUnit-based membership gate checked against the
  registry's expected name set.
* New parameterized cases are verified **by index**. A `--gtest_filter` on a
  parameterized case *name* matches zero tests and reports a pass.
* Every new gate test must be shown **failing before the fix**.
* `ctest -jN` races on fixed temp directories: `gtest_discover_tests` registers
  each *case* as its own CTest entry, so concurrent cases share one fixture path.
  Serial CI never sees it; a local parallel run does.

## 14.4 Measurement discipline

* Three runs minimum on this fixture, whose run-to-run spread measured **0.72 GiB**.
* One build tree per A/B pair. The host compiler moves wall and RSS more than the
  optimisation does — round 2's comparison was invalidated exactly this way.
* **Release and Debug both.** No release number exists today.
* Wall time is reportable only on a quiet, unswapped machine; otherwise CPU.

## 14.5 Anti-cannibalisation

`baseline` and `scaled` run on every fixture in CI and are diffed. An
unexercised oracle rots into a false one — the same failure mode as a green suite
full of skips.

## 14.6 Metrics as a committed artefact

Every fixture run emits a machine-readable record, so this roadmap's exponent and
per-KLOC claims are **re-derivable rather than quoted**. Round 3's headline
lesson, made structural: magnitudes are hypotheses until a task measures its own
target.

The per-run half is already shipped: `run-metrics.json` (PR #139) is versioned,
writes object keys in sorted order, uses integer nanoseconds, and contains no
absolute path, so two artifacts diff byte for byte. M14's corpus record
**references** it rather than replacing it, and adds only the closure-shape
quantities and the fixture label. An unmeasured quantity is **absent, not zero** —
`RunOutputInventory.svfg_edges` (`RunReport.h:89-91`) is the precedent, and the
reason is that a plausibly-zero value diffs as "unchanged" while proving nothing.

---

# 15. Non-goals and preserved contracts

## 15.1 Preserved under `scaled`

* Every expected `(SccId, WpaComponentKind)` is still independently
  materialized, executed, canonicalized, and published, in the same
  reverse-topological order.
* Canonical fact and witness order, canonical-owner selection among duplicate
  facts, and the single retained witness derivation per fact are unchanged.
* No weakening of `Validate`, `ValidateSemanticRow`, or any identity check.
  Redundancy is removed by placement and memoization, never by deletion.
* Epistemic states, fact sets, witness selection, and the set of executed
  `(SccId, WpaComponentKind)` components are unchanged: **no accuracy trade**.
* Error paths continue to use `Status`/`StatusOr`; no RTTI and no exceptions
  (`.claude/rules/cpp-compilation-policy.md`).

## 15.2 Non-goals

* No change to SVF's *analysis* configuration, `summary.v2`, `relations.v2`, the
  typed relation registry, rule bundles, model bundles, or fact identity —
  except where Stage 6 is explicitly funded to change the last of these, and
  then only with `baseline`-equivalence as the gate.
* No performance-only CLI flag that silently changes semantics. `--scale-profile`
  is an explicit opt-in whose two values are each separately verified.
* No process-global cache.

## 15.3 Explicitly retracted from earlier rounds' reasoning

* The claim that the peak occurs "before any row is written" was **refuted**
  (R3 §2.3: peak at 545 s of 572.82 s, inside publication).
* The claim that the ~685 MiB of `Malloc Small (empty)` regions was reclaimable
  was **refuted** (R3 §3.6: the `resident` column was read where `dirty` was
  meant; the relief API is a no-op on Darwin).
* The claim that the retained component payload was "a small fraction" was
  **refuted** (R3 §7.7: ≈60 %).

These are recorded because the *wrong reasons* proved reusable; this roadmap's
own §3 claims carry the same exposure and Stage 1 exists to test them.

---

# 16. Risks

1. **Stage 1 may not be able to distinguish the regimes.** If the real fixtures
   have uniform call-graph shape, only the synthetic generators will separate
   them, and their realism is then the open question. Mitigation: report both
   regimes' evidence separately rather than aggregating.
2. **Stage 3's cache-verification change is a correctness-adjacent trade.** The
   re-derivation at `WpaRunRepository.cpp:572-585` exists for a reason. Moving it
   off the read path must preserve the ability to detect a corrupt store, and
   the policing mode must be exercised in CI, not merely available.
3. **Stage 6 is the only stage that changes meaning.** It changes what a summary
   is. It must not be started on arithmetic alone, and its `baseline`-equivalence
   check is weaker than the others because it deliberately changes the fact set —
   so it needs a different, specified equivalence criterion, not the one in §14.1.
4. **The 1 MLOC fixture may not exist in a usable form.** `compile_commands.json`
   for a million-line C/C++ project is a real practical barrier. Milestone M11's
   external-IR adapter path exists as an alternative route to a large program
   model and should be considered if the fixture search fails.
5. **The machine is not a controlled environment.** Wall time was unmeasurable in
   round 3 because of swap. Stage gates that depend on wall time need a named,
   quiet machine or they will report noise as progress.

---

# 17. Reading order

1. This document, §1–§4 — the problem and the constraints.
2. `docs/specs/veritas-build-analyze-round3-performance-design-spec.md` §2, §3,
   §9.6 — the measured baseline and the measurement discipline this roadmap
   inherits.
3. `docs/architecture/02-whole-program-analysis-architecture.md` §6, §9 — the
   alias tiers and SCC-scoped execution.
4. `docs/architecture/03-summarydb-storage-architecture.md` §14, §15 — the
   distributed-worker design and the sizing claim under test.
5. `logic/flow/global_flow.v2.dl`, `logic/reachability/reachability.v2.dl` —
   constraint C0 in its own words.

Each stage's design specification and implementation plan are written when that
stage starts, at the granularity of the round-2 and round-3 documents. This
roadmap does not carry them: detail written before measurement is exactly the
failure mode §4 describes.
