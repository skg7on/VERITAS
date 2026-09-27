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
