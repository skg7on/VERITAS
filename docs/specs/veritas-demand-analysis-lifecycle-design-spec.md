# Demand Analysis, Publication, and Evidence Lifecycle

**Status:** Detailed proposed P2/P3 contract; gated by P1 evaluation.
**Plan:** [demand lifecycle](../plans/veritas-demand-analysis-lifecycle-implementation-plan.md).
**Baseline:** Native M9 receipt/publication and baseline conformance remain intact.

## 1. Identity and immutable scope

`QueryDescriptor` binds `ReviewBinding`, registered predicate/version,
typed source/sink/subject refs, selected producer/rule/model/configuration
digests, ordered boundary summaries, admitted scope/frontier policy, and
deterministic work/output limits. `QueryId` hashes canonical descriptor bytes.
It is not an AnalysisRunID. Model prose, scheduler priority and elapsed time
do not affect query identity. `QueryArtifactId` additionally hashes the result
membership, coverage, assumptions, selected proof DAG and completion outcome.
A timed-out artifact may be inspected but cannot answer a complete request.

```cpp
struct QueryLimits { uint64_t examined_rows, proof_edges; uint32_t functions, sccs; };
StatusOr<QueryPlan> PlanQuery(const QueryDescriptor&, const DependencySnapshot&);
StatusOr<QueryArtifact> ExecuteQuery(const QueryPlan&, QueryExecutor&);
```

New public types live in `include/veritas/query/QueryTypes.h`. Default query
limits are 100,000 examined rows, 20,000 proof edges, 64 functions and 8 SCCs;
runtime wall/memory limits are operational abort policies. Reject zero values,
overflow, mismatched bindings and unsupported producer versions before execution.

`QueryPlan` records the full admitted SCC set, relevant local/base relations,
boundary summaries, unresolved edges, producer capabilities and deterministic
work ordering. Never split a recursive SCC just to fit the function limit.
An oversized SCC returns a frontier/incomplete outcome; it is not silently
ignored. Shared/escaped memory, indirect targets, callbacks and external effects
are open unless covered by a conservative, versioned boundary contract.

## 2. Scoped reads and work accounting

Add a virtual read API alongside `EvidenceReadSnapshot::GetCurrentFacts()`:

```cpp
struct FactSelector { RelationId relation; std::vector<CellEquality> equalities; };
struct PageRequest { uint32_t limit; uint64_t max_examined, max_output_bytes; std::optional<PageCursor> after; };
StatusOr<FactPage> ReadFacts(const FactSelector&, const PageRequest&) const;
```

`CellEquality` includes column index, cell domain and typed value. A cursor binds
snapshot digest, selector digest, canonical last consumed FactID and ordering version.
Reject another selector/snapshot's cursor. `FactPage` returns facts, examined
count, continuation, exhaustion and work-exhausted status; it is not by itself a closed-world absence
certificate. Cap page size at 4096; use an indexed `limit+1` probe whose extra
row is not returned. Exact boundary completion differs from truncation.
Bound candidate/index probes and canonical-row checks by `max_examined`, including
rejected digest collisions and sparse multi-equality intersections. A page may
return zero matches plus a continuation at the last consumed candidate. Consumed
includes returned matches and filtered rejects, but excludes the unreturned
lookahead row; that row must remain eligible on the next page. Charge all probes
to examined work, including the lookahead. It must
not restart from the last returned match and repeat unbounded filtered work.
Only exhaustion of the actual selector domain means traversal-complete; exhausting
the work allowance is INCOMPLETE even when the returned set is empty.
Drive the page from one ordered covering index (or the relation/run index when
unbound), then check other equalities and canonical values within the allowance;
do not hide an unbounded SQL join/count behind a logical examined counter.
Bound serialized page output (default 1 MiB). An oversized first row yields an
explicit required-byte/frontier outcome without consuming it, permitting a
larger explicit request rather than silently losing the fact.

