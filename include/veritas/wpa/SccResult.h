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

// SccResult.h — the shared SCC convergence/status types for the incremental WPA
// scheduler. Extracted from the retired FixpointEngine so SccStateRepository and
// WpaOrchestrator can describe a converged component without the legacy
// FactTuple fact system; derived facts now live in the V2 run repository.

#ifndef VERITAS_WPA_SCC_RESULT_H_
#define VERITAS_WPA_SCC_RESULT_H_

#include <cstddef>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::wpa {

enum class SccStatus {
  kConverged,
  kApproximated,
  kTimeout,
  kUnsupported,
};

struct SccResult {
  core::StableId scc_id;
  summary::v1::ComponentKind component_kind;
  std::string input_hash;
  std::string fixpoint_hash;
  std::string externally_visible_hash;
  std::size_t iteration_count;
  SccStatus status;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_SCC_RESULT_H_
