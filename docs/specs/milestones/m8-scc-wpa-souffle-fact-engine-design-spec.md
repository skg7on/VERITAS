# M8 SCC-Aware WPA and Souffle Fact Engine Design Spec

**Status:** Implemented
**Milestone:** M8
**Issue:** `#11`
**Depends on:** M7 scheduler, M6 thin CPG optional projection
**Feeds:** M9 fact store and provenance, M10 Evidence APIs

---

# 1. Purpose

M8 computes deterministic whole-program facts from immutable function
summaries. It builds a call graph with explicit uncertainty, collapses recursive
regions into strongly connected components (SCCs), and evaluates finite
monotone domains until each SCC converges.

The required executable domains are:

```text
TransitiveCalls
MayWrite
```

M8 also establishes a stable file-based relation boundary for Souffle. The C++
engine is the required implementation and remains usable when Souffle is not
installed or is explicitly disabled. Souffle is an optional accelerator and
cross-check for recursive relations, not a source of durable VERITAS facts.

M8 is complete when every acceptance test in section 15 passes in a build
without Souffle. When Souffle is available, the optional integration tests must
also pass.

---

# 2. Scope

## 2.1 In scope

- Stable callee identities in `FunctionSummary` call facts.
- Loading all current summaries for one revision and build variant.
- Deterministic call-graph and SCC construction.
- Reverse-topological processing of the condensation graph.
- C++ fixpoint evaluation for transitive calls and may-write effects.
- Epistemic weakening through call chains.
- Scoped unknown-call effects without whole-program fanout.
- Persistent SCC topology and component convergence state in SQLite.
- M7 work-item production when an SCC's external hash changes.
- Stable base and derived fact tuples with immediate provenance inputs.
- Deterministic Souffle fact export and derived-result import.
- Optional Souffle execution and rules for reachability and may-write.

## 2.2 Out of scope

- Durable publication of derived facts into the M9 fact store.
- `GlobalValueFlow`, `MayRead`, taint, ownership, lock, or state domains.
- A user-facing WPA CLI.
- Distributed or multi-writer SCC execution.
- Treating the M6 CPG as the source of call-graph truth.
- Persisting Souffle-native records as VERITAS facts.

The fact schema reserves relation names for later domains, but M8 does not
implement their transfer functions.

---

# 3. Reuse and Backend Strategy

VERITAS owns:

```text
summary-to-call-graph conversion
stable identities
epistemic joins
SCC construction and scheduling
fixpoint state hashes
unknown-call policy
tuple identity and provenance
fact validation and publication boundaries
```

Souffle may execute:

```text
recursive call reachability
transitive may-write rules
```

The integration is file based. No Souffle C++ type appears in a public VERITAS
header. Base `.facts` files and derived `.csv` files are temporary execution
artifacts. Imported `FactTuple` objects are the only output that can cross into
M9.

---

# 4. Stable Call Identity

## 4.1 Existing mismatch

The current summary model stores the caller as
`FunctionIdentity.function_variant_id`, a semantic stable ID. A `Call` stores
only `callee_symbol`, which is a raw LLVM symbol name. Comparing these values
would make the M8 graph incomplete for normal M4/M5 output.

M8 adds an optional resolved identity without changing the diagnostic symbol:

```proto
message Call {
  string callee_symbol = 1;
  string call_site_anchor_id = 2;
  EpistemicState epistemic = 3;
  string provenance_ref = 4;
  string resolved_callee_function_variant_id = 5;
}
```

The field number is additive and the existing fields retain their meaning.
The field is populated only when the analysis can map the target to a VERITAS
function variant.

## 4.2 Extraction policy

- A resolved direct call stores the target's function-variant ID and preserves
  its raw `callee_symbol`.
- A refined indirect call stores the target ID for every resolved `MAY` target.
- A call with no stable target keeps the raw symbol and has an empty resolved
  ID.
- An empty resolved ID is not matched by name during WPA. It produces one
  `UnknownCallEffect` scoped to the caller and call site.
