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

#include "SvfBudget.h"

namespace veritas::analysis::svf {

const char* BudgetReasonName(BudgetReason reason) {
  switch (reason) {
    case BudgetReason::kNone:
      return "none";
    case BudgetReason::kTimeLimit:
      return "time_limit";
    case BudgetReason::kGraphNodeLimit:
      return "graph_node_limit";
    case BudgetReason::kFactLimit:
      return "fact_limit";
  }
  return "unknown";
}

SvfBudget::SvfBudget(SvfConfig config, Now now)
    : config_(std::move(config)),
      now_(std::move(now)),
      started_(now_()) {}

bool SvfBudget::Checkpoint(std::size_t observed_graph_nodes) {
  // Already over budget
  if (state_.reason != BudgetReason::kNone) {
    return false;
  }

  state_.observed_graph_nodes = observed_graph_nodes;
  state_.elapsed = now_() - started_;

  // Check graph node limit
  if (observed_graph_nodes > config_.max_graph_nodes) {
    state_.reason = BudgetReason::kGraphNodeLimit;
    return false;
  }

  // Check time limit
  if (state_.elapsed > config_.soft_analysis_budget) {
    state_.reason = BudgetReason::kTimeLimit;
    return false;
  }

  return true;
}

bool SvfBudget::TryEmit() {
  // Already over budget
  if (state_.reason != BudgetReason::kNone) {
    return false;
  }

  // Check fact limit before incrementing
  if (state_.emitted_facts >= config_.max_emitted_facts) {
    state_.reason = BudgetReason::kFactLimit;
    return false;
  }

  ++state_.emitted_facts;
  return true;
}

}  // namespace veritas::analysis::svf
