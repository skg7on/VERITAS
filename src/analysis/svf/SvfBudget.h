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

#ifndef VERITAS_ANALYSIS_SVF_SVFBUDGET_H_
#define VERITAS_ANALYSIS_SVF_SVFBUDGET_H_

#include <chrono>
#include <cstddef>
#include <functional>

#include "analysis/svf/SvfConfig.h"

namespace veritas::analysis::svf {

// BudgetReason indicates why a budget limit was reached
enum class BudgetReason {
  kNone,
  kTimeLimit,
  kGraphNodeLimit,
  kFactLimit,
};

// BudgetReasonName converts BudgetReason to a string for provenance
const char* BudgetReasonName(BudgetReason reason);

// SvfBudgetState tracks observed resource usage and limit status
struct SvfBudgetState {
  BudgetReason reason = BudgetReason::kNone;
  std::size_t observed_graph_nodes = 0;
  std::size_t emitted_facts = 0;
  std::chrono::steady_clock::duration elapsed{};
};

// SvfBudget implements checkpoint-based resource tracking for SVF analysis.
//
// Tracks graph node count, emitted fact count, and elapsed time against
// configured limits. Checkpoints occur at supported phases where partial
// results can be validated. TryEmit() must be called before every fact
// emission to enforce the fact limit.
//
// Soft limits never interrupt SVF mid-operation; they return false at the
// next checkpoint or emission attempt.
class SvfBudget {
 public:
  using Now = std::function<std::chrono::steady_clock::time_point()>;

  // Construct with config limits and a clock function (for testing)
  explicit SvfBudget(SvfConfig config,
                     Now now = std::chrono::steady_clock::now);

  // Checkpoint checks time and graph node limits at a supported phase boundary.
  // Returns true if within budget, false if a limit was reached.
  bool Checkpoint(std::size_t observed_graph_nodes);

  // TryEmit checks if another fact can be emitted within the fact limit.
  // Returns true and increments the counter if within budget, false otherwise.
  bool TryEmit();

  // state returns the current budget state (reason, counts, elapsed)
  const SvfBudgetState& state() const { return state_; }

 private:
  SvfConfig config_;
  Now now_;
  std::chrono::steady_clock::time_point started_;
  SvfBudgetState state_;
};

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFBUDGET_H_