- `UNKNOWN` calls never create graph edges to all known functions.

## 4.3 Current-summary loading

`SummaryRepository` gains a deterministic bulk read:

```cpp
StatusOr<std::vector<summary::v1::FunctionSummary>> ListCurrentSummaries(
    std::string_view revision_id,
    std::string_view build_variant_id) const;
```

It resolves current bindings in SQLite, loads immutable summary bodies from the
object store, validates the requested revision/build context, and returns them
ordered by `function_variant_id`.

---

# 5. Call Graph

The call graph contains only stable function-variant IDs.

```cpp
namespace veritas::wpa {

struct CallEdge {
  core::StableId caller;
  core::StableId callee;
  std::string call_site_anchor_id;
  summary::v1::EpistemicState epistemic;
  std::string provenance_ref;
};

struct UnknownCallEffect {
  core::StableId caller;
  std::string call_site_anchor_id;
  std::string callee_symbol;
  std::string provenance_ref;
};

class CallGraph {
 public:
  static StatusOr<CallGraph> FromSummaries(
      std::span<const summary::v1::FunctionSummary> summaries);

  Status AddFunction(core::StableId function_variant_id);
  Status AddCall(CallEdge edge);
  Status AddUnknownCall(UnknownCallEffect effect);

  std::span<const core::StableId> Functions() const;
  std::span<const CallEdge> Outgoing(core::StableId caller) const;
  std::span<const UnknownCallEffect> UnknownCalls(
      core::StableId caller) const;
};

}  // namespace veritas::wpa
```

Graph invariants:

- Vertices, adjacency lists, and unknown markers have deterministic ordering.
- `MUST` and `MAY` calls create edges and participate in SCC construction.
- `UNKNOWN`, `INFERRED`, `ASSUMED`, `MUST_NOT`, and unspecified calls do not
  create SCC edges. Unsupported states produce a scoped unknown marker.
- Duplicate identical edges are idempotent.
- Conflicting facts for the same exact call site return `InvalidArgument`.

---

# 6. SCC Construction

`IdKind` gains `kScc`, serialized as `scc:sha256:<digest>`. An SCC ID is computed
from a version tag plus the sorted member function-variant IDs. It does not
include native addresses, discovery order, revision ID, or timestamps.

```cpp
namespace veritas::wpa {

class SccGraph {
 public:
  static StatusOr<SccGraph> Build(const CallGraph& call_graph);

  StatusOr<core::StableId> SccForFunction(
      core::StableId function_variant_id) const;
  StatusOr<std::span<const core::StableId>> Members(
      core::StableId scc_id) const;
  StatusOr<std::span<const core::StableId>> Predecessors(
      core::StableId scc_id) const;
  StatusOr<std::span<const core::StableId>> Successors(
      core::StableId scc_id) const;
  std::span<const core::StableId> ReverseTopologicalOrder() const;
};

}  // namespace veritas::wpa
```

Tarjan's algorithm is used with sorted vertices and sorted outgoing edges.
Members and SCC adjacency lists are sorted by stable-ID text. The condensation
graph is acyclic. Its reverse-topological order is callee-first: for `A -> B ->
C`, the required order is `C, B, A`. Ties are resolved by SCC ID.

Self-recursive functions form one-member recursive SCCs. Mutually recursive
functions form one SCC regardless of call insertion order.

---

# 7. Epistemic Algebra

M8 never produces `INFERRED` or `ASSUMED` facts from deterministic inputs.

For a supported call edge and a callee fact:

```text
MUST + MUST -> MUST
MUST + MAY  -> MAY
MAY  + MUST -> MAY
MAY  + MAY  -> MAY
```

`UNKNOWN` calls produce `UnknownCallEffect`; they do not create unbounded
`MayWrite` facts. `MUST_NOT` is not a positive edge or effect. Unsupported or
unspecified states are rejected at public mutation APIs and converted to a
scoped unknown only at the summary-import boundary.

