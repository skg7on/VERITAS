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

#include "veritas/wpa/WpaCoordinator.h"

#include <utility>

namespace veritas::wpa {

Status WpaCoordinator::EnqueuePredecessorsIfChanged(
    ExternalChange change, core::StableId changed_scc,
    summary::v1::ComponentKind component_kind, const SccContext &context,
    std::vector<core::StableId> triggering_delta_ids, const SccGraph &scc_graph,
    runtime::WorklistScheduler *scheduler) {
  if (scheduler == nullptr) {
    return Status::InvalidArgument("WPA scheduler must not be null");
  }
  if (change == ExternalChange::kUnchanged)
    return Status::Ok();
  auto predecessors = scc_graph.Predecessors(std::move(changed_scc));
  if (!predecessors.ok())
    return predecessors.status();
  for (const auto &predecessor : *predecessors) {
    scheduler->Enqueue(runtime::WorkItem{
        .kind = runtime::WorkItemKind::kWpaComponent,
        .target_id = predecessor,
        .revision_id = context.revision_id,
        .build_variant_id = context.build_variant_id,
        .consumer_component = component_kind,
        .triggering_delta_ids = triggering_delta_ids,
    });
  }
  return Status::Ok();
}

} // namespace veritas::wpa
