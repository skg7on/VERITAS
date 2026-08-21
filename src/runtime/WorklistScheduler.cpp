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

#include "veritas/runtime/WorklistScheduler.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace veritas::runtime {

namespace {

// Serialize the deduplication key. Component values and the target ID string
// are domain-separated by '\x1f' so no two distinct tuples collide.
std::string WorkItemKey(const WorkItem& item) {
  return std::to_string(static_cast<int>(item.kind)) + '\x1f' +
         core::ToString(item.target_id) + '\x1f' + item.revision_id + '\x1f' +
         item.build_variant_id + '\x1f' +
         std::to_string(static_cast<int>(item.consumer_component));
}

}  // namespace

void WorklistScheduler::Enqueue(WorkItem item) {
  const std::string key = WorkItemKey(item);
  auto it = by_key_.find(key);
  if (it == by_key_.end()) {
    by_key_.emplace(key, std::move(item));
    return;
  }

  // Duplicate invalidation: collapse into the existing item, merging the
  // triggering delta ids so the merged work item carries every reason it was
  // scheduled.
  WorkItem& existing = it->second;
  for (const auto& delta_id : item.triggering_delta_ids) {
    if (std::find(existing.triggering_delta_ids.begin(),
                  existing.triggering_delta_ids.end(),
                  delta_id) == existing.triggering_delta_ids.end()) {
      existing.triggering_delta_ids.push_back(delta_id);
    }
  }
  existing.priority = std::min(existing.priority, item.priority);
  existing.attempt_count = std::max(existing.attempt_count, item.attempt_count);
}

std::optional<WorkItem> WorklistScheduler::PopNext() {
  if (by_key_.empty()) {
    return std::nullopt;
  }

  // Lowest priority first; ties broken by the dedup key for determinism.
  auto best = by_key_.begin();
  for (auto it = std::next(by_key_.begin()); it != by_key_.end(); ++it) {
    if (it->second.priority < best->second.priority ||
        (it->second.priority == best->second.priority &&
         it->first < best->first)) {
      best = it;
    }
  }

  WorkItem result = std::move(best->second);
  by_key_.erase(best);
  return result;
}

}  // namespace veritas::runtime