When two derivations produce the same semantic fact, their epistemic states are
joined to the weaker positive state. A later derivation can never strengthen a
published result from `MAY` to `MUST`.

An `APPROXIMATED` SCC weakens every externally visible `MUST` result to `MAY`.

---

# 8. Fixpoint Domains and Engine

The V1 domains are finite maps with deterministic keys:

```text
TransitiveCalls:
    function_variant_id -> callee_function_variant_id -> epistemic

MayWrite:
    function_variant_id -> memory_location -> epistemic
```

Direct calls and direct writes seed the maps. Transfer across an edge joins the
edge epistemic state with the callee result. Join inserts a new key or weakens
an existing positive epistemic state. Widen is the identity operation because
both V1 domains are finite sets bounded by functions and summary memory
locations.

```cpp
namespace veritas::wpa {

enum class SccStatus {
  kConverged,
  kApproximated,
  kTimeout,
  kUnsupported,
};

struct FixpointBudget {
  std::size_t max_iterations;
};

struct SccResult {
  core::StableId scc_id;
  summary::v1::ComponentKind component_kind;
  std::string input_hash;
  std::string fixpoint_hash;
  std::string externally_visible_hash;
  std::size_t iteration_count;
  SccStatus status;
  std::vector<facts::FactTuple> facts;
};

class FixpointEngine {
 public:
  FixpointEngine(const CallGraph& call_graph, const SccGraph& scc_graph,
                 std::span<const summary::v1::FunctionSummary> summaries);

  StatusOr<std::vector<SccResult>> ComputeAll(
      summary::v1::ComponentKind component_kind,
      FixpointBudget budget);

  StatusOr<SccResult> Compute(
      core::StableId scc_id,
      summary::v1::ComponentKind component_kind,
      FixpointBudget budget);
};

}  // namespace veritas::wpa
```

`ComputeAll` evaluates SCCs in `ReverseTopologicalOrder` and returns results in
that same order. `Compute` evaluates the requested SCC after ensuring all of its
successor results are available in the engine cache.

The engine processes members in stable-ID order. Acyclic single-member SCCs
without a self-edge require one evaluation. Recursive SCCs iterate until the
domain is equivalent or the deterministic iteration budget is exhausted.
Budget exhaustion returns `kApproximated`; wall-clock time never participates
in deterministic output hashes.

Only `COMPONENT_KIND_CALLS` and `COMPONENT_KIND_MEMORY_EFFECTS` are supported in
M8. Other component kinds return an `SccResult` with `kUnsupported` and no
facts.

---

# 9. State Hashes and Incrementality

Each SCC component has three hashes:

```text
input_hash
    local component digests + successor SCC external hashes

fixpoint_hash
    complete converged per-member domain, epistemic states, and support IDs

externally_visible_hash
    facts visible to predecessor SCCs, excluding internal-only provenance
```

All inputs are canonically sorted and version tagged before SHA-256 hashing.
An internal provenance or support change may change `fixpoint_hash` while
leaving `externally_visible_hash` unchanged. In that case the new state is
persisted, but no predecessor work is enqueued.

The first successful computation is externally changed because no prior state
exists. A matching `input_hash` and converged prior state may be reused without
re-evaluation.

---

# 10. SQLite State

M8 adds four logical tables to the V1 metadata schema:

```text
wpa_sccs(
    scc_id, revision_id, build_variant_id, created_at,
    primary key(scc_id, revision_id, build_variant_id))

wpa_scc_members(
    scc_id, revision_id, build_variant_id, function_variant_id,
    primary key(scc_id, revision_id, build_variant_id, function_variant_id))

wpa_scc_edges(
    caller_scc_id, callee_scc_id, revision_id, build_variant_id, epistemic,
    primary key(caller_scc_id, callee_scc_id, revision_id, build_variant_id))

wpa_component_states(
    scc_id, revision_id, build_variant_id, component_kind,
    input_hash, fixpoint_hash, externally_visible_hash,
    iteration_count, status, updated_at,
    primary key(scc_id, revision_id, build_variant_id, component_kind))
```