Add `fact_lookup_v1(run_id, relation_id, column_index, value_digest, fact_id)`
in the rebuildable auxiliary file `fact_lookup.v1.db`, with an ordered covering
index and `index_meta(run_id,batch_id,producer_version,status)`. Bind it to a
sealed native receipt; it is not an authoritative metadata.db migration and
does not consume schema-v5. Old readers ignore the derived file. Build it atomically
from validated cells at publication/migration. Verify typed values against
the canonical stored row before returning; digest collision cannot produce a
false match. Legacy stores may build the index once, with progress and explicit
cost, or return UNSUPPORTED; they do not pretend a full scan is bounded work.
Native Explain remains under the same snapshot and depth/member budget.

## 3. Compositional kernels and proofs

Store local transfer/boundary summaries in a new `boundary-summary.v1` artifact:
function/config/layout identity, parameter/return/escaped-memory interface,
supported predicates, conservative effects, model assumptions, unknown frontier,
dependency digests, and producer version. Continue reading `summary.v2`; do not
reinterpret its canonical bytes or make it carry model-inferred contracts.

Query-seeded Soufflé kernels receive only admitted local tuples and boundary
support, with source/sink/predicate bindings applied before recursive evaluation.
For the first implementation, compute a conservative backward relevance set
from the sink, intersect forward reachability from the source, and retain open
boundary markers. Prove against full native answers on supported small closed
fixtures; new ranges/check semantics require independent oracles. A relevance
filter is not a proof that discarded callers cannot affect shared memory.

Inside a recursive region run the registered fixed point to completion or
return INCOMPLETE. Preserve epistemic weakening. Do not claim complete IFDS/IDE
semantics without a separately qualified transfer-function contract. Magic-set
transformation is optional research because current witness/key functors may
exclude it. Baseline rules remain unchanged.

Return a selected finite rooted proof DAG for admitted results. Nodes identify
the exact tuple/rule/producer/input artifact; edges are required premises.
Canonical tie breaking selects the same proof for the same semantic inputs.
Explain may expand a deferred subproof, but an authoritative published result
must retain enough validated derivation/certificate to check its support.
Dropping witness premises to meet a budget returns a partial result instead.

Workers use the captured snapshot and explicit boundary input, terminate under
the controller limits, and return serializable artifacts. Record global SVF
bootstrap separately. A worker killed after output generation but before
publication does not change the current snapshot or mint a complete artifact.

## 4. Staged publication and migration

Keep `AnalysisFactBus` native whole-run receipts unchanged. Query publication
uses additive schema-v5 tables, with exact migration ordering:

```text
query_staging(staging_id, query_id, state, lease, descriptor_digest)
query_staging_chunks(staging_id, ordinal, digest, byte_count, row_count)
query_manifests(query_id, artifact_id, descriptor_digest, state, manifest_blob)
query_chunks(artifact_id, ordinal, digest, byte_count, row_count)
query_current(slot_id, generation, artifact_id)
query_dependencies(artifact_id, kind, key, digest, polarity)
query_occurrences(occurrence_id, query_id, outcome, diagnostic_ref)
```

Staging handles have occurrence identity and STAGING/ABANDONED states; final
artifact identity is computed only by Seal. Seal atomically creates immutable
SEALED manifest/chunk references from staged digests without copying payloads.
SEALED → PUBLISHED advances the final manifest; ABANDONED staging is terminal.
Stage at most
4096 canonical rows or 4 MiB per chunk, whichever binds first; individually
oversized rows use one bounded spill object and fail if over the artifact
limit. Objects are immutable and digest-checked. Streaming validation tracks
cross-chunk order/deduplication, proof closure and dependencies without rebuilding
a rich whole-batch vector. Retain a bounded external index/spill for proof
references if needed; measure its disk and memory costs.

```cpp
StatusOr<StagingHandle> BeginQuery(const QueryDescriptor&);
Status AppendChunk(StagingHandle, const CanonicalChunk&);
StatusOr<ArtifactId> SealQuery(StagingHandle, const QueryCompletion&);
Status PublishQuery(ArtifactId, const PublicationSlot&, uint64_t expected_generation);
```

