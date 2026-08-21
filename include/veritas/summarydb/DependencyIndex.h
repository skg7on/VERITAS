// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VERITAS_SUMMARYDB_DEPENDENCY_INDEX_H_
#define VERITAS_SUMMARYDB_DEPENDENCY_INDEX_H_

#include <cstddef>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/SummaryDelta.h"

namespace veritas::summarydb {

class MetadataStore;

// How a consumer depends on a producer component. Controls which kind of
// recomputation a component delta schedules (see Sensitivity).
enum class DependencyKind {
  kCall,
  kDataFlow,
  kMemoryEffect,
  kControlFlow,
  kRange,
  kAlias,
  kOther,
};

// Whether a dependency is sensitive to semantic content or to evidence only.
// A semantic delta schedules SEMANTIC consumers; an evidence-only delta
// schedules EVIDENCE_ONLY consumers.
enum class Sensitivity {
  kSemantic,
  kEvidenceOnly,
  kIdentity,
  kConfiguration,
};

// A dependency edge from a consumer summary component to a producer summary
// component.
struct DependencyEdge {
  core::StableId consumer_id;
  summary::v1::ComponentKind consumer_component;
  core::StableId producer_id;
  summary::v1::ComponentKind producer_component;
  DependencyKind dependency_kind;
  Sensitivity sensitivity;
};

// A resolved consumer of a producer component, returned by the reverse index.
struct ConsumerRef {
  core::StableId consumer_id;
  summary::v1::ComponentKind consumer_component;
  Sensitivity sensitivity;
};

// Budget applied to impact traversal. Exceeding either bound stops the walk
// and reports truncation so callers never mistake a partial result for a
// complete one.
struct ImpactBudget {
  std::size_t max_consumers = 1000;
  std::size_t max_depth = 16;
};

// The set of consumers transitively affected by a summary delta.
struct ImpactGraph {
  std::vector<ConsumerRef> consumers;  // deduplicated, deterministically ordered
  std::size_t explored_edges = 0;
  bool truncated = false;
};

// DependencyIndex maintains the current reverse dependency index keyed by
// producer component, plus an append-only historical record. It shares a
// MetadataStore with SummaryRepository so dependency replacement can be
// staged in the same transaction as summary publication.
class DependencyIndex {
 public:
  explicit DependencyIndex(MetadataStore& metadata_store);

  // Atomically replace the current dependency edges for a consumer: delete the
  // consumer's current reverse-index rows, insert the new current rows, and
  // append historical rows. All within one transaction.
  Status ReplaceCurrentDependencies(
      core::StableId consumer_summary_id,
      const std::vector<DependencyEdge>& edges);

  // Look up the current consumers of a producer component, ordered
  // deterministically by (consumer_id, consumer_component).
  StatusOr<std::vector<ConsumerRef>> UsersOf(
      core::StableId producer_id,
      summary::v1::ComponentKind producer_component) const;

  // Traverse the reverse index transitively from a summary delta and return
  // the affected consumers. Semantic component deltas follow SEMANTIC edges
  // only; evidence-only deltas follow EVIDENCE_ONLY edges only. Traversal is
  // budgeted and reports truncation explicitly.
  StatusOr<ImpactGraph> GetImpactSet(const SummaryDelta& delta,
                                     ImpactBudget budget) const;

 private:
  MetadataStore& metadata_store_;
};

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_DEPENDENCY_INDEX_H_