`SccStateRepository` publishes a complete SCC graph transactionally and
upserts one component state transactionally. Failed publication leaves the
previous graph/state readable. Historical function summaries and M7 dependency
records are not deleted.

---

# 11. M7 Scheduler Handoff

After storing a component state, the repository reports:

```cpp
enum class ExternalChange {
  kUnchanged,
  kChanged,
};
```

For `kChanged`, the WPA coordinator creates one M7 `WorkItem` per predecessor
SCC:

```text
kind                 = kWpaComponent
target_id            = predecessor SCC ID
revision_id          = current revision
build_variant_id     = current build variant
consumer_component   = computed component kind
triggering_delta_ids = inherited triggering delta IDs
```

The existing `WorklistScheduler` deduplicates these items. For `kUnchanged`, no
predecessor item is created. M8 does not persist the in-memory M7 worklist.

---

# 12. Fact and Provenance Contract

```cpp
namespace veritas::facts {

enum class FactRelation {
  kDirectCall,
  kDirectRead,
  kDirectWrite,
  kLocalFlow,
  kMayAlias,
  kReachableCall,
  kMayWrite,
  kGlobalFlow,
};

struct FactTuple {
  core::StableId tuple_id;
  FactRelation relation;
  std::vector<std::string> columns;
  summary::v1::EpistemicState epistemic;
  std::string rule_id;
  std::vector<core::StableId> input_tuple_ids;
};

}  // namespace veritas::facts
```

Relation arity and column meaning are fixed:

```text
DirectCall(caller, callee)
DirectRead(function, memory)
DirectWrite(function, memory)
LocalFlow(source_value, destination_value, function)
MayAlias(memory_a, memory_b)
ReachableCall(source, destination)
MayWrite(function, memory)
GlobalFlow(source_value, destination_value)
```

M8 exports every base-relation shape above from summary facts. Its executable
C++ and Souffle derivations produce only `ReachableCall` and `MayWrite`.
`GlobalFlow` is reserved and validated as a relation name/arity so M9 and a
later domain can adopt it without changing the boundary version.

Every tuple ID uses `IdKind::kFact`.

- A base tuple ID hashes the relation, columns, epistemic state, originating
  summary ID, call site or memory location, and provenance reference.
- A derived tuple ID hashes the relation, columns, epistemic state, rule ID,
  and sorted immediate input tuple IDs.
- `rule_id` is empty for base tuples.
- `input_tuple_ids` is empty for base tuples and non-empty for derived tuples.
- Relation rows are sorted by tuple ID before export or publication.

M8 rule IDs are versioned constants:

```text
m8.reachable.direct.v1
m8.reachable.transitive.v1
m8.may_write.direct.v1
m8.may_write.transitive.v1
```

---

# 13. Souffle Boundary

## 13.1 Build behavior

`VERITAS_ENABLE_SOUFFLE` defaults to `ON`, but missing Souffle does not fail
configuration. The build records `VERITAS_HAS_SOUFFLE=0` and still builds:

- the C++ call graph and SCC graph;
- the C++ fixpoint engine;
- the fact schema;
- base relation export;
- derived relation import and provenance reconstruction.

Only `SouffleRunner` execution tests are skipped when the executable is absent
or the option is disabled. A disabled build must not compile or link against
Souffle headers or libraries.

## 13.2 Base export

`SouffleExporter::WriteBaseRelations` writes tab-separated UTF-8 rows with
deterministic escaping and row order:

```text
DirectCall.facts:  tuple_id, caller, callee, epistemic
DirectRead.facts:  tuple_id, function, memory, epistemic
DirectWrite.facts: tuple_id, function, memory, epistemic
LocalFlow.facts:   tuple_id, source_value, destination_value, function, epistemic
MayAlias.facts:    tuple_id, memory_a, memory_b, epistemic
```