`PublicationSlot={repository_id, program_selection_key, question_key,
predicate_kind, stable_subject_refs}` is registered by the controller. `slot_id`
hashes this canonical tuple. Program selection is a stable operator target key,
not the content-derived ProgramScopeID; question_key is a persisted controller
question handle. Exclude revision/snapshot, budgets and result digests so a newer
compatible answer can CAS the same logical slot. A changed/ambiguously matched
question gets a new handle; never guess cross-revision anchor equivalence.
Publish validates repository/program/question/predicate/ref compatibility of
the artifact with the registered slot. Exact descriptor/artifact IDs still bind
snapshot and budgets, and readers request minimum coverage explicitly; a weaker
artifact cannot satisfy an earlier complete request merely because it is current.
Native run receipts remain unrelated to these review-slot handles.

Seal validates every digest, ordinal, canonical ordering, coverage and required
proof root. Publish uses one SQLite transaction with compare-and-swap generation
to insert the final manifest/current binding and receipt. CAS blobs may exist
before the transaction; readers follow only PUBLISHED manifests. Idempotent
repeat returns the same receipt. A generation conflict returns retryable
`STALE_PUBLICATION`, without replacing a newer binding. Incomplete artifacts
are inspectable by explicit ID but never become a complete current answer.

Migration is additive and failure-atomic; existing v4 readers reject unsupported
v5 stores rather than silently ignoring semantics. Exported baseline stores may
remain v4. Recovery abandons unsealed manifests after verifying ownership/lease;
it does not infer completion from a collection of orphan chunks.

## 5. Reuse and executable invalidation

Exact-snapshot cache lookup checks descriptor identity, trusted sealed-manifest
digest and required completeness. Cross-revision reuse additionally revalidates
all dependencies against the new snapshot. Positive dependencies include
summaries, layouts, models, source/test/doc contracts and target configuration.
Negative dependencies include searched callsite/guard sets, indirect target
sets, build membership and open-boundary assumptions. A new relevant member
invalidates absence even when no old supporting tuple changed.

`Revalidate(const QueryArtifact&, const DependencySnapshot&) -> ReuseDecision`
returns REUSE, STALE or PARTIALLY_STALE with reason refs. Reuse creates a new
binding/receipt to unchanged content; it never mutates an old artifact's identity.
Portability remains host-scoped until #125 qualifies AnalysisTarget identity.

For native WPA, implement #143's durable, deduplicated `(SCC, component)` worklist.
Seed changed local/configuration inputs; process reverse dependencies until no
externally visible hash changes. Reconcile SCC merges/splits and removals before
reuse. A no-op executes zero semantic components; necessary frontend/digest
validation is counted separately. Cache identity policing may move only with
an explicit trusted-ingest/scrub policy and corruption tests; never remove it
as an incidental optimization. Existing #143's M15 prerequisite remains for
its full baseline claim; exact-snapshot review reuse does not require that claim.

## 6. Retention and physical storage

GC roots are current bindings, retained history, active workers/staging leases,
published reports and all transitive input/proof artifacts. Mark through
manifests, then sweep unreferenced objects older than the retention epoch.
Readers pin an epoch/lease; racing GC cannot remove an active proof. Crash
between marking and sweeping is restartable. Never follow paths supplied by
model text. #144 remains owner of unchanged-object physical amplification.

Measure logical live bytes, table/index bytes, WAL/SST bytes, obsolete operands,
temporary spill and compaction state independently. Unchanged publication must
not append another payload operand; verify concurrent put-if-absent and
same-key/different-content refusal before changing RocksDB Merge behavior.

Acceptance includes cursor substitution, digest collision, chunk corruption,
crash at each publication transition, mixed-snapshot refusal, no-op and relevant/
irrelevant edit comparison against clean recomputation, negative dependency
addition, SCC topology change, and concurrent reader/GC. Baseline equivalence
uses M13's existing projection; demand artifacts use versioned answer/coverage/
proof comparisons, not whole-store byte equality.
