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

#ifndef VERITAS_WPA_SCC_GRAPH_H_
#define VERITAS_WPA_SCC_GRAPH_H_

#include <map>
#include <span>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/wpa/CallGraph.h"

namespace veritas::wpa {

class SccGraph {
public:
  static StatusOr<SccGraph> Build(const CallGraph &call_graph);

  StatusOr<core::StableId>
  SccForFunction(core::StableId function_variant_id) const;
  StatusOr<std::span<const core::StableId>>
  Members(core::StableId scc_id) const;
  StatusOr<std::span<const core::StableId>>
  Predecessors(core::StableId scc_id) const;
  StatusOr<std::span<const core::StableId>>
  Successors(core::StableId scc_id) const;
  std::span<const core::StableId> ReverseTopologicalOrder() const {
    return reverse_topological_order_;
  }

private:
  std::map<core::StableId, core::StableId> function_to_scc_;
  std::map<core::StableId, std::vector<core::StableId>> members_;
  std::map<core::StableId, std::vector<core::StableId>> predecessors_;
  std::map<core::StableId, std::vector<core::StableId>> successors_;
  std::vector<core::StableId> reverse_topological_order_;
};

} // namespace veritas::wpa

#endif // VERITAS_WPA_SCC_GRAPH_H_