It rejects embedded newlines, invalid stable IDs, invalid epistemic states, and
conflicting duplicate tuple IDs.

## 13.3 Derived import

Souffle emits semantic rows for `ReachableCall` and `MayWrite`. The importer
does not trust Souffle to create VERITAS IDs. It validates every row, applies
the epistemic algebra, reconstructs one canonical immediate derivation using
rule-specific joins, and then computes the derived stable tuple ID.

When multiple proofs exist, the canonical proof is selected by:

1. direct rule before transitive rule;
2. then lexicographically smallest ordered input tuple-ID sequence.

This makes tuple identity independent of Souffle evaluation order. Failure to
reconstruct a proof is `FailedPrecondition`; the row is never published without
provenance.

## 13.4 Rules

The optional rules implement:

```text
ReachableCall(f, g) :- DirectCall(f, g).
ReachableCall(f, h) :- DirectCall(f, g), ReachableCall(g, h).

MayWrite(f, x) :- DirectWrite(f, x).
MayWrite(f, x) :- DirectCall(f, g), MayWrite(g, x).
```

Epistemic columns are joined using the same truth table as the C++ engine. The
optional integration test compares imported Souffle semantic results with the
C++ fixpoint result for the same fixture.

---

# 14. Error Handling and Determinism

- Public APIs return `Status` or `StatusOr<T>`; no exceptions or RTTI are used.
- Malformed stable IDs, invalid epistemic states, and conflicting facts return
  `InvalidArgument`.
- Missing stable callee identity becomes a scoped unknown effect, not an error
  and not a graph-wide edge.
- Missing summaries or SCCs return `NotFound`.
- Malformed fact files report the relation and one-based row number.
- A failed Souffle process returns `Internal` and does not update SQLite state.
- SQLite writes use explicit transactions and rollback on every early return.
- Native addresses, hash-table iteration, timestamps, and wall-clock budgets
  never participate in stable IDs or state hashes.

---

# 15. Acceptance Tests

## 15.1 Identity and loading

- Direct extraction stores both diagnostic symbol and resolved callee variant.
- Current summaries load in function-variant order for one revision/build.
- A missing resolved callee creates a scoped unknown effect only.

## 15.2 SCC behavior

- `A -> B -> C` processes as `C, B, A`.
- A self-recursive function forms one SCC and converges.
- `A -> B -> A` forms one deterministic SCC regardless of insertion order.
- Unknown calls do not change SCC membership or create all-function fanout.

## 15.3 Fixpoint behavior

- Transitive calls derive `A -> C` through `A -> B -> C`.
- A write in `C` derives `MayWrite(A, X)` through `A -> B -> C`.
- A `MAY` edge weakens a transitive `MUST` write to `MAY`.
- Identical finite inputs converge to identical facts and hashes.
- Budget exhaustion returns `APPROXIMATED` and weakens external facts.

## 15.4 Incrementality

- SCC input, fixpoint, and external hashes persist and reload.
- An internal change with the same external hash stores the new fixpoint state
  without scheduling predecessors.
- An external hash change enqueues one deduplicated WPA item per predecessor.

## 15.5 Fact boundary

- Base export covers direct-call, direct-read, direct-write, local-flow, and
  may-alias tuples with stable IDs, semantic fields, and epistemic state in
  stable order.
- Every imported derived tuple carries a stable tuple ID, rule ID, and immediate
  input tuple IDs.
- Malformed or unsupported rows fail without partial results.
- When Souffle is available, imported reachability and may-write results equal
  the C++ engine's semantic results.
- When Souffle is disabled or absent, all C++ WPA and fact-boundary tests run
  and pass.

---

# 16. Handoff to M9

M9 consumes `FactTuple` values plus SCC convergence metadata. It persists exact
tuple IDs, semantic fact hashes, rule IDs, and immediate derivation edges.

M9 must not recompute M8 facts merely to recover provenance. M8 therefore hands
off only facts that already have validated identities, epistemic states, and
immediate input tuple IDs.
