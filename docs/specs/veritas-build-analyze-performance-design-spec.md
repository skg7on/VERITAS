# `veritas-build analyze` Performance Design Specification

**Status:** Approved for implementation

**Tracking issue:** [#133](https://github.com/skg7on/VERITAS/issues/133)

**Implementation plan:**
[`../plans/veritas-build-analyze-performance-implementation-plan.md`](../plans/veritas-build-analyze-performance-implementation-plan.md)

## 1. Purpose

This specification removes avoidable whole-program analysis overhead observed
when `veritas-build analyze` processes a project with many functions and mostly
singleton strongly connected components. The change is deliberately bounded:
it improves data reuse, ownership, encoding, and canonicalization inside the
existing pipeline without altering any analysis result or execution contract.

The motivating command is:

```bash
./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb
```

The implementation must preserve the production Soufflé engine, the four WPA
domains, reverse-topological SCC execution, engine-neutral logical inputs,
component caching, provenance, and the immutable `AnalysisFactBatch` handoff.

## 2. Measured Baseline

The diagnostic run used the binary at commit
`14ea201929dd7f65e024ed7f6788682148e272f2`, redirected output to a fresh
directory under `/tmp`, and sampled the running process three times with the
macOS `sample` tool.

| Measurement | Observed value |
| --- | ---: |
| Compilation database entries | 39 |
| Analyzed function objects | 3,548 |
| SCCs | 3,429 |
| WPA domains per SCC | 4 |
| WPA component executions | 13,716 |
| SVF pointers | 88,563 |
| SVFG nodes | 187,139 |
| SVFG edges | 246,104 |
| SVF Andersen time | 20.495 s |
| SVF MemorySSA time | 32.531 s |
| Peak resident memory | approximately 9.5 GB |
| Wall time when stopped | 747.08 s |
| Completion state | all WPA components complete; final batch still sorting |

The project is small by source-repository size but not by VERITAS semantic
state. Clang template expansion, library headers, and linking produce thousands
of functions and an SCC condensation graph dominated by singleton SCCs. The
pipeline must therefore scale with semantic content and must not multiply a
whole-program scan by the number of SCC components.

## 3. Root Causes

### 3.1 Summary indexing is rebuilt for every component

`WpaOrchestrator::Run` builds the call graph and SCC graph once, but every call
to `WpaInputMaterializer::Build` independently constructs:

```cpp
std::map<core::StableId, const summary::SummaryArtifact*> by_function;
```

It does so by parsing every summary's `function_variant_id`. With 3,548
summaries and 13,716 components, the LevelDB run performs up to 48,664,368
avoidable whole-program identity parses before accounting for IDs in local
facts. Runtime samples repeatedly placed the main thread in this loop.

### 3.2 Stable-ID validation uses stream-based digest rendering

`ParseStableId` validates canonical digest spelling through `DigestToHex`.
`DigestToHex` currently creates an `ostringstream` and applies `std::hex`,
`std::setfill`, and `std::setw` for every byte. Samples inside materialization
showed a large share of `ParseStableId` time below these locale-aware stream
operations. The digest is fixed at 32 bytes and needs only deterministic
lowercase nibble lookup.

### 3.3 WPA results retain redundant flattened payloads

Each `WpaComponentCompletion` owns its canonical facts, witnesses, and
diagnostics. `WpaOrchestrator` additionally appends the same payloads to
flattened vectors in `WpaRunResult`. `MakeAnalysisFactBatch` does not consume
those flattened vectors; it walks the component completions again to select a
single canonical owner for duplicate facts. The flattened vectors therefore
increase memory without serving the publication path.

### 3.4 Fact-batch assembly copies and repeatedly re-encodes large values

`MakeAnalysisFactBatch(const WpaRunResult&)` copies all component completions,
including their result payloads. It then copies owned facts and witnesses into
new batch vectors. Finally, its sort comparators call `EncodeSemanticKey` for
the same rows on every comparison. Once the 13,716 components completed, the
process reached approximately 9.5 GB and spent the remainder of the diagnostic
run sorting inside `MakeAnalysisFactBatch`.

## 4. Goals

1. Build the function-summary index once per WPA run and reuse it across every
   component materialization.
2. Render SHA-256 digests with one fixed-size lowercase encoding pass and no
   iostream formatting.
3. Retain each component result payload exactly once until fact ownership is
   selected.
4. Assemble the publication batch by moving payloads and precomputing each
   canonical sort key once.
5. Preserve every logical input, semantic result, witness, hash, cache key,
   component completion record, and batch identity.
6. Complete the motivating LevelDB analysis within 375 seconds and at no more
   than 4 GB peak RSS on the same development machine and build configuration.

## 5. Non-Goals

- Do not batch multiple SCCs into one Soufflé execution.
- Do not parallelize SCC or component execution.
- Do not change the default production engine or enable the C++ emergency
  engine implicitly.
- Do not change SVF construction, Andersen analysis, MemorySSA, or SVFG
  construction.
- Do not change `summary.v2`, `relations.v2`, rule bundles, model bundles,
  epistemic states, fact identity, or witness selection.
- Do not add a performance-only CLI flag or weaken validation.
- Do not introduce a process-global cache or retain pointers beyond one
  `WpaOrchestrator::Run` invocation.

## 6. Preserved Contracts

The following are hard invariants:

1. Every expected `(SccId, WpaComponentKind)` remains an independently
   materialized, cached, executed, canonicalized, and published component.
2. SCCs remain in reverse topological order so successor support is available
   before predecessor materialization.
3. Production and conformance engines receive byte-identical
   `WpaLogicalComponentInput` values for the same semantic request.
4. `LogicalInputHash`, `FixpointHash`, and `ExternalHash` remain byte-identical
   for unchanged input.
5. Dense-ID assignments depend only on the stable-ID set, never discovery
   order.
6. Duplicate facts are owned by the first component in canonical component-key
   order, and the selected fact retains one complete witness derivation.
7. `AnalysisFactBatch` continues to carry exact expected/completed component
   equality, rooted input evidence, canonical facts, finite witnesses,
   diagnostics, and a content-addressed `BatchId`.
8. Completed component entries in `AnalysisFactBatch` retain their key,
   immutable result object key, logical-input hash, fixpoint hash, external
   hash, SCC ID, and component kind. Their already-flattened fact, witness, and
   diagnostic vectors are empty after assembly because those payloads live in
   the batch's canonical vectors.
9. Error paths continue to use `Status` and `StatusOr`; the implementation uses
   neither RTTI nor exceptions.

## 7. Design

### 7.1 Run-scoped summary index

Add a non-owning `WpaSummaryIndex` beside `WpaInputMaterializer`. It owns a
`std::map<StableId, const SummaryArtifact*>` and records the source span's data
pointer and size. Construction parses and validates every function-variant ID
exactly once. Duplicate function IDs are rejected rather than silently keeping
one artifact.

The public surface is:

```cpp
class WpaSummaryIndex {
 public:
  static StatusOr<WpaSummaryIndex> Build(
      std::span<const summary::SummaryArtifact> summaries);

  bool Covers(std::span<const summary::SummaryArtifact> summaries) const;
  const summary::SummaryArtifact* Lookup(core::StableId function_id) const;
  bool Contains(core::StableId function_id) const;
};
```

`Covers` prevents an index from being reused with a different container or
after reallocation. The index is valid only while its source summary span is
alive and unchanged.

`WpaInputMaterializer` exposes two entry points:

```cpp
static StatusOr<WpaLogicalComponentInput> Build(
    const WpaMaterializationRequest& request);

static StatusOr<WpaLogicalComponentInput> Build(
    const WpaMaterializationRequest& request,
    const WpaSummaryIndex& summaries);
```

The one-argument overload builds a local index and preserves existing
standalone callers. `WpaOrchestrator::Run` builds one index after the call/SCC
graphs succeed and passes it to every component. The indexed overload rejects
a non-covering index with `InvalidArgument`.

This changes only how summaries are located. Member selection, unmodeled
external detection, model lookup, row construction, dense maps, canonical
ordering, and logical-input hashing remain unchanged.

### 7.2 Direct digest-to-hex encoding

Replace `ostringstream` formatting in `DigestToHex` with one pre-sized
64-character string and a constant lowercase table:

```cpp
constexpr char kHex[] = "0123456789abcdef";
std::string result(kSHA256DigestBytes * 2, '0');
for (std::size_t i = 0; i < digest.size(); ++i) {
  const auto byte = static_cast<std::uint8_t>(digest[i]);
  result[2 * i] = kHex[byte >> 4];
  result[2 * i + 1] = kHex[byte & 0x0f];
}
```

The output remains exactly 64 lowercase ASCII characters. Existing canonical
ID strings and hashes do not change.

### 7.3 Single ownership of component payloads

`WpaRunResult` stops exposing redundant flattened `facts`, `witnesses`, and
`diagnostics` vectors. The authoritative payload before publication is
`completed_components[*].result`. Tests and callers needing flattened content
must construct the canonical `AnalysisFactBatch`, which is already the sole M9
WPA handoff.

`WpaOrchestrator` continues to keep component results in memory because they
provide successor support, optional conformance comparison, and final batch
assembly. It no longer copies each result into a second run-level vector.

### 7.4 Consuming fact-batch assembly

Change the assembly signature to accept the run by value:

```cpp
AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result);
```

Callers may still pass an lvalue and receive copy semantics. The production
path passes `std::move(*wpa_result)`, transferring the run's allocations into
assembly. This keeps tests convenient while making the large path explicit.

Assembly performs these steps:

1. Move run metadata, component vectors, roots, and scheduling-independent
   diagnostics into the batch.
2. Sort component completions by `WpaComponentKey` before ownership selection.
3. Move the first canonically owned occurrence of each fact into a keyed fact
   vector; record overridden semantic keys for each later component.
4. Move only witnesses belonging to the selected derivation into a keyed
   witness vector.
5. Move component diagnostics into the batch.
6. Clear each completion's fact, witness, and diagnostic vectors immediately
   after processing. The completion metadata and hashes remain intact.
7. Sort keyed facts and witnesses by their precomputed keys, move the payloads
   into the batch's public canonical vectors, and discard the temporary keys.
8. Derive the unchanged canonical `BatchId`.

This ordering preserves the existing canonical-owner rule while avoiding a
second live copy of every component result.

### 7.5 Canonical sort keys

Fact sorting uses one encoded semantic key per owned fact. Witness sorting uses
one composite key containing:

```text
result semantic key
rule ID
input semantic key
input ordinal
```

The comparison order is identical to the current comparator. The only change
is that `EncodeSemanticKey` runs once per item during assembly instead of
repeatedly during `O(n log n)` comparisons.

## 8. Error Handling

- An invalid summary identity makes `WpaSummaryIndex::Build` return
  `InvalidArgument("invalid summary identity")`.
- Duplicate function-variant IDs make index construction return
  `InvalidArgument("duplicate summary function identity")`.
- Passing an index that does not cover `request.summaries` returns
  `InvalidArgument("summary index does not cover request summaries")`.
- Existing missing-SCC, invalid local ID, relation-schema, cache-validation,
  executor, canonicalizer, witness, and publication errors remain unchanged.
- A failed index build happens before any component execution and marks the WPA
  run incomplete through the orchestrator's existing failure path.

## 9. Verification Strategy

### 9.1 Unit tests

- `HashTest` verifies exact lowercase rendering for a digest containing leading
  zeroes and high-bit bytes, plus the existing round trip.
- `WpaInputMaterializerTest` builds a reusable index and proves indexed and
  fallback materialization have identical EDB rows, mappings, roots, and
  `LogicalInputHash`.
- `WpaInputMaterializerTest` rejects an index built over a different summary
  span and rejects duplicate function identities.
- `WpaOrchestratorTest` continues to prove reverse-topological execution,
  exact component count, support isolation, cache reuse, and failure atomicity.
- `AnalysisFactBusTest` passes an rvalue run, verifies canonical fact/witness
  output and unchanged `BatchId` under reversed completion order, and verifies
  that component payload vectors in the batch are empty after assembly.

### 9.2 Integration and qualification

Run the targeted WPA, fact-bus, end-to-end, conformance, and M9 entry-gate
tests. No test may be skipped. A green CTest summary is insufficient when a
`GTEST_SKIP` occurred, so the existing no-skips qualification check remains
mandatory.

### 9.3 Performance acceptance

Build a clean Debug configuration and run the motivating command with a fresh
output directory under `/tmp` using `/usr/bin/time -lp`. Record:

- wall, user, and system time;
- maximum resident set size;
- translation-unit, function, SCC, and component counts;
- final published summary, CPG, WPA run, fact, and unknown counts.

On the same development machine and LevelDB checkout used for the baseline, the
run must complete within 375 seconds and peak RSS must not exceed 4 GB. These
machine-specific thresholds are an acceptance benchmark, not a portable CI
timing assertion.

### 9.4 Implementation status (2026-09-22)

The bounded implementation is functionally complete, but the LevelDB
performance acceptance criterion is not met. The branch implements:

- direct digest encoding and incremental SHA-256 hashing;
- one run-scoped summary index;
- single ownership and consuming assembly of component results;
- a non-copying fact-bus handoff and compact ID-indexed witness validation;
- prepared-statement reuse in `MetadataStore` and copy reduction in
  `FactStore` publication.

Measured results on the reference machine are:

| Revision state | Output mode | Wall time | Maximum RSS | Result |
| --- | --- | ---: | ---: | --- |
| Tasks 1-3 | fresh | 543.47 s | unavailable | misses time limit |
| + persistence reuse | fresh | 519.84 s | 8.60 GiB | misses both limits |
| + non-copying bus handoff | receipt reuse | 313.40 s | 7.61 GiB | isolates pre-insert peak |
| + streaming batch hash and compact validation | receipt reuse | 327.66 s | 7.18 GiB | memory improved; still over limit |

### 9.5 Refinement round 2 (2026-09-24)

Five further refinements were implemented and measured against the state above:

- identity is derived without copying the row (`DeriveFactId`), and semantic
  keys are appended into one reusable buffer (`AppendSemanticKey`) rather than
  returned as a fresh string per row, so validation and hashing stop
  materializing a row copy per fact and per witness endpoint;
- per-witness ordering keys live in one packed arena instead of a `std::string`
  per endpoint, stripped component payload vectors release their capacity
  instead of only their elements, and the batch-id hash streams every row
  through one scratch buffer;
- a successful component is stored by move, and successor support reads the
  completed results instead of a second copy of every component's facts;
- witness ids stream their fields into the hash instead of first copying every
  input row into a temporary vector;
- facts, bindings, and provenance nodes and edges are published through
  `BulkInsertBatcher`, which emits multi-row statements sized from the
  connection's own bind-parameter limit and preserves the ordering that
  AUTOINCREMENT identities depend on. The receipt still commits with the rows
  it accounts for, and facts are still flushed before any binding that a
  foreign key depends on.

| Revision state | Output mode | Wall time | Peak resident memory | Result |
| --- | --- | ---: | ---: | --- |
| round 1 head | fresh | 519.84 s | 8.60 GiB | misses both limits |
| + round 2 refinements | fresh | 490.42 s | 6.14 GiB | misses both limits |
| + round 2 refinements (repeat) | fresh | 486 s | 7.38 GiB sampled | misses both limits |

Peak memory is reported by `/usr/bin/time -lp` as "maximum resident set size";
the repeat run was sampled every five seconds instead, and the two metrics
disagree, so the range is quoted rather than a single value.

**Published content is unchanged.** Ordered dumps of the four published tables
hash identically before and after this round, byte for byte:

| Table | Rows | SHA-256 of the ordered dump |
| --- | ---: | --- |
| `analysis_facts` | 1,249,792 | `452a850ec90ab192a2e5cde28269067f06f4618338675c26c870cac466e41cda` |
| `run_fact_bindings` | 752,076 | `bab0a696647ffb25c6c6a5fb04b086ae3b9cf1af20a15ecd8929457ef71c8282` |
| `provenance_nodes` | 752,076 | `387d299985baf5e8c3467efdc2edc84976ea4fce6ae767a6d7036dbfe63277ba` |
| `provenance_edges` | 1,375,911 | `da4829f280ade0b82da2e1a537896c6c796a932cbfd6c8f439e5b09a05121f0a` |

**Run and batch identity moved, and not because of a semantic change.**
`engine_toolchain_identity` incorporates the SHA-256 of the compiled Souffle
functor library, and this round edits `SemanticKeyCodec.cpp`, which is compiled
into that library. A byte-for-byte equivalent refactor of that source therefore
changes the functor library's hash, the engine toolchain identity
(`souffle-628f2b13...` to `souffle-ae3b37bf...`), the analysis run id
(`run:sha256:be1ef973...` to `run:sha256:ce2bddbe...`), and, because the batch
id covers the run id, the batch id
(`fact:sha256:ee83dca0...` to `fact:sha256:76dcc885...`). The new identity equals
the build's own `canonical_provenance_sha256`. Any edit to the functor library
has this effect; it is a property of the identity scheme, not of these changes.

**The peak is the retained component payload, not publication.** Sampling the
resident set every five seconds during a fresh run gives:

| Elapsed | Resident set | Phase |
| ---: | ---: | --- |
| 60 s | 1.24 GiB | ingestion and local summaries |
| 75 s | 3.42 GiB | SVF construction |
| 296 s | 7.38 GiB | peak, during WPA while component results accumulate |
| 486 s | 6.53 GiB | end of run, after assembly released its ordering keys |

Memory climbs monotonically with completed components and peaks well before any
row is written. The run holds every component's canonical facts and witnesses
from the moment the component completes until the batch is assembled, because
successor support may need any of them until the last predecessor has run. At
1,249,792 facts and 1,375,911 witness edges that retention, plus SVF's
high-water mark, is the floor. Nothing that reduces assembly intermediates,
duplication, or statement overhead can move it.

Reaching 4 GiB therefore requires not retaining all component payloads at once:
each component would have to fold into the batch as it completes, or the batch
would have to be assembled by reloading component payloads from the
content-addressed component store. Both change execution order, failure
isolation, component caching, and result ownership, which design section 5 and
rejected alternative 11.3 place outside this change. Of the two, reloading for
assembly is the smaller step: it preserves execution order and adds one read per
component, at the cost of re-reading roughly the payload's worth of bytes.

The measured 490.42 s is 115 s over the time limit. Bulk persistence removed the
per-statement overhead of about 4.9 million statements, but the remaining time
is dominated by the 13,716 component executions and by SVF construction, which
this change does not touch.

Until both the 375-second and 4-GiB limits pass together, issue #133 remains
open and this work must not be reported as meeting performance acceptance.

## 10. Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Index outlives or mismatches its summary storage | Non-owning index records source data pointer and size; indexed build validates `Covers` on every call. |
| Moving facts changes canonical ownership | Component completions are sorted before moving; existing ownership tests run in both original and reversed input order. |
| Stripped completion payload breaks M9 | M9's documented completion contract is key + object/hash metadata; canonical payloads remain in batch facts/witnesses/diagnostics. |
| Cached sort keys change ordering | Keys use the same `EncodeSemanticKey`, rule, input, and ordinal sequence as the current comparator. |
| Faster hex encoding changes IDs | Exact known-output and round-trip tests prove byte-for-byte equivalence. |
| Optimization hides a larger Soufflé-process cost | SCC batching remains out of scope and is evaluated only if the bounded changes miss the acceptance target. |

## 11. Rejected Alternatives

### 11.1 Select the C++ emergency engine by default

Rejected because Soufflé is the mandatory production WPA engine. Changing the
engine would alter provenance and evade rather than fix shared orchestration
costs.

### 11.2 Cache summary indices globally

Rejected because process-global pointers would complicate lifetime, revision,
and concurrency safety. One run-scoped index removes the repeated work without
new shared state.

### 11.3 Batch all SCCs into one Soufflé invocation

Potentially valuable, but architectural. It changes failure isolation,
component caching, limits, result ownership, and incremental scheduling. It
must be proposed separately if the bounded work misses the target.

### 11.4 Add wall-clock assertions to CTest

Rejected because timing thresholds are machine-dependent and flaky. CI proves
semantic equivalence; the pinned LevelDB benchmark proves performance on the
reference machine.

## 12. Completion Criteria

The work is complete only when:

1. All goals and preserved contracts above are implemented.
2. Targeted tests demonstrate red/green behavior for the new index and
   consuming batch contract.
3. The full clean build and full test suite pass with no failures or skips.
4. `git diff --check` and the repository license-header check pass.
5. The LevelDB benchmark meets both the wall-time and peak-RSS thresholds.
6. The branch diff contains only this performance improvement and its tests and
   documentation.
