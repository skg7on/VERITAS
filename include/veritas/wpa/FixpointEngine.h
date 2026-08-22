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

#ifndef VERITAS_WPA_FIXPOINT_ENGINE_H_
#define VERITAS_WPA_FIXPOINT_ENGINE_H_

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/FactSchema.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

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
  FixpointEngine(const CallGraph &call_graph, const SccGraph &scc_graph,
                 std::span<const summary::v1::FunctionSummary> summaries);

  StatusOr<std::vector<SccResult>>
  ComputeAll(summary::v1::ComponentKind component_kind, FixpointBudget budget);

  StatusOr<SccResult> Compute(core::StableId scc_id,
                              summary::v1::ComponentKind component_kind,
                              FixpointBudget budget);

  std::span<const facts::FactTuple> BaseFacts() const { return base_facts_; }

private:
  struct CacheEntry {
    SccResult result;
    std::size_t max_iterations;
    std::vector<std::pair<core::StableId, std::string>>
        successor_fixpoint_hashes;
  };

  StatusOr<SccResult> Evaluate(core::StableId scc_id,
                               summary::v1::ComponentKind component_kind,
                               FixpointBudget budget);

  const CallGraph &call_graph_;
  const SccGraph &scc_graph_;
  std::map<core::StableId, summary::v1::FunctionSummary> summaries_;
  std::vector<facts::FactTuple> base_facts_;
  Status initialization_status_;
  std::map<std::pair<core::StableId, summary::v1::ComponentKind>, CacheEntry>
      cache_;
};

} // namespace veritas::wpa

#endif // VERITAS_WPA_FIXPOINT_ENGINE_H_
