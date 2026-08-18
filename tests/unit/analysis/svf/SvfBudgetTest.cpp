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

#include "analysis/svf/SvfBudget.h"

#include <gtest/gtest.h>

namespace veritas::analysis::svf {
namespace {

// Fake clock for deterministic testing
class FakeClock {
 public:
  FakeClock() : current_(std::chrono::steady_clock::time_point{}) {}

  std::chrono::steady_clock::time_point Now() const { return current_; }

  void Advance(std::chrono::seconds delta) { current_ += delta; }

 private:
  std::chrono::steady_clock::time_point current_;
};

TEST(SvfBudgetTest, DefaultConstructionStartsWithNoBudgetViolation) {
  FakeClock clock;
  SvfBudget budget(SvfConfig::Default(), [&]() { return clock.Now(); });

  EXPECT_EQ(budget.state().reason, BudgetReason::kNone);
  EXPECT_EQ(budget.state().observed_graph_nodes, 0u);
  EXPECT_EQ(budget.state().emitted_facts, 0u);
}

TEST(SvfBudgetTest, CheckpointDetectsGraphNodeLimit) {
  FakeClock clock;
  auto config = SvfConfig::Default();
  config.max_graph_nodes = 100;
  SvfBudget budget(config, [&]() { return clock.Now(); });

  EXPECT_TRUE(budget.Checkpoint(50));
  EXPECT_EQ(budget.state().reason, BudgetReason::kNone);

  EXPECT_FALSE(budget.Checkpoint(150));
  EXPECT_EQ(budget.state().reason, BudgetReason::kGraphNodeLimit);
  EXPECT_EQ(budget.state().observed_graph_nodes, 150u);
}

TEST(SvfBudgetTest, CheckpointDetectsTimeLimit) {
  FakeClock clock;
  auto config = SvfConfig::Default();
  config.soft_analysis_budget = std::chrono::seconds(10);
  SvfBudget budget(config, [&]() { return clock.Now(); });

  clock.Advance(std::chrono::seconds(5));
  EXPECT_TRUE(budget.Checkpoint(100));
  EXPECT_EQ(budget.state().reason, BudgetReason::kNone);

  clock.Advance(std::chrono::seconds(10));
  EXPECT_FALSE(budget.Checkpoint(200));
  EXPECT_EQ(budget.state().reason, BudgetReason::kTimeLimit);
  EXPECT_GE(budget.state().elapsed, std::chrono::seconds(10));
}

TEST(SvfBudgetTest, TryEmitDetectsFactLimit) {
  FakeClock clock;
  auto config = SvfConfig::Default();
  config.max_emitted_facts = 2;
  SvfBudget budget(config, [&]() { return clock.Now(); });

  EXPECT_TRUE(budget.TryEmit());
  EXPECT_EQ(budget.state().emitted_facts, 1u);

  EXPECT_TRUE(budget.TryEmit());
  EXPECT_EQ(budget.state().emitted_facts, 2u);

  EXPECT_FALSE(budget.TryEmit());
  EXPECT_EQ(budget.state().reason, BudgetReason::kFactLimit);
  EXPECT_EQ(budget.state().emitted_facts, 2u);
}

TEST(SvfBudgetTest, OnceOverBudgetAllOperationsFail) {
  FakeClock clock;
  auto config = SvfConfig::Default();
  config.max_graph_nodes = 100;
  config.max_emitted_facts = 10;
  SvfBudget budget(config, [&]() { return clock.Now(); });

  // Exceed graph node limit
  EXPECT_FALSE(budget.Checkpoint(200));
  EXPECT_EQ(budget.state().reason, BudgetReason::kGraphNodeLimit);

  // Subsequent operations fail immediately
  EXPECT_FALSE(budget.TryEmit());
  EXPECT_FALSE(budget.Checkpoint(50));
}

TEST(SvfBudgetTest, BudgetReasonNameReturnsValidStrings) {
  EXPECT_STREQ(BudgetReasonName(BudgetReason::kNone), "none");
  EXPECT_STREQ(BudgetReasonName(BudgetReason::kTimeLimit), "time_limit");
  EXPECT_STREQ(BudgetReasonName(BudgetReason::kGraphNodeLimit),
               "graph_node_limit");
  EXPECT_STREQ(BudgetReasonName(BudgetReason::kFactLimit), "fact_limit");
}

}  // namespace
}  // namespace veritas::analysis::svf
