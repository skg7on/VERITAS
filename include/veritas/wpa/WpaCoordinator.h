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

#ifndef VERITAS_WPA_WPA_COORDINATOR_H_
#define VERITAS_WPA_WPA_COORDINATOR_H_

#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/runtime/WorklistScheduler.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SccStateRepository.h"

namespace veritas::wpa {

class WpaCoordinator {
 public:
  static Status EnqueuePredecessorsIfChanged(
      ExternalChange change, core::StableId changed_scc,
      summary::v1::ComponentKind component_kind,
      const SccContext& context,
      std::vector<core::StableId> triggering_delta_ids,
      const SccGraph& scc_graph,
      runtime::WorklistScheduler* scheduler);
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_COORDINATOR_H_
