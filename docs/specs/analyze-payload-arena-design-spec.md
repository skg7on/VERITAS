# `veritas-build analyze` Payload Arena Design Specification

**Status:** Draft for review

**Tracking issue:** [#133](https://github.com/skg7on/VERITAS/issues/133)

**Predecessor:**
[`veritas-build-analyze-performance-design-spec.md`](veritas-build-analyze-performance-design-spec.md)
(tasks 1-3 merged; its task 4 acceptance benchmark is unmet)

**Implementation plan:**
[`../plans/analyze-payload-arena-implementation-plan.md`](../plans/analyze-payload-arena-implementation-plan.md)

## 1. Purpose

The predecessor specification removed four measured inefficiencies and left the
LevelDB acceptance benchmark unmet. This specification is the larger-ceiling
continuation: it attacks the **representation** the pipeline holds its semantic
payload in, because that representation is roughly an order of magnitude larger
than the information it carries and it is the term that grows with a project's
derived-fact count.

The motivating requirement is not the LevelDB fixture. LevelDB is 39 translation
units; the target is that the pipeline's memory and time scale to a project two
to three orders of magnitude larger. A design that reaches 4 GiB on LevelDB by
constant-factor trimming but still retains one rich object per derived fact would
not scale, so this specification is written against the scaling curve, not
against the fixture's threshold alone.

## 2. Measured Baseline

Taken on 2026-09-27 on the documented Debug configuration (`clang 17.0.6`, the
host compiler `CLAUDE.md` fixes for this machine), from a clean, fully built tree
at `59198f5`, with a fresh output directory and the phase recorder enabled:

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
| Maximum resident set size | 7,553,449,984 B (7.03 GiB) |
| Peak memory footprint (Darwin) | 6,731,733,664 B (6.27 GiB) |
| Translation units | 39 |
| Analyzed functions / summaries / SCCs | 3,548 / 3,429 / 3,429 |
| WPA components | 13,716 expected, 0 reused, 13,716 executed |
| `analysis_facts` rows | 1,249,792 |
| `provenance_edges` rows | 1,375,911 |
| `provenance_nodes` / `run_fact_bindings` rows | 752,076 / 752,076 |
| `wpa-component-results` object store | 196 MiB |
| `metadata.db` | 2.82 GiB |

### 2.1 Phase profile

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

### 2.2 Stack-attributed profile

Four main-thread `sample` captures during `wpa.orchestrate` attribute its leaves
to their innermost named frame:

| Frame | Share |
| --- | ---: |
| `EncodeSemanticKey` | ~17% |
| result scan-back (`ScanOutput` → dense→stable mapping → row validation) | ~19% |
| `ResultCanonicalizer::Canonicalize` interior | ~16% |
| `WpaInputMaterializer::Build` interior | ~11% |
| `ComputeSHA256` | ~13% |
| remainder (`orchestrate` bookkeeping, `DeriveFactId`) | ~24% |

Eight captures during `facts.publish` attribute its ~19,000 leaves:

| Frame | ≈ time |
| --- | ---: |
| `FactStore::Publish` container work | ~34 s |
| `AnalysisFactBus::Validate` | ~24 s |
| `ComputeSHA256` | ~16 s |
| `DeriveBatchId` | ~15 s |
| `DeriveWitnessId` | ~11 s |
| proto serialization + hex encoding + SQLite | ~14 s |

### 2.3 The representation gap

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
adds +2.39 GiB and the whole run's component payloads serialize to 196 MiB.

## 3. Root Causes

### 3.1 The compact form already exists and is not used in memory

`SerializeResult` / `DeserializeResult` already encode exactly this content
losslessly — relation, cell count, per-cell variant tag, and each cell's value —
and the read path already revalidates every fact identity and recomputed hash
before a cached result is accepted. The pipeline serializes each component once,
stores it, and then keeps the rich copy alive instead of the compact one.

### 3.2 Downstream containers and comparators are keyed by encoded text

`ResultCanonicalizer`, `AnalysisFactBus::Validate`, and `FactStore::Publish` hold
`std::map<std::string, …>` / `std::set<…>` keyed by the encoded semantic key
(roughly 250 bytes), and compare by re-encoding inside sort comparators. The
canonicalizer additionally nests three such maps
(`map<string, map<string, map<string, Derivation>>>`). Measured leaf share:
`EncodeSemanticKey` ~17% of the WPA phase.

`AnalysisFactBus::MakeAnalysisFactBatch` already solved this problem privately
with `FactRanks` interning; the same defect remains in the three sites above.

### 3.3 The retained payload is rich, and retention is therefore expensive

`WpaOrchestrator::Run` keeps every component's payload for the whole run because
successor support reads `completed[*].result.facts` and because assembly walks
the completions again. Retaining a run's payloads is not itself the defect — the
run genuinely needs them until the last predecessor has been materialized, and
the compact form of the whole set is only 196 MiB. The defect is that the
retained form costs 2.39 GiB for that 196 MiB of content.

This matters for the remedy's shape: because a compact retained payload is
affordable, successor support can keep being served from memory rather than
re-read from the object store, and no execution order, failure isolation, or
cache behaviour has to change.

### 3.4 Successor support decodes more than it needs

`SuccessorSupport` walks every fact of every successor SCC and copies the
matching rows out. It needs only the relations the component kind derives, so
the decode and copy can be restricted to those rows.

### 3.5 Consequences that do not scale

Both retained terms grow with the run's derived-fact count: the rich payload
retention and the rich assembled batch. A project producing ten to twenty
million facts multiplies each of them by roughly an order of magnitude over
LevelDB, in a representation already 12.5× larger than necessary.

## 4. Goals

1. Hold component payloads and the assembled batch in the existing compact
   encoding rather than as a rich object per fact, row, and witness endpoint.
2. Key the canonicalizer, batch assembly, batch validation, and the fact store
   on dense identifiers or precomputed key hashes, and encode each row's key
   once per stage rather than once per comparison.
3. Decode only the successor-support rows a component actually consumes, so the
   per-component working set stops being a whole successor payload.
4. Preserve every published byte, every identity, and every contract the
   predecessor specification froze.
5. Reduce the LevelDB wall time materially by removing the encode-and-hash tax
   the profile attributes to sections 3.1 and 3.2.

## 5. Non-Goals

- Do not change `relations.v2`, `summary.v2`, rule bundles, model bundles,
  epistemic states, fact identity, or witness selection.
- Do not change the published database schema, the hex-encoded `cells_hex`
  storage format, or any on-disk cache object format. The arena is an in-memory
  representation only.
- Do not change SVF construction, Andersen analysis, MemorySSA, or SVFG
  construction; that term is measured at 3.40 GiB and is a separate problem.
- Do not change the production engine, the four WPA domains, reverse-topological
  SCC execution, or component caching.
- Do not stream publication to the fact store in this change; that is the
  successor stage recorded in section 10.
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
5. The four published tables remain byte-identical; ordered dumps must hash
   identically before and after.
6. Dense-id assignments continue to depend only on the stable-ID set, never on
   discovery order.
7. Error paths continue to use `Status` and `StatusOr`; no RTTI, no exceptions.
8. Every modified file retains its Apache-2.0 header.

## 7. Design

### 7.1 The row arena

Introduce a compact, append-only, arena-backed store for semantic rows and
witness edges, reusing the encoding `SerializeResult` already defines. A
single arena owns one contiguous byte buffer; every row is a byte range in
it. Handles are offsets, not pointers, so the arena may reallocate while
handles stay valid.

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
renderers, tests — and is never called in a loop that retains the result.

### 7.2 Payload retention

`WpaComponentResult` keeps its identity and hash fields and holds its payload in
a `RowArena` instead of two rich vectors. `WpaComponentCompletion` is unchanged
in shape; `result_object_key` continues to name the immutable store object.

`WpaOrchestrator` retains each completion's arena — not a decoded copy — so the
run's resident payload becomes the compact form. `SuccessorSupport` reads the
relations that kind derives directly out of each successor's arena and
materializes only those rows; it never decodes a successor payload it does not
consume. Retention needs no eviction policy and no reload from the object store,
because the retained form is compact; the object store stays a cache, not a
working set.

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
into arenas instead of vectors. Rank interning, already present, is retained.

### 7.4 Id-keyed downstream

- `ResultCanonicalizer` replaces its three nested `std::map<std::string, …>`
  levels with dense identifier keys obtained from one interner built over the
  component's rows, precomputes each row's sort rank once, and replaces the
  comparative sort that re-encodes inside its comparator with a rank sort.
- The derivation-cost relaxation becomes a worklist: a derivation is revisited
  only when the cost of one of its inputs decreases, replacing the sweep over
  every derivation for up to `results.size()` rounds.
- `AnalysisFactBus::Validate` keys its fact index and root membership on
  precomputed 64-bit key hashes with exact arena byte comparison on collision,
  and stores the witness DAG's dependencies as one flat CSR array pair instead
  of `std::vector<std::vector<std::size_t>>`.
- `FactStore::Publish` groups witnesses by interned result rank rather than by
  encoded key string, and keys its fact and witness maps on dense identifiers.

### 7.5 Data flow

Unchanged end to end: ingest → local analysis → SVF → summaries → WPA
components → canonicalized component results → batch → fact store. What changes
is that every arrow after canonicalization carries a compact arena rather than a
rich object graph, and each row's canonical key is encoded once per stage.

## 8. Error Handling

- An arena append of a row whose relation or cell count does not match
  `relations.v2` returns `InvalidArgument`, as `AppendRow`'s callers do today.
- A `RowHandle` outside the arena's byte range returns `InvalidArgument` and
  never reads out of bounds; the decode path checks the range before it parses.
- `DeserializeResult`'s existing revalidation — fact identities, recomputed
  fixpoint and external hashes, SCC and component identity — is unchanged and
  remains the guard on the cache read path.
- All existing validation failures in `Validate` retain their current messages
  and ordering: a batch that fails today fails identically and with the same
  diagnostic.
- No new failure mode is introduced on any path that currently succeeds.

## 9. Verification Strategy

### 9.1 Semantic equivalence (primary)

The published content must not move. Prove it with ordered dumps of the four
published tables taken before and after, on the same build tree:

| Table | Baseline rows |
| --- | ---: |
| `analysis_facts` | 1,249,792 |
| `run_fact_bindings` | 752,076 |
| `provenance_nodes` | 752,076 |
| `provenance_edges` | 1,375,911 |

The four SHA-256 digests must be equal, and the batch id must be unchanged.
`veritas-store-diff` already exists for this comparison.

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
  `WpaEndToEndTest`: unchanged expectations, proving the representation change
  is invisible to every consumer.
- `WpaOrchestratorTest`: successor support is unchanged when served from decoded
  successor payloads.

### 9.3 Qualification

Run the targeted WPA, fact-bus, end-to-end, conformance, and M9 entry-gate
tests. No test may be skipped: a green CTest summary does not prove a test ran,
so `tests/qualification/check_no_skips.py` and `tools/check_m9_entry.py` are
mandatory.

### 9.4 Performance acceptance

Measured as a controlled pair on one build tree, rebuilt between revisions, with
a fresh output directory each and `/usr/bin/time -lp`, as the predecessor
specification's section 9.5.1 established.

Absolute numbers from different conditions are not comparable, and neither are
numbers from different revisions. The predecessor recorded 600.88 s for its own
head while this specification measures 419.78 s at `59198f5` on the documented
configuration; the two differ in both the revision measured and the machine load
at the time, so neither figure may be quoted against the other. Only a
same-tree, same-session pair supports a claim about this change.

Expected outcome at LevelDB: `wpa.orchestrate`'s resident delta from +2.39 GiB
to roughly +0.3 GiB, `facts.batch_assemble`'s from +0.56 GiB to roughly +0.1 GiB,
`facts.publish`'s working set from roughly +1.0 GiB to roughly +0.3 GiB, and wall
time from 419 s to roughly 300 s.

**This specification does not claim the 4 GiB threshold.** The measured 3.62 GiB
resident before WPA begins is untouched here (section 5), so the LevelDB peak is
expected to land near 4.2 GiB. What this change buys is that the payload terms
become ~12× cheaper and stop dominating, which is the property that scales.

## 10. Successor Stages (not this change)

1. **Streaming publication.** Remove batch materialization: stream canonical
   rows from component payloads into the fact store, holding ownership as dense
   identifiers. This removes the last O(batch) term and is the stage that makes
   a ten-to-twenty-million-fact run fit.
2. **Pre-WPA resident memory and the SVF/IR lifetime.** The 3.40 GiB SVF
   high-water and the `ProgramIr` held across WPA are the terms that grow with
   source size. This is a frozen non-goal today and needs its own amendment.

## 11. Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Arena handles outlive their arena | Handles are offsets, not pointers; arenas are owned by the same object as the payload they describe; a decode is range-checked. |
| `AppendKey` diverges from `AppendSemanticKey` | A unit test asserts byte equality on a corpus covering every cell alternative; `DeriveBatchId` equality is the end-to-end guard. |
| Descending to a rich vector for a consumer reintroduces the old cost | Consumers that only iterate use the decoded range; the tests assert the arena is not materialized on the publication path. |
| The rank sort changes canonical order | Rank assignment preserves encoded-key order by construction; a test compares both orderings on a tie-heavy corpus. |
| The worklist relaxation changes a selected proof | It converges to the same costs and the same proof-selection inputs; the four published-table digests are the end-to-end guard. |
| A representation change silently enlarges memory | The controlled pair measures peak RSS per phase, not only the total. |

## 12. Rejected Alternatives

### 12.1 Constant-factor tuning of the current representation

Rejected as insufficient. The measured profile places the remaining time in
encode-and-hash work spread across four stages and the remaining memory in
retained rich payloads; trimming allocations inside the existing shape leaves
both terms proportional to the rich object count, which is the property that
fails at scale.

### 12.2 A run-scoped interner only, leaving the payload shape alone

Rejected as a half-measure. Interning keys removes the encode tax but not the
2.39 GiB payload retention, which is the larger term and the one that scales.

### 12.3 Reload component payloads from the object store during assembly

Recorded by the predecessor specification's section 9.5.4 as the smaller step
for memory. It bounds retention but leaves the assembled batch rich, so the
batch term still scales with the fact count. It survives as stage 1 of section
10, where it belongs.

### 12.4 Replace the hex-encoded `cells_hex` storage with the compact binary

Rejected here. It would shrink `metadata.db` (2.82 GiB) substantially, but it
changes an on-disk schema and every historical cache object, which section 5
excludes and which is an independent decision.

### 12.5 Per-binding `is_current` update as the publish bottleneck

Hypothesised from reading `FactStore::AppendBinding`, measured, and refuted: one
no-match `UPDATE` against the partial unique index costs 1.9 µs on a fully built
store, so the 752,076 of them account for at most about 1.4 s of the 80 s sink.
Recorded so it is not re-proposed.

## 13. Documentation Repair (this change)

The predecessor specification lost three sections to commit `4493cc4`, which
deleted `## 10. Risks and Mitigations`, `## 11. Rejected Alternatives`, and
`## 12. Completion Criteria` while reporting round-2 measurements. Two defects
follow: section 9.5.4 cites "rejected alternative 11.3", a section that no
longer exists, and the specification no longer states its own completion
criteria. Restore the three sections verbatim so the citation resolves and issue
#133's completion criteria are stated where its scope is defined.

## 14. Completion Criteria

The work is complete only when:

1. All goals in section 4 are implemented and every contract in section 6 holds.
2. The four published-table digests and the batch id are unchanged.
3. The full clean build and the full CTest suite pass with no failures, skips,
   or not-run tests, and the M9 entry gate reports all ten criteria passing.
4. `git diff --check` and the repository license-header check pass.
5. A controlled before/after pair on one build tree records wall time and peak
   RSS for both revisions, and the improvement in both is larger than the
   run-to-run spread of the pair.
6. The branch diff contains only this change, its tests, and its documentation.
