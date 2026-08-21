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

#ifndef VERITAS_RUNTIME_WORKLIST_SCHEDULER_H_
#define VERITAS_RUNTIME_WORKLIST_SCHEDULER_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::runtime {

// The kind of recomputation a work item requests. Ordering from most-local to
// most-global; a scheduler may use it to derive a default priority.
enum class WorkItemKind {
  kLocalSummary,
  kSccRecompute,
  kWpaComponent,
  kFactDerivation,
  kEvidenceInvalidation,
};

// A single scheduling unit. Deduplicated by (kind, target_id, revision_id,
// build_variant_id, consumer_component); duplicate enqueues merge their
// triggering delta ids rather than creating a second item.
struct WorkItem {
  WorkItemKind kind = WorkItemKind::kLocalSummary;
  core::StableId target_id;
  std::string revision_id;
  std::string build_variant_id;
  summary::v1::ComponentKind consumer_component =
      summary::v1::COMPONENT_KIND_UNSPECIFIED;
  std::vector<core::StableId> triggering_delta_ids;
  int priority = 0;
  int attempt_count = 0;
};

// A deterministic, deduplicating, priority-aware worklist. PopNext returns the
// lowest-priority item first, breaking ties by the deduplication key so the
// drain order is stable across runs.
class WorklistScheduler {
 public:
  void Enqueue(WorkItem item);

  std::optional<WorkItem> PopNext();

  bool Empty() const { return by_key_.empty(); }
  std::size_t PendingCount() const { return by_key_.size(); }

 private:
  std::map<std::string, WorkItem> by_key_;
};

}  // namespace veritas::runtime

#endif  // VERITAS_RUNTIME_WORKLIST_SCHEDULER_H_
