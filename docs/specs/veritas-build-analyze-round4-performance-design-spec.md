# `veritas-build analyze` Round 4 Performance Design Specification

**Status:** Draft for review

**Tracking issue:** [#133](https://github.com/skg7on/VERITAS/issues/133), still
open. Its successor [#136](https://github.com/skg7on/VERITAS/issues/136) closed
with round 3, and round 4 continues #133's criteria on the baseline round 3 left.

**Extends:**
[`veritas-build-analyze-round3-performance-design-spec.md`](veritas-build-analyze-round3-performance-design-spec.md)
(merged as PR #137) and, through it,
[`veritas-build-analyze-performance-design-spec.md`](veritas-build-analyze-performance-design-spec.md).

**Implementation plan:**
[`../plans/veritas-build-analyze-round4-performance-implementation-plan.md`](../plans/veritas-build-analyze-round4-performance-implementation-plan.md)

## 1. Purpose

Rounds 1–3 removed the costs they measured. Round 3's defining result is that
published content is provably unchanged while CPU fell 26.5%, so the remaining
costs are ones it did not reach, and round 3's own record says the 4 GiB peak
criterion is probably unreachable behind a measured 3.21 GiB post-SVF floor.

Round 4 therefore changes the question. The LevelDB fixture is 39 translation
units; the requirement is that the pipeline scale to a project two to three
orders of magnitude larger. This specification attacks the term that grows with
a project's derived-fact count — the **representation** every stage holds
semantic payload in — rather than trimming constant factors inside the shape
that scales badly. Its acceptance is stated in measured per-term reductions, not
as an absolute peak, per the decision recorded in section 4 and measured by the
instruments in section 9.4.

## 2. Measured Baseline

One run, 2026-09-27, on the documented Debug configuration (`clang 17.0.6`
against LLVM 24.x libraries, the host compiler `CLAUDE.md` fixes for this
machine), from a clean fully-built tree at `59198f5`, with a fresh output
directory and the phase recorder enabled:

```bash
/usr/bin/time -lp ./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output /tmp/veritas-issue133-baseline-01 \
  --metrics true --metrics-interval-ms 250 --metrics-top-n 15 --metrics-series true
```

| Measurement | Value |
| --- | ---: |
| Exit code | 0 |
| Wall time | 419.78 s |
| CPU (process, from the `run` span) | 418.702 s |
| Maximum resident set size | 7,553,449,984 B (7.03 GiB) |
| Peak physical footprint | 6,731,733,664 B (6.27 GiB) |
| Translation units | 39 |
| Analyzed functions / summaries / SCCs | 3,548 / 3,429 / 3,429 |
| WPA components | 13,716 expected, 0 reused, 13,716 executed |
| `analysis_facts` rows | 1,249,792 |
| `provenance_edges` rows | 1,375,911 |
| `provenance_nodes` / `run_fact_bindings` rows | 752,076 / 752,076 |
| `wpa_component_states_v2` rows | 13,716 |
| `wpa-component-results` object store | 196 MiB |
| `metadata.db` | 2.82 GiB |

### 2.1 This is a post-round-3 baseline, and one run is not a peak estimate

The CPU figure reproduces round 3's result independently: it recorded 421.67 s
worst-of-three for the same command after its change. This run is therefore a
valid baseline for round 4 and is **not** comparable to any figure taken before
round 3 landed.

Round 3 also measured a **0.72 GiB run-to-run spread** in peak RSS on
byte-identical input and reported a worst-of-three peak of 8.5945 GiB. The
7.03 GiB above is one observation inside that spread, not a peak. Per section
9.4, no peak claim in this specification rests on a single run.

Round 3's measured post-SVF floor is **3.21 GiB**, and it established that the
floor is not reclaimable: freeing 800 MiB of small-zone allocations left the
resident set unchanged, and `malloc_zone_pressure_relief(nullptr, 0)` released
0 bytes at every boundary in two runs. Round 4 does not re-propose that.

### 2.2 Phase profile

| Span | Inclusive | Self | Peak within | Δ resident |
| --- | ---: | ---: | ---: | ---: |
| `run` | 419.178 s | 8.044 s | 7.02 GiB | +5.35 GiB |
| `facts.publish` | 119.998 s | 1.403 ms | 7.02 GiB | −816.25 MiB |
| `wpa.orchestrate` | 157.318 s | 15.690 s | 6.01 GiB | +2.39 GiB |
| `m5.svf` | 74.847 s | 829.431 ms | 3.40 GiB | +2.66 GiB |
| `facts.batch_assemble` | 32.820 s | 32.820 s | 6.56 GiB | +555.86 MiB |
| `m2m3.publish_summaries` | 11.872 s | 11.872 s | 3.56 GiB | +263.81 MiB |
| `m4.local_analysis` | 10.140 s | 10.140 s | 417.58 MiB | +303.80 MiB |
| `m6.cpg_projection` | 2.180 s | 2.180 s | 3.00 GiB | +59.47 MiB |
| `wpa.graph_build` | 354.241 ms | 354.241 ms | 3.62 GiB | +0 B |

Inside `facts.publish`: `sink.fact-store` 80.271 s, `validate` 39.726 s. Inside
`wpa.orchestrate`: `canonicalize` 66.056 s, `execute` 42.034 s, `materialize`
33.260 s, `cache_lookup` 279.324 ms.

The two costs that matter — `facts.publish` at 120.0 s and `wpa.orchestrate` at
157.3 s — are 66% of the run between them, and `canonicalize` at 66.1 s is
larger than Soufflé execution at 42.0 s.

### 2.3 Stack-attributed profile

Four main-thread `sample` captures inside `wpa.orchestrate`, attributed to the
innermost named frame:

| Frame | Share |
| --- | ---: |
| `EncodeSemanticKey` | ~17% |
| result scan-back (`ScanOutput` → dense→stable mapping → row validation) | ~19% |
| `ResultCanonicalizer::Canonicalize` interior | ~16% |
| `WpaInputMaterializer::Build` interior | ~11% |
| `ComputeSHA256` | ~13% |
| remainder (`orchestrate` bookkeeping, `DeriveFactId`) | ~24% |

Eight captures inside `facts.publish` attribute its ~19,000 leaves:

| Frame | ≈ time |
| --- | ---: |
| `FactStore::Publish` container work | ~34 s |
| `AnalysisFactBus::Validate` | ~24 s |
| `ComputeSHA256` | ~16 s |
| `DeriveBatchId` | ~15 s |
| `DeriveWitnessId` | ~11 s |
| proto serialization + hex encoding + SQLite | ~14 s |

These shares are one run's captures and select the design's targets; section
9.4 is what decides whether acting on them paid.

### 2.4 The representation gap

Every component's complete canonical result — all facts and witnesses, stable
identifiers included as canonical text — occupies **196 MiB** in the
content-addressed object store, written by `SerializeResult` in
`src/wpa/WpaRunRepository.cpp`. The same content costs **+2.39 GiB** resident
while the run holds it in `WpaRunResult::completed_components[*].result`.

That is a **12.5×** gap with no information difference between the two forms.
The cause is the in-memory object shape, not the data:

- a `SemanticCellValue` is a `std::variant` of eight alternatives, so every cell
  occupies the variant's largest alternative whatever it actually holds, and a
  `core::StableId` cell additionally owns a 64-character `std::string` on the
  heap;
- a `SemanticRow` owns a `std::vector<SemanticCellValue>`, one heap block per row;
- a `WitnessEdge` owns **two** complete `SemanticRow` values plus two more
  strings, so every witness edge is three or more heap blocks;
- a `RootedInputFact` adds five more strings to a fact.

Two independent measurements agree on the size of the term: `wpa.orchestrate`
adds +2.39 GiB, and the whole run's component payloads serialize to 196 MiB.

## 3. Root Causes

### 3.1 The compact form already exists and is not used in memory

`SerializeResult` / `DeserializeResult` already encode exactly this content
losslessly — relation, cell count, per-cell variant tag, and each cell's value —
and `LoadReusableComponent` already revalidates every fact identity and both
recomputed hashes before a cached result is accepted. The pipeline serializes
each component once, stores it, and then keeps the rich copy alive instead.

This is the round's central finding and the one round 3 did not reach. Round 3
section 7.7 tested releasing the rich copy and **reloading** it, and measurement
rejected that design (section 12.2). Round 4 does not release the payload; it
keeps all of it, in a form 12.5× smaller.

### 3.2 Two stages still key containers and comparators on encoded text

`EncodeSemanticKey` is ~17% of the WPA phase's leaves and ~16 s of the publish
phase's.

- `ResultCanonicalizer::Canonicalize` (`src/facts/ResultCanonicalizer.cpp`) holds
  a three-level nested `std::map<std::string, …>` keyed by the encoded semantic
  key, orders results and facts with comparators that call `EncodeSemanticKey` on
  **every comparison**, and relaxes derivation costs by sweeping every
  derivation for up to `results.size()` rounds with two string-keyed map lookups
  per input. Round 3 never touched this file.
- `FactStore::Publish` (`src/facts/FactStore.cpp`) groups witnesses in a
  `std::map<std::string, ResultWitness>` keyed by the encoded result key, and
  holds `std::set<core::StableId> fact_ids`, `std::map<core::StableId, …>
  witness_id_by_fact`, and `std::map<core::StableId, …> root_evidence`.

Round 3 did land `perf(facts): memoize row identity and write the preimage in one
pass` (`cf8a22a`) in `FactStore`, and `perf(facts): order facts and witnesses by
packed ranks` (`0ae4ec6`) in `MakeAnalysisFactBatch`. Those two mechanisms are
the precedent round 4 reuses; the container choices above are what remains.

### 3.3 Verifying a batch re-derives rows the pipeline already derived

`AnalysisFactBus::Validate` recomputes `DeriveBatchId` over every row and then
re-derives each fact's identity and both endpoints of every witness edge. Round
3 measured 9,561,990 SHA-256 computations of already-derived rows across the
process and memoized what it could. `DeriveBatchId` remains ~15 s and
`Validate`'s own container work ~24 s.

Round 3 kept the derivation because it is a genuine integrity check, and round 4
keeps it for the same reason. What round 4 removes is the cost of the containers
it consults, not the check.

### 3.4 Successor support decodes more than it consumes

`WpaOrchestrator::SuccessorSupport` walks every fact of every successor SCC and
copies matching rows out, materializing each copied row's cells. It needs only
the relations the component kind derives.

### 3.5 Consequences that do not scale

Every term above grows with the run's derived-fact count: the rich payload
retention, the rich assembled batch, the nested string-keyed containers, and the
per-row re-derivation. A project producing ten to twenty million facts
multiplies each of them, in a representation already 12.5× larger than
necessary. This is why the round is scoped by the representation and not by the
fixture's threshold.

## 4. Acceptance Framing

Round 4 is accepted on **measured per-term reductions**, each proven with the
byte-identical content instrument of section 9.1, plus a stated projection of how
each term scales with the derived-fact count. This is the decision recorded for
this round, and it supersedes #133's absolute thresholds for the purpose of
judging this change:

- The **375 s wall criterion** was shown unreachable as written by round 3:
  CPU alone was 421.67 s then and 418.702 s now, so no wall-clock change can
  satisfy it, and wall time on this machine measures swap. Round 4 targets CPU,
  which is the load-independent quantity.
- The **4 GiB peak criterion** sits behind a measured 3.21 GiB post-SVF floor
  that round 3 proved is not reclaimable, and round 3 recorded that reopening it
  is a separate decision. Round 4 reports the peak it achieves and the term
  attribution behind it; it does not claim the threshold.

Both absolute criteria are recorded here as reopened rather than as carried. The
reopened decision belongs to the repository owner, not to this change.

## 5. Non-Goals

- Do not release, evict, or reload component payloads. Section 12.2 records the
  measurement that rejected that design.
- Do not change SVF construction, Andersen analysis, MemorySSA, or SVFG
  construction. The post-SVF floor is ~3.21 GiB and is not reclaimable.
- Do not change `relations.v2`, `summary.v2`, rule bundles, model bundles,
  epistemic states, fact identity, or witness selection.
- Do not change the published database schema, the hex-encoded `cells_hex`
  storage format, or any on-disk cache object format. The arena is an in-memory
  representation only.
- Do not change the production engine, the four WPA domains, reverse-topological
  SCC execution, or component caching.
- Do not remove or weaken `Validate`'s integrity checks, including the
  `DeriveBatchId` recomputation; section 3.3 says what stays and why.
- Do not stream publication to the fact store in this change; that is the
  successor stage in section 10.
- Do not add a performance-only CLI flag, a process-global cache, RTTI, or
  exceptions.

## 6. Preserved Contracts

1. `DeriveBatchId` remains byte-identical for unchanged input. It hashes
   `AppendSemanticKey(row)` per row in canonical order; the arena reproduces that
   encoding exactly, so the batch id — and the published `BatchId` — do not move.
2. Canonical fact and witness order, the canonical-owner rule, uniqueness, and
   the single selected witness derivation per fact are unchanged.
3. Every `(SccId, WpaComponentKind)` remains an independently materialized,
   cached, executed, canonicalized, and published component; reverse-topological
   order is unchanged.
4. `LogicalInputHash`, `FixpointHash`, and `ExternalHash` remain byte-identical.
5. Round 3's equivalence instrument must report equality, not merely matching
   row counts.
6. Dense-id assignments continue to depend only on the stable-ID set, never on
   discovery order.
7. Error paths continue to use `Status` and `StatusOr`; no RTTI, no exceptions.
8. Every modified file retains its Apache-2.0 header.

## 7. Design

### 7.1 The row arena

Introduce a compact, append-only, arena-backed store for semantic rows and
witness edges, reusing the encoding `SerializeResult` already defines. One arena
owns a contiguous byte buffer; each row is a byte range in it. Handles are
offsets, not pointers, so they stay valid across reallocation.

```cpp
// A stable handle into a RowArena. Never a pointer: the arena's buffer moves.
struct RowHandle { std::uint64_t offset = 0; std::uint64_t size = 0; };

class RowArena {
 public:
  // Appends one encoded row; returns its handle.
  RowHandle Append(const facts::SemanticRow& row);
  // Materialises a row. Allocates, so callers decode one row at a time.
  facts::SemanticRow Decode(RowHandle handle) const;
  // Appends the row's canonical semantic key to `out`, exactly as
  // AppendSemanticKey derives it from the rich row.
  void AppendKey(RowHandle handle, std::string* out) const;
  bool RowEquals(RowHandle left, RowHandle right) const;
  std::size_t size() const;
};
```

`Decode` exists for consumers that need a rich row — `ToProtoFact`, row
renderers, tests — and is never called in a loop that retains its result.

### 7.2 Payload retention

`WpaComponentResult` keeps its identity and hash fields and holds its payload in
a `RowArena` instead of two rich vectors. `WpaComponentCompletion` is unchanged
in shape; `result_object_key` continues to name the immutable store object.

`WpaOrchestrator` retains each completion's arena — not a decoded copy — so the
run's resident payload becomes the compact form, which section 2.4 measures at
196 MiB for this fixture. Nothing is released and nothing is reloaded, so
execution order, failure isolation, component caching, and owner selection are
untouched, and none of round 3 section 7.7's hazards can arise.

`SuccessorSupport` reads the relations that kind derives directly out of each
successor's arena and materializes only those rows.

### 7.3 Batch assembly

`AnalysisFactBatch` keeps its logical fields and holds canonical facts and
witnesses in arenas. Consumers iterate a decoded view:

```cpp
// Yields one decoded row per step; the source of truth stays in the arena.
class FactRange { public: class iterator; iterator begin() const; iterator end() const; };
FactRange facts() const;
FactRange witnesses() const;
```

`MakeAnalysisFactBatch` continues to sort completions by canonical key, select
the canonical owner of each fact, and move the selected derivations; it appends
into arenas instead of vectors. Round 3's packed ranks are retained.

### 7.4 Id-keyed downstream

- `ResultCanonicalizer` replaces its three nested `std::map<std::string, …>`
  levels with dense identifier keys from one interner built over the component's
  rows, precomputes each row's sort rank once so no comparator encodes, and
  replaces the cost relaxation's full sweep with a worklist that revisits a
  derivation only when the cost of one of its inputs decreases.
- `FactStore::Publish` groups witnesses by interned result rank rather than by
  encoded key string, and keys its fact, witness and root lookups on dense
  identifiers.
- `AnalysisFactBus::Validate` keys its fact index and root membership on
  precomputed 64-bit key hashes with exact arena byte comparison on collision,
  and stores the witness DAG's dependencies as one flat CSR array pair instead
  of `std::vector<std::vector<std::size_t>>`.

### 7.5 Data flow

Unchanged end to end: ingest → local analysis → SVF → summaries → WPA
components → canonicalized component results → batch → fact store. What changes
is that every arrow after canonicalization carries a compact arena rather than a
rich object graph, and each row's canonical key is encoded once per stage.

## 8. Error Handling

- An arena append of a row whose relation or cell count does not match
  `relations.v2` returns `InvalidArgument`, as `AppendRow`'s callers do today.
- A `RowHandle` outside the arena's byte range returns `InvalidArgument` and
  never reads out of bounds; the decode path range-checks before it parses.
- `Validate` retains every check and every message: a batch that fails today
  fails identically, in the same order, with the same diagnostic. In particular
  the closed-witness check and the expected/completed component equality are
  unchanged — round 3 section 7.7 records that a design which released payloads
  could publish an *empty* batch that `Validate` accepts, and that the guard
  against it was an `assert` that `NDEBUG` would drop. Round 4 retains all
  payloads, so it cannot produce that state; the checks stay regardless.
- No new failure mode is introduced on any path that currently succeeds.

## 9. Verification Strategy

### 9.1 Content equivalence

Round 3's equivalence instrument is the standard for this change and is used
unchanged rather than re-derived: the ordered-dump A/B pair over the published
store, under both `fact_id` and `rowid` ordering, including the per-component
`input_hash` / `fixpoint_hash` / `externally_visible_hash` comparison across all
13,716 components. `veritas-store-diff` implements the store-level comparison.

The round is accepted on that instrument reporting equality, not on row counts.

### 9.2 Unit tests

- `RowArenaTest` (new): round-trip of every cell alternative, `AppendKey`
  equality with `AppendSemanticKey` on the rich row, `RowEquals` against
  `SemanticRow::operator==`, and a rejected out-of-range handle.
- `ResultCanonicalizerTest`: the rank sort orders identically to the encoded-key
  comparator on a corpus including duplicate keys and ties; the worklist cost
  relaxation converges to the same costs and rejects the same unrooted cycles.
- `AnalysisFactBusTest`: the batch id is unchanged with the arena in place; the
  canonical owner, uniqueness, and witness selection are unchanged under
  reversed component order; arena-backed and rich assembly agree row for row.
- `FactStoreTest`, `ProvenanceStoreTest`, `VeritasExplainTest`,
  `WpaEndToEndTest`: unchanged expectations, proving the representation change is
  invisible to every consumer.
- `WpaOrchestratorTest`: successor support is unchanged when served from arenas.

### 9.3 Qualification

Run the targeted WPA, fact-bus, end-to-end, conformance, and M9 entry-gate
tests. No test may be skipped: a green CTest summary does not prove a test ran,
so `tests/qualification/check_no_skips.py` and `tools/check_m9_entry.py` are
mandatory. Round 3's bar was 811 of 811 with no skips; round 4 must not lower it.

### 9.4 Acceptance measurement

Measured as a controlled pair on one build tree, rebuilt between revisions, with
a fresh output directory each. Following round 3:

- **CPU, not wall.** Report worst-of-three CPU (user + sys). Round 3 established
  that wall time on this machine measures swap.
- **Instructions retired, where available.** Round 3 used it as the
  load-independent instrument; a per-process hardware count cannot be produced
  by contention.
- **Peak RSS as worst-of-three**, against this fixture's 0.72 GiB run-to-run
  spread, with peak physical footprint reported alongside because the two differ
  by ~1.4 GiB at the peak.
- **Per-term resident deltas**, taken from the phase recorder's per-span
  `rss_start` / `rss_end`, not only from the process total.

Acceptance is that the payload terms move by roughly the measured representation
gap and that CPU falls; each claim is reported with its own instrument, and a
term that does not move is reported as not having moved.

### 9.5 Measured outcome (2026-10-01)

**The round does not meet the acceptance framing of section 4.** CPU rose and the
larger of the two payload-time terms rose with it. On memory, of the three payload
terms one moved **down** (`wpa.orchestrate`, thinly), one moved **up** robustly
(`facts.batch_assemble`), and one is **withdrawn** because its own run-to-run
spread swallows the difference (`facts.publish`); the process peaks do not move
measurably at all. This section records the pair, the equivalence result, and
every term, including the ones that did not move.

#### 9.5.1 The pair and the protocol

Both revisions were built **in one build tree**, each run three times against a
fresh, non-existent output directory with the phase recorder enabled:

```bash
# pre-change: 59198f5, the round's branch point, rebuilt in this tree
# post-change: 6a7635b, this round's head, after a clean full rebuild
OUT=/tmp/veritas-round4-<rev>-<n>   # does not exist before the run
/usr/bin/time -lp <icount wrapper> ./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output "$OUT" --metrics true --metrics-interval-ms 250 \
  --metrics-top-n 15 --metrics-series true
```

Six runs, all exit 0, all on the documented Debug configuration, all in one
session at load 1.3-2.0 with 1,262 MiB of a 2,048 MiB swap file in use before,
during and after. **Every figure below is worst-of-three within its revision**,
round 3's convention for a fixture with a measured run-to-run spread.

**The run order is post, pre, post, and that is the protocol's consequence, not
an oversight.** One build tree has one `build/bin/veritas-build`, so measuring
two revisions in it forces a blocked design: the runs cannot interleave, and a
machine-state drift across the session lands entirely on one side. The blocks
are, from each run's own records:

| Block | Window (UTC) | Runs |
| --- | --- | --- |
| post, first block | 15:58:25 - 16:25:04 | post-1, post-2, post-3 |
| pre | 16:28:22 - 16:52:08 | pre-1, pre-2, pre-3 |
| post, profiled block | 17:06:00 - 17:39:04 | post-4, post-5, post-6 |

**The profiled block is the reversal control.** It reruns the post revision
*after* the pre block and reproduces the first block, so the CPU and instruction
gaps are not a drift that accumulated while the pre block ran:

| Block | `run` CPU | instructions retired |
| --- | --- | --- |
| post, first block | 490.382 / 488.329 / 488.211 s | 8.5239 / 8.5099 / 8.5045 e12 |
| pre | 420.231 / 411.234 / 410.057 s | 7.3537 / 7.2868 / 7.2736 e12 |
| post, profiled block | 491.015 / 495.581 / 488.633 s | 8.5325 / 8.5656 / 8.5104 e12 |

Post is equally slow on both sides of the pre block: its worst-of-three CPU is
1.06 % *higher* after the pre block than before it, so the drift, such as it is,
runs against the finding rather than for it. Across all six post runs the CPU
spread is **7.370 s** and the instruction spread is **0.061e12**, against a
70.151 s CPU gap and a 1.170e12 instruction gap. **This is the strongest evidence
in the task**: the sign of every CPU and instruction delta below is a property of
the revision, not of when the run happened.

The instruction count is Darwin's per-process hardware counter,
`ri_instructions` from `proc_pid_rusage(pid, RUSAGE_INFO_V6)`, read by a wrapper
that forks the analyzer and polls the child's rusage every 100 ms, reporting the
last reading taken before exit. The counter is real for this instrument: a
10M/100M/1G-iteration busy loop reports 7.07e7 / 6.10e8 / 6.01e9 instructions,
scaling linearly. It cannot be obtained without root through `powermetrics`, and
the product execs no worker process on the production path, so a single-process
count is the whole run's count. Two properties were checked rather than assumed.

macOS propagates a grandchild's maximum resident set size through `wait4`: a
direct run of a probe that touches 200 MiB reports 211,435,520 B and the same
probe under the wrapper reports 211,419,136 B, so the wrapper does not hide the
analyzer's peak.

The phase recorder's `memory.peak.rss_bytes` and `/usr/bin/time`'s `maxrss` are
two independent readings, and **they agree exactly in five of the six measured
runs — and in all three profiled runs, so eight of nine**. The exception is
post-1, where the recorder reports 7,376,060,416 B against `time`'s
7,447,281,664 B, short by 71,221,248 B (0.066 GiB). The recorder samples every
250 ms and the run's peak falls between its last sample and process exit, so the
disagreement is the sampler's interval, not a disagreement about the value. It is
recorded here rather than smoothed over because post-1 is *also* the one run whose
memory behaves differently in section 9.5.4, and a reader is entitled to both
facts together.

#### 9.5.2 The pre-change member reproduces section 2.2

The pair is only interpretable if the pre-change revision measured here is the
one section 2 measured. It is, on every phase:

| Span | Section 2.2 (`59198f5`, another tree) | This pair, pre-1 | Δ |
| --- | ---: | ---: | ---: |
| `run` | 419.178 s | 421.515 s | +2.337 s |
| `facts.publish` | 119.998 s | 127.994 s | +7.996 s |
| `wpa.orchestrate` | 157.318 s | 154.641 s | −2.677 s |
| `m5.svf` | 74.847 s | 73.329 s | −1.518 s |
| `facts.batch_assemble` | 32.820 s | 32.229 s | −0.591 s |
| `m2m3.publish_summaries` | 11.872 s | 11.589 s | −0.283 s |
| `m4.local_analysis` | 10.140 s | 9.845 s | −0.295 s |
| `m6.cpg_projection` | 2.180 s | 2.127 s | −0.053 s |
| `wpa.graph_build` | 0.354 s | 0.351 s | −0.003 s |
| `canonicalize` | 66.056 s | 65.230 s | −0.826 s |
| `execute` | 42.034 s | 41.180 s | −0.854 s |
| `materialize` | 33.260 s | 32.665 s | −0.595 s |
| `validate` | 39.726 s | 38.658 s | −1.068 s |
| `sink.fact-store` | 80.271 s | 89.335 s | +9.064 s |
| CPU (from `run`) | 418.702 s | 420.231 s | +1.529 s |

Every phase reproduces within 10 s and almost all within 3 s; the two largest
deviations, `facts.publish` at +8.0 s and `sink.fact-store` at +9.1 s, are on the
span section 2.3 already attributes mostly to `ComputeSHA256`, `DeriveBatchId`
and SQLite, i.e. the terms that move with machine state rather than with the
build. Peak RSS is the other figure that does not reproduce — 7.03 GiB then,
7.92 GiB in pre-1 and 7.96 GiB worst-of-three — and section 2.1 already records
that its 7.03 GiB is one observation inside a spread and not a peak.

#### 9.5.3 Equivalence: the form used, and why

The section 9.1 instrument was run in round 3 section 9.1's **Step 2a** form —
"identity unchanged", the strong form — and not in Step 2b. Step 2b exists to
concede that identity columns move between two builds; they do not move here.
Round 4 changes no file compiled into either hashed Soufflé library
(`SouffleRunner.cpp`, `SouffleSemanticKeyFunctor.cpp`, `SemanticKeyCodec.cpp`),
so the toolchain identity is unchanged, and with it every run-scoped column.
That is checked first, from the two runs' own metrics artefacts:

| Identity coordinate | Value in both revisions |
| --- | --- |
| `run_id` | `run:sha256:04df8175595dc38b56cd304b70fb08d92a785818b42e3263d3d0c518e67fa5d1` |
| `batch_id` | `fact:sha256:2216f98ad0c4d7aae606e966371d0870dc58988ad7c01fdee237007ea91ac67d` |
| `repository_id` | `repo:sha256:514e8958d4a5b803bc1325c5b63e64a619fef868c2ee8b8415945ba5c64ddc0f` |
| `projection_id` | `cpgproj:sha256:e569ebf98cf26545487730a1581287a005c52582d4c4b8cb5e174efc8f27971f` |
| `revision_id` | `rev:sha256:30ba85d6f2c3cfc5dfe77c2d2927aeeb0e4e0a7b257d5cfa7569f8a08d81cc1e` |
| `build_variant_id` | `bv:sha256:9a44504b2a3bcd664ab916427e277d87d3cccdb4ae7eff1aeec9d760c870573d` |
| `svf_config_hash` | `7b73593f52ef07ea056d435566969754e5c6846936e6a5193f5fc220d43140f6` |
| `wpa_config_hash` | `59a176893f32081fdc57556d8e9c2bb17be683932b311578119a6126024ca553` |
| `engine_toolchain_identity` | `souffle-a4cd7e4fcbee4241d278eab67b905ece4dbb03998ef096e1a870f881be5c53de` |

All nine coordinates are byte-identical, `batch_id` included. Since
`DeriveBatchId` hashes the canonical key of every row in order, an identical
batch id over 1,249,792 facts and 1,375,911 witness edges is the round's
strongest single equivalence result: the arena's encoding, its canonical order
and the ownership rule all reproduce byte for byte.

`veritas-store-diff` compares the two stores through the recorded projection and
reports equality:

```
$ ./build/bin/veritas-store-diff /tmp/veritas-round4-pre-1 /tmp/veritas-round4-post-1
stores are equivalent (37 tables compared)          # exit 0
```

The round's implementation plan writes that command as
`veritas-store-diff <pre>/metadata.db <post>/metadata.db`; the tool takes store
*roots* — the directories handed to `--output` — and appends `metadata.db`
itself.

**Both dump orderings, on all four published tables.** The recorded projection
orders `analysis_facts` by `fact_id` and the other three by `rowid`, so
`veritas-store-diff` alone exercises one ordering per table. The ordering half
was therefore closed separately, with `sqlite3` projections ordered by `rowid`
for **all four** tables, over the recorded column sets with the recorded
exclusions (`run_id`; `analyzer_run_id` and `binding_id` for
`run_fact_bindings`), escaped with the instrument's own rule so the stream stays
unambiguous where a value carries a newline:

| Projection (`ORDER BY rowid`) | Rows | Digest, identical in both revisions |
| --- | ---: | --- |
| `analysis_facts` | 1,249,792 | `6b0aea6381242e301ddfb993c7048b213037f9aafc18c7acd3e23ac894ab8ed1` |
| `run_fact_bindings` | 752,076 | `d732ec43ca21a5967170139f69f3688145c2771b7a079b78ef7b26a6eddf3b63` |
| `provenance_nodes` | 752,076 | `d8410e270ed19257491a830e9debb666299be54365482b83236ac06cbacdf321` |
| `provenance_edges` | 1,375,911 | `6691f96f5b5f137e29f9ce0aeb4ddf4380c2da4f15ec3041a6f5344daa98a038` |

Every one of the four row-stream files is **byte-identical** between the two
revisions (`cmp`), not merely equal in digest, and every one reproduces the
literal round 3 section 9.1 recorded for it. `analysis_facts` also reproduces
under the recorded projection's own `fact_id` ordering,
`452a850ec90ab192a2e5cde28269067f06f4618338675c26c870cac466e41cda`, in both
revisions.

This is the half Task 6's `semantic_zoo` A/B could not close. There, the witness
arrival order already coincided with encoded-key order, so a rowid-ordered
projection could not tell rank-order visiting from arrival-order visiting. On
this workload it can: `rowid` order is the physical order the batch hands to
`FactStore`, and across ~1.25M facts and ~1.375M witness edges any change in
visit order would move it. It did not move.

**Per-component hashes, all 13,716 components.** The section 9.1 instrument also
requires the per-component comparison. The names section 9.1 uses —
`input_hash`, `fixpoint_hash`, `externally_visible_hash` — are the v1
`wpa_component_states` columns; the v2 table names the same three quantities
`logical_input_hash`, `fixpoint_hash`, `external_hash`. Both were compared, and
since the identity did not move, `run_id` was **included** rather than excluded:

| Comparison | Rows | Digest, identical in both revisions |
| --- | ---: | --- |
| `wpa_component_states` (v1) + `revision_id`, `build_variant_id`, `iteration_count`, `status` | 13,716 | `51abd038fb3da2b70eb0d1df9478f67946a19f2c351c89afee0bca3bce98ceda` |
| `wpa_component_states_v2` + `run_id` | 13,716 | `ce6d62ad36699b004920e83dd9dabdbfae3fbc2728e096481b5832efcd164ba5` |
| `wpa_component_states_v2`, the three hashes alone | 13,716 | `c14be4d9337e7eb2492589b7ebf1d11148e0159d8d911f78dc3e930504b53275` |
| `wpa_component_result_cache_v2` | 13,716 | `e5c405ad032085f53f33fbd6669ea15dc4ec19140c4776d8249d27cef97fe4df` |

**The instruments can fail.** A one-cell perturbation — `cells_hex || 'f'` on
`rowid` 1 of `analysis_facts`, row count unchanged — makes `veritas-store-diff`
exit 1 and name the table (`differs: analysis_facts (left 1249792 rows, right
1249792 rows)`), and moves both `analysis_facts` digests. A digest that cannot
change would prove nothing; these change on one cell.

#### 9.5.4 The terms

CPU and wall, worst of three, with the hardware counter beside them:

| Term | pre | post | Δ | % |
| --- | ---: | ---: | ---: | ---: |
| CPU (user+sys, `run` span) | 420.231 s | 490.382 s | **+70.151 s** | **+16.7 %** |
| CPU (user+sys, `/usr/bin/time`) | 420.970 s | 491.100 s | +70.130 s | +16.7 % |
| Wall | 422.170 s | 491.490 s | +69.320 s | +16.4 % |
| Instructions retired | 7,353,722,014,589 | 8,523,927,367,036 | **+1,170,205,352,447** | **+15.9 %** |

The per-run figures show this is not spread:

| Run | `run` CPU | instructions | `maxrss` | peak footprint |
| --- | ---: | ---: | ---: | ---: |
| pre-1 | 420.231 s | 7.3537e12 | 7.918 GiB | 5.980 GiB |
| pre-2 | 411.234 s | 7.2868e12 | 7.893 GiB | 5.936 GiB |
| pre-3 | 410.057 s | 7.2736e12 | 7.961 GiB | 6.062 GiB |
| post-1 | 490.382 s | 8.5239e12 | 6.936 GiB | 5.864 GiB |
| post-2 | 488.329 s | 8.5099e12 | 7.794 GiB | 6.286 GiB |
| post-3 | 488.211 s | 8.5045e12 | 7.420 GiB | 5.574 GiB |

The pre revision spans 10.2 s of CPU and 0.080e12 instructions; the post
revision spans 2.2 s and 0.019e12. The 70.2 s CPU gap is seven times the wider of
those two spreads and the 1.17e12-instruction gap is fifteen times the wider
instruction spread, so **CPU did not fall: it rose**, and the load-independent
counter agrees.

Memory, worst of three:

| Term | pre | post | Δ | % | Moved? |
| --- | ---: | ---: | ---: | ---: | --- |
| Maximum resident set size (`/usr/bin/time`) | 8,548,581,376 B (7.961 GiB) | 8,368,816,128 B (7.794 GiB) | −179,765,248 B (−0.167 GiB) | −2.1 % | **No** — inside the post spread of 0.858 GiB |
| Peak physical footprint (sampler) | 6,509,468,344 B (6.062 GiB) | 6,749,067,960 B (6.286 GiB) | +239,599,616 B (+0.223 GiB) | +3.7 % | **No** — inside the post spread of 0.712 GiB; it *does* exceed the pre spread of 0.126 GiB, so this row is the one place the two spreads disagree |
| Peak RSS (sampler) | 7.961 GiB | 7.794 GiB | −0.167 GiB | −2.1 % | same as `maxrss` |

The peak-RSS reduction is **not established**: −0.167 GiB is far inside the post
revision's own 0.858 GiB run-to-run spread, and this section does not claim it.
The three pre runs cluster within 0.068 GiB (7.893-7.961) while the three post
runs span 0.858 GiB (6.936-7.794); the post revision's best run is 0.96 GiB below
the pre revision's best, and its worst is 0.17 GiB below the pre revision's
worst. **Most of that post spread is post-1**, whose recorder reading is 0.066 GiB
below its own `/usr/bin/time` reading (section 9.5.1) — and post-1 is also the run
whose `facts.publish` delta is the outlier in the table below. Worst-of-three is
what makes the process peak comparable at all, and it is also what hides these two
facts; both are stated so a reader can weigh the trough rather than only the
worst case. Peak footprint did not improve either: worst-of-three it is +0.223 GiB
against a 0.712 GiB post spread, so under this section's criterion it did not move
in either direction, while its spread widened from 0.126 GiB. Neither absolute
threshold in section 4 is claimed here: the 375 s and 4 GiB criteria remain
reopened.

Per-span wall time, worst of three. The nested-inclusive rows are children of the
row above them:

| Span | pre | post | Δ | % |
| --- | ---: | ---: | ---: | ---: |
| `run` | 421.515 s | 490.805 s | +69.290 s | +16.4 % |
|  `run` (self) | 7.826 s | 7.046 s | −0.780 s | −10.0 % |
| `facts.batch_assemble` | 32.236 s | 79.942 s | **+47.706 s** | **+148.0 %** |
| `facts.publish` | 127.994 s | 145.135 s | +17.142 s | +13.4 % |
|  `facts.publish.validate` | 38.684 s | 52.371 s | +13.687 s | +35.4 % |
|  `facts.publish.sink.fact-store` | 89.335 s | 92.763 s | +3.428 s | +3.8 % |
| `wpa.orchestrate` | 155.120 s | 160.680 s | +5.560 s | +3.6 % |
|  `wpa.orchestrate` (self) | 15.437 s | 46.736 s | **+31.299 s** | **+202.7 %** |
|  `wpa.component.canonicalize` | 65.337 s | 36.192 s | **−29.145 s** | **−44.6 %** |
|  `wpa.component.execute` | 41.343 s | 41.455 s | +0.112 s | +0.3 % |
|  `wpa.component.materialize` | 32.740 s | 36.241 s | +3.501 s | +10.7 % |
| `m5.svf` | 73.481 s | 73.871 s | +0.390 s | +0.5 % |
| `m4.local_analysis` | 9.953 s | 10.096 s | +0.143 s | +1.4 % |
| `m2m3.publish_summaries` | 11.589 s | 11.600 s | +0.011 s | +0.1 % |
| `m6.cpg_projection` | 2.135 s | 2.134 s | −0.002 s | −0.1 % |
| `wpa.graph_build` | 0.354 s | 0.352 s | −0.002 s | −0.5 % |

Read plainly: **`canonicalize` fell 29.1 s — the round's one measured time win —
and three other terms rose by 92.7 s between them.** The phases the round does
not touch (`m5.svf`, `m4`, `m2m3`, `m6`, `graph_build`, `execute`, and the `run`
span's own self time) are unchanged within 0.8 s, which is the control that makes
the rest interpretable.

**The criterion, stated once and applied to every memory term below.** A term has
moved only if *both* hold: its **worst-of-three** difference exceeds the **post
revision's own run-to-run spread** for that term, *and* that difference is at
least **0.05 GiB**. Worst-of-three is round 3's convention, and the post spread is
the right yardstick because it is the revision whose behaviour is in question; the
pre spread is reported beside it so a reader can see when the two disagree.

The magnitude floor is stated rather than left implicit because two rows below —
`m5.svf` and `m4.local_analysis` — clear their spreads by 1.2× and 1.1× while
amounting to 14.2 MB and 7.4 MB, which is not a design quantity at this phase
scale. 0.05 GiB sits 3.8× above the larger of those two and 8.0× below the
smallest effect claimed here (`wpa.orchestrate`'s peak), so it separates them
without arbitrating anything else. Nothing below is called a reduction on a
difference that fails either half, and the peak-RSS term in the table above is
dismissed by the spread half of the same rule.

Per-term resident deltas, from each span's `rss_start` / `rss_end`. This is the
section's central claim and it is why the process total is not the evidence.
Every run's delta is shown, because the spread is what decides:

| Span | Δ resident, pre (three runs) | pre spread | Δ resident, post (three runs) | post spread | worst-of-three Δ | Moved? |
| --- | --- | ---: | --- | ---: | ---: | --- |
| `wpa.orchestrate` | +2.039 / +2.110 / +2.153 | 0.113 | +1.738 / +1.399 / +1.436 | 0.338 | **−0.415 GiB** | **Yes, by 0.077 GiB** |
| `facts.publish` | +1.759 / +1.838 / +1.697 | 0.141 | **−2.257** / +1.081 / +1.172 | **3.429** | −0.666 GiB | **Withdrawn — see below** |
| `facts.batch_assemble` | +0.300 / +0.565 / +0.356 | 0.265 | +0.929 / +1.150 / +0.934 | 0.221 | **+0.585 GiB** | **Yes — it rose** |
| `m5.svf` | +2.668 / +2.644 / +2.643 | 0.024 | +2.652 / +2.654 / +2.643 | 0.011 | −0.013 GiB | No — clears the spread, below the 0.05 GiB floor |
| `m4.local_analysis` | +0.287 / +0.298 / +0.296 | 0.011 | +0.285 / +0.291 / +0.291 | 0.006 | −0.007 GiB | No — clears the spread, below the 0.05 GiB floor |
| `m2m3.publish_summaries` | +0.345 / +0.344 / +0.248 | 0.097 | +0.348 / +0.345 / +0.243 | 0.105 | +0.004 GiB | No — inside the spread |

**`facts.publish`'s −0.666 GiB is withdrawn.** Its post spread is 3.429 GiB —
the three deltas are −2.257, +1.081 and +1.172 GiB — so by the rule used above to
dismiss peak RSS this difference is not established, and
reporting it as one would be exactly the inconsistency the rule exists to
prevent. `post-1` is the run that makes the spread: it is the only one of the six
where `facts.publish` *released* memory, ending the span 2.257 GiB below where it
started, and it is the only one where the recorder's peak and `/usr/bin/time`'s
disagree. Worst-of-three hides both facts, which is why both are stated here.

**`wpa.orchestrate`'s −0.415 GiB clears the rule, but thinly**, and it is the only
one of the three payload deltas that does. Its margin is 0.077 GiB against a
0.338 GiB post spread. Under the stricter reading — the pre revision's *best* run
against the post revision's *worst* — it is 2.039 − 1.738 = 0.302 GiB against that
same 0.338 GiB, and does not clear; so this claim rests on the worst-of-three
convention alone and a reader who prefers the stricter test should treat it as
unresolved rather than as refuted.

**`facts.batch_assemble`'s +0.585 GiB is the one robust memory result in this
table**: it is 2.2× the wider of the two spreads. It rose because section 5
forbids releasing component payloads, so the batch's own arena is a second copy of
the payload living beside the component arenas rather than a re-homing of them.

The peak-resident instrument, from the same runs and by the same rule:

| Span | pre (three runs) | pre spread | post (three runs) | post spread | worst-of-three Δ | Moved? |
| --- | --- | ---: | --- | ---: | ---: | --- |
| `wpa.orchestrate` | 5.701 / 5.721 / 5.747 | 0.046 | 5.345 / 5.049 / 5.004 | 0.341 | **−0.402 GiB** | **Yes, by 0.061 GiB** |
| `facts.batch_assemble` | 6.242 / 6.287 / 6.401 | 0.159 | 6.275 / 6.202 / 5.939 | 0.336 | −0.126 GiB | No |
| `facts.publish` | 7.918 / 7.893 / 7.961 | 0.068 | 6.869 / 7.794 / 7.420 | 0.925 | −0.167 GiB | No — inside the spread |
| `m5.svf` | 3.407 / 3.393 / 3.391 | 0.016 | 3.391 / 3.400 / 3.390 | 0.010 | −0.007 GiB | No — inside the spread |

`wpa.orchestrate`'s peak is the strongest single memory result in the round:
−0.402 GiB worst-of-three, clearing the post spread by 0.061 GiB, on the phase
that holds the component results, and pointing the same way as its resident delta
(−0.415 GiB). Both are thin against a 0.341 GiB spread and a stricter
min-pre-vs-max-post reading gives 5.701 − 5.345 = 0.356 GiB against that same
0.341 GiB. **Two independent instruments on the same phase agree in sign and
magnitude, which is worth more than either margin alone, but neither is a large
effect on this fixture.**

#### 9.5.5 Where the time went

Three profiled runs took main-thread `sample` captures during the post revision,
section 2.3's method. They are profiled runs, not acceptance measurements. Shares
are summed per function rather than read off one node: `sample` splits a single
function into many sibling frames because it aggregates by return address, so
`MakeAnalysisFactBatch` appears as 33 nodes that together are the whole thread.

Inside `facts.batch_assemble` (three captures at t≈273, 278 and 303 s of a
79.8 s span; **100 % of the main thread is inside `MakeAnalysisFactBatch`** in all
three):

- the two selection-pass captures are **51.9 % and 52.1 %**
  `WitnessRange::Iterator::operator*` / `AnalysisFactRange::Iterator::operator*`
  — the row decode — with `RowArena::DecodeWitness` / `DecodeFact` at 51.0 % and
  51.4 %;
- the third, in the emit pass, is **61.3 % `RowArena::Append`** (appending every
  owned row into the batch's own arena, `AppendId`'s hex rendering included) and
  still 33.4 % decode.

The mechanism is in the diff of `MakeAnalysisFactBatch`. The selection pass
iterates each component's arena through `AnalysisFactRange` and dereferences it —
`const AnalysisFact &fact = *it;`, whose documented contract is "decodes this
entry, allocates the fact it returns and nothing else" — once per fact and once
per witness, to recover an id and a sort key. The pre-change code iterated
`completion.result.facts`, a vector of rich rows it already held, and read the
id and key off them with no decode at all, then *moved* each row into the batch.
So this pass pays 2.6M allocations the old one did not, and the emit pass pays
another decode plus an arena append where the old one moved a pointer.

Inside `facts.publish`, two captures cover `Validate` and the fact-store sink:

- every main-thread sample in the `Validate` capture is inside `DeriveBatchId`
  (100 %), and 90.1 % of the capture is inside `AppendStoredRowKey`, which splits
  almost evenly into two halves. **45.4 % of `AppendStoredRowKey` renders each
  row's key out of the batch's arena**, and **51.4 % hashes the rendered key**
  through `AppendField` → `SHA256Hasher::Update`. The render half's whole stack is
  `AppendStoredRowKey` → `AnalysisFactBatch::AppendWitnessRowKey` →
  `RowArena::AppendKey` → `WriteKey`: the accessor that parents every
  `RowArena::AppendKey` frame here is the **witness**-arena one
  (`AnalysisFactBus.h:94`), and `AppendFactKey` — its fact-arena sibling at `:91`
  — has no frame in this capture at all. `RowArena::AppendKey` is the largest named
  function on that path, at 40.9 % of the capture, and that figure is a **summed
  per-function share** (1835 of 4484 frames across five call sites), not a single
  frame. The distinction matters because the two aggregations disagree: **by tips
  the capture is dominated by the SHA-256 core — `RotR` 25.9 %, `ProcessBlock`
  23.9 % — and `AppendKey` contributes 10 samples.** The render does not decode:
  `ReadRow` and `DecodeCell` are absent from the capture entirely. Since
  `batch_id` is byte-identical between the revisions the hashed byte *stream* is
  identical too, so the hash half cannot itself be where 13.7 s went — **which
  leaves the render, new work on this round's rewritten path, as the suspect and
  the capture pointing at the round rather than away from it.** The original draft
  of this section read the opposite way: it called the capture "dominated by
  `AppendField` → `SHA256Hasher::Update`", generalising one frame's composition to
  the whole function and, worse, using it to conclude that the arena's key render
  was not implicated. The disclaimer stands: a 6-second capture inside a 52-second
  span cannot apportion the delta, and the pre revision was not profiled, so the
  +13.7 s is reported as measured and **not** attributed;
- the fact-store capture is 49.4 % `WitnessRange::Iterator::operator*` →
  `RowArena::DecodeWitness` (2201 of 4456 samples), with `FactIdentityMemo::
  Identify` at 26 % — the same decode-per-row shape, now in the consumer.

The `wpa.orchestrate` self term (+31.3 s) is likewise measured and not
attributed: its captures show `ResultCanonicalizer::Canonicalize`,
`WpaInputMaterializer::Build`, `SerializeResult` and `MakeResult` sharing the
span, with `WitnessRange::Iterator::operator*` and `RowArena::AppendWitness`
appearing under the last two, but no pre-revision capture was taken to say which
of them grew.

#### 9.5.6 Verdict

Section 4 accepts round 4 on measured per-term reductions proven with the section
9.1 instrument, and requires that the payload terms move by roughly the measured
representation gap **and that CPU falls**. Measured:

- the equivalence half **passes**, in its strongest form — every identity
  coordinate identical, both orderings of all four published tables
  byte-identical, all 13,716 per-component hashes identical, with working
  controls;
- the per-term memory half **passes only in part, and less than the first draft
  of this section claimed**: `wpa.orchestrate`'s peak moved −0.402 GiB and its
  resident delta −0.415 GiB, both clearing the post spread thinly (by 0.061 and
  0.077 GiB against a 0.341 and 0.338 GiB spread) and agreeing with each other;
  `facts.batch_assemble`'s resident delta grew a robust +0.585 GiB; and
  `facts.publish`'s −0.666 GiB is **withdrawn** because its own spread is
  3.429 GiB. The process peaks did not move measurably;
- the CPU half **fails**: +70.151 s (+16.7 %) and +1.17e12 instructions
  (+15.9 %), against a phase profile whose untouched phases are unchanged within
  0.8 s. Post is equally slow on both sides of the pre block, so the sign is the
  revision's and not the session's.

**This round is not accepted on section 4's framing as written.** The 29.1 s
`canonicalize` win is real and is the round's own; it is outweighed by 47.7 s in
batch assembly, 31.3 s in `wpa.orchestrate`'s own time and 13.7 s in validation,
all three on the path this round rewrote. The attribution in section 9.5.5 is
what a successor stage needs: the decode-per-row tax that the compact
representation introduces in every consumer of it, and which section 7.1's own
premise — "callers decode one row at a time" — priced at the wrong place. The
`validate` capture points at `RowArena::AppendKey`, the key render the arena
representation introduced, so that tax is not only a decode cost; a successor
should treat the render as a suspect too.

## 10. Successor Stages (not this change)

1. **Streaming publication.** Remove batch materialization: emit canonical rows
   from component payloads into the fact store as they are selected, holding
   ownership as dense identifiers. This removes the last O(batch) term. It is
   *not* the reload design of section 12.2 — nothing is re-read from the object
   store and no revalidation is repeated.
2. **The post-SVF floor.** The 3.21 GiB floor and the `ProgramIr` held across WPA
   grow with source size. Round 3 proved the floor is not reclaimable by
   returning freed pages, so this needs its own amendment.
3. **Soufflé program instantiation.** Round 3 dropped per-run program reuse on
   measurement; its measurement, not the idea, is what stands in the way.

## 11. Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Arena handles outlive their arena | Handles are offsets, not pointers; arenas are owned by the same object as the payload they describe; a decode is range-checked. |
| `AppendKey` diverges from `AppendSemanticKey` | A unit test asserts byte equality across every cell alternative; `DeriveBatchId` equality is the end-to-end guard. |
| A consumer descends to a rich vector and reintroduces the old cost | Consumers that only iterate use the decoded range; tests assert the publication path does not materialize every row. |
| The rank sort changes canonical order | Rank assignment preserves encoded-key order by construction; a test compares both orderings on a tie-heavy corpus. |
| The worklist relaxation changes a selected proof | It converges to the same costs and the same proof-selection inputs; the section 9.1 instrument is the end-to-end guard. |
| A representation change silently enlarges memory | Per-term resident deltas are measured, not only the process total. |
| The round is judged against 4 GiB and recorded as a miss | Section 4 records the reopened framing, so the round reports term reductions and does not claim the threshold. |

## 12. Rejected Alternatives

### 12.1 Constant-factor tuning inside the current shape

Rejected as insufficient. It was the shape of rounds 1–3 and it left the
remaining cost proportional to the rich object count, which is the property that
fails at scale.

### 12.2 Release component payloads and reload them during assembly

**Refuted on measurement by round 3 section 7.7, reverted in `deaead8`, and not
to be re-proposed.** Built and proven content-identical, then measured: CPU
429.77 → 538.78 s (+109.01 s), instructions retired +13.8%, peak RSS 7.99 →
8.29 GiB (+0.30 GiB), with `swaps 0` at 98.8% CPU so the sign is not a swap
artefact. The cost is `LoadReusableComponent`'s full revalidation — every fact
identity re-derived and both canonical hashes recomputed — 13,716 times, and the
premise was false: the subset successor support needs is ~60% of published facts
(748,647 of 1,249,792), not a small fraction. Round 4 keeps every payload
resident in compact form instead, which costs no revalidation and no reads.

### 12.3 A run-scoped key interner only, leaving the payload shape alone

Rejected as a half-measure: it removes the encode tax but not the 2.39 GiB
retention, which is the larger term.

### 12.4 Replace the hex-encoded `cells_hex` storage with the compact binary

Rejected here. It would shrink `metadata.db` (2.82 GiB), but it changes an
on-disk schema and every historical cache object, which section 5 excludes and
which is an independent decision.

### 12.5 Per-binding `is_current` update as the publish bottleneck

Hypothesised from reading `FactStore::AppendBinding`, measured, and refuted: one
no-match `UPDATE` against the partial unique index costs 1.9 µs on a fully built
store, so the 752,076 of them account for at most about 1.4 s of the 80 s sink.
Recorded so it is not re-proposed.

### 12.6 Reclaim allocator arenas at phase boundaries

Already refuted by round 3 section 3.6 and not re-proposed: freeing 800 MiB of
small-zone allocations left the resident set exactly unchanged, and
`malloc_zone_pressure_relief(nullptr, 0)` returned 0 bytes at every boundary in
two runs. Recorded because the `vmmap` reading that suggested it is a natural
mistake — the `resident` column was read where `dirty` was meant.

## 13. Documentation Repair (this change)

The round-1/2 specification lost three sections to commit `4493cc4`, which
deleted `## 10. Risks and Mitigations`, `## 11. Rejected Alternatives`, and
`## 12. Completion Criteria` while reporting round-2 measurements. Two defects
follow: its section 9.5.4 cites "rejected alternative 11.3", a section that no
longer exists, and the specification no longer states its own completion
criteria. Issue #136's record already notes that this citation does not resolve.
Restore the three sections verbatim so the citation resolves.

## 14. Completion Criteria

The work is complete only when:

1. All goals in section 7 are implemented and every contract in section 6 holds.
2. The section 9.1 instrument reports published content equal, including the
   batch id and all 13,716 per-component hashes.
3. The full clean build and the full CTest suite pass with no failures, skips, or
   not-run tests, and the M9 entry gate reports all ten criteria passing.
4. `git diff --check` and the repository license-header check pass.
5. A controlled before/after pair records CPU (worst of three), instructions
   retired, worst-of-three peak RSS and footprint, and the per-term resident
   deltas, for both revisions on one build tree.
6. Each section 4 term is reported with its own instrument, and any term that did
   not move is reported as not having moved.
7. The branch diff contains only this change, its tests, and its documentation.
