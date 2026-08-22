# M7 Reverse Dependency Index and Incremental Scheduler Design Spec

**Status:** Draft
**Milestone:** M7
**Depends on:** M3 component hashes, M4/M5 summary facts
**Feeds:** M8 WPA, M10B stale Evidence detection

---

# 1. Purpose

M7 implements semantic incremental recomputation. It converts summary component deltas into precise worklist items by looking up consumers of the changed producer component.

The key question is:

> If function F's range component changed, which consumers actually depend on F.range?

The answer must not be "all callers" unless every caller truly consumes that component.

---

# 2. Core Concepts

```text
SummaryDelta:
    old_summary_id
    new_summary_id
    changed_components

ComponentDelta:
    component_kind
    old_semantic_hash
    new_semantic_hash
    old_evidence_hash
    new_evidence_hash

DependencyEdge:
    consumer_id
    consumer_component
    producer_id
    producer_component
    dependency_kind
    sensitivity
```

Sensitivity:

```text
SEMANTIC
EVIDENCE_ONLY
IDENTITY
CONFIGURATION
```

---

# 3. Reverse Dependency Index

Hot lookup shape:

```text
producer_kind
producer_id
producer_component
    -> consumer_kind
    -> consumer_id
    -> consumer_component
    -> sensitivity
```

Example:

```text
producer = validatePacket.range
consumer = decodeIE.value_flow
sensitivity = SEMANTIC
```

When `validatePacket.range` changes, `decodeIE.value_flow` is scheduled. If only `validatePacket.provenance` changes, it is not scheduled for semantic recomputation.

---

# 4. Worklist Scheduler

Work item:

```text
WorkItem {
    kind
    target_id
    revision_id
    build_variant_id
    consumer_component
    triggering_delta_ids[]
    priority
    attempt_count
}
```

Kinds:

```text
LOCAL_SUMMARY
SCC_RECOMPUTE
WPA_COMPONENT
FACT_DERIVATION
EVIDENCE_INVALIDATION
```

Deduplication key:

```text
kind
target_id
revision_id
build_variant_id
consumer_component
```

---

# 5. Delta Semantics

```text
semantic hash unchanged, evidence hash unchanged
    -> no scheduling

semantic hash unchanged, evidence hash changed
    -> schedule EVIDENCE_INVALIDATION consumers only

semantic hash changed
    -> schedule semantic consumers for changed component

identity/config changed
    -> broad invalidation according to dependency kind
```

This split is required for large repositories, where source churn should not automatically become WPA churn.

---

# 6. Index Reconciliation

After a consumer recomputes:

```text
1. Keep old summary's historical dependency rows.
2. Remove old rows from current reverse_dependency_index.
3. Insert new current reverse index rows.
4. Publish summary delta and scheduled work in one metadata transaction.
```

A validation command should be able to rebuild the hot reverse index from current summary dependencies and compare it to stored rows.

---

# 7. API Contract

```cpp
namespace veritas::summarydb {
class DependencyIndex {
 public:
  Status ReplaceCurrentDependencies(
      core::StableId consumer_summary_id,
      std::vector<DependencyEdge> edges);

  StatusOr<std::vector<ConsumerRef>> UsersOf(
      core::StableId producer_id,
      summary::ComponentKind producer_component) const;

  StatusOr<ImpactGraph> GetImpactSet(
      core::StableId delta_id,
      ImpactBudget budget) const;
};
}
```

```cpp
namespace veritas::runtime {
class WorklistScheduler {
 public:
  void Enqueue(WorkItem item);
  std::optional<WorkItem> PopNext();
  bool Empty() const;
  size_t PendingCount() const;
};
}
```

---

# 8. Acceptance Tests

Required tests:

```text
range-only delta schedules range consumers, not call graph consumers
call-only delta schedules SCC consumers
provenance-only delta schedules evidence consumers only
duplicate scheduling collapses to one work item
old reverse index rows are removed from hot index after republish
historical dependencies remain explainable
impact budget truncates output explicitly
```

---

# 9. Handoff to M8

M8 consumes scheduled SCC and WPA work items. M7 is complete when `veritas-diff` can report changed components and the scheduler can produce precise recomputation targets for fixture mutations.
