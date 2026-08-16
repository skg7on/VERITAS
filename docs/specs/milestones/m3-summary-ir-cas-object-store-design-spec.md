# M3 Summary IR and Immutable CAS Object Store Design Spec

**Status:** Draft
**Milestone:** M3
**Depends on:** M2 identity and metadata
**Feeds:** M4 local extraction, M7 incremental scheduling, M8 WPA, M10 Evidence APIs

---

# 1. Purpose

M3 defines the first durable Function Summary IR and stores summaries as immutable content-addressed objects. Real extraction precision arrives later, but the storage and hash contracts must be correct now.

The core invariant is:

```text
summary bytes are immutable
current bindings are mutable metadata
component hashes drive semantic invalidation
```

---

# 2. Summary Object Model

Logical summary shape:

```text
FunctionSummary {
    header
    identity
    component_hashes
    calls
    memory_effects
    value_flows
    control_flow_summaries
    range_facts
    alias_facts
    taint_transfers
    ownership_effects
    lock_effects
    state_transitions
    unknowns
    assumptions
    dependencies
    provenance_refs
}
```

Production representation should be Protobuf. Diagnostic JSON can exist for tests and CLI inspection.

---

# 3. Content Addressing

`FunctionSummaryID` is computed from canonical summary bytes.

The object store key is the summary content hash:

```text
summary:sha256:<digest>
```

Object store operations:

```text
put_if_absent(key, bytes)
get(key)
exists(key)
```

RocksDB is the V1 store. Tests may use an in-memory fake implementing the same interface.

---

# 4. Component Hashes

Every summary records both semantic and evidence hashes per component.

```text
ComponentDigest {
    component_kind
    semantic_hash
    evidence_hash
    item_count
    payload_offset
    payload_length
}
```

Semantic hash changes invalidate analysis consumers.

Evidence hash changes refresh explanations and Evidence slices but do not invalidate semantic WPA consumers.

Required component kinds:

```text
Calls
MemoryEffects
ValueFlow
ControlFlow
RangeFacts
AliasFacts
Taint
Ownership
Locks
State
Unknowns
Assumptions
Dependencies
Provenance
```

## 4.1 Versioned control-flow summary extension

M6 extends the versioned Summary IR with compact M4 control-flow anchors rather than persistent LLVM basic blocks:

```text
BasicBlockSummaryRef {
    basic_block_summary_id
    function_variant_id
    ordered_semantic_source_anchor_ids
    sorted_predecessor_successor_anchor_ids
}

DominatorSummaryFact {
    dominator
    dominated
    epistemic
    provenance_ref
}
```

`BasicBlockSummaryID` is the M2 canonical hash with kind prefix `bbsummary` over schema tag `bbsummary.v1`, owning `FunctionVariantID`, ordered mapped semantic `SourceAnchorID` members, and sorted mapped predecessor/successor anchor IDs. LLVM pointer identity and block order are forbidden inputs. M4 omits a block reference and emits a scoped unknown when the required mapped anchors are unavailable.

The `ControlFlow` semantic component hash includes canonical block IDs, dominance endpoints, and epistemic state. Its evidence hash additionally includes source-display and provenance fields. Reordering serialized repeated fields does not change either canonical component hash.

---

# 5. Metadata Publication

Current summary selection is a metadata binding:

```text
revision_id
build_variant_id
function_variant_id
function_summary_id
publication_epoch
is_current
```

Publication order:

```text
1. Write CAS object.
2. Insert summary object metadata.
3. Insert component rows.
4. Insert dependency rows if present.
5. In one transaction, replace current binding.
```

If a process crashes before metadata commit, the object is unreachable garbage. If it crashes after commit, the object must be readable.

---

# 6. API Contract

```cpp
namespace veritas::summary {
std::vector<ComponentDigest> ComputeComponentDigests(
    const veritas::summary::v1::FunctionSummary& summary);

StatusOr<core::StableId> ComputeFunctionSummaryId(
    const veritas::summary::v1::FunctionSummary& summary);
}
```

```cpp
namespace veritas::summarydb {
class ObjectStore {
 public:
  virtual Status PutIfAbsent(std::string_view key, std::span<const std::byte> bytes) = 0;
  virtual StatusOr<std::vector<std::byte>> Get(std::string_view key) const = 0;
};

class SummaryRepository {
 public:
  StatusOr<core::StableId> PublishSummary(
      const summary::v1::FunctionSummary& summary,
      const PublicationContext& context);
  StatusOr<summary::v1::FunctionSummary> GetSummary(core::StableId summary_id) const;
};
}
```

---

# 7. Summary Delta Foundation

M3 must expose enough component data for M7 to compute:

```text
SummaryDelta
ComponentDelta
semantic_changed
evidence_changed
```

M3 does not need the full scheduler, but it must store old and new component hashes accurately.

---

# 8. Acceptance Tests

Required tests:

```text
same summary -> same FunctionSummaryID
same object inserted twice -> one CAS object
range fact change -> RangeFacts semantic hash changes
control-flow topology change -> only ControlFlow semantic hash changes
control-flow source-display change -> only ControlFlow evidence hash changes
reordered BasicBlockSummaryRef records -> ControlFlow hashes remain stable
provenance ref change -> Provenance evidence hash changes
source anchor display change -> evidence hash changes only
failed metadata transaction leaves no current binding
current binding returns the newest published summary
historical summary remains readable after new binding
```

---

# 9. Handoff to M4

M4 consumes:

```text
summary.proto
ComponentDigest
BasicBlockSummaryRef and DominatorSummaryFact schema
SummaryRepository::PublishSummary
metadata publication context
```

M3 is complete when synthetic summaries can be published, retrieved, hashed by component, and rebound without mutating old objects.
