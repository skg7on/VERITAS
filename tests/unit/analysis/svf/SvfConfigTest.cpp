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

#include "analysis/svf/SvfConfig.h"

#include <gtest/gtest.h>

namespace veritas::analysis::svf {
namespace {

TEST(SvfConfigTest, DefaultIsRequiredBoundedAndersenWaveDiff) {
  const auto config = SvfConfig::Default();
  EXPECT_EQ(config.pointer_analysis, PointerAnalysisKind::kAndersenWaveDiff);
  EXPECT_GT(config.soft_analysis_budget.count(), 0);
  EXPECT_GT(config.max_graph_nodes, 0u);
  EXPECT_GT(config.max_emitted_facts, 0u);
  EXPECT_TRUE(config.field_sensitive);
}

TEST(SvfConfigTest, CanonicalConfigIsDeterministic) {
  const auto config = SvfConfig::Default();
  const auto canonical1 = config.CanonicalAnalyzerConfig();
  const auto canonical2 = config.CanonicalAnalyzerConfig();
  EXPECT_EQ(canonical1, canonical2);
  EXPECT_FALSE(canonical1.empty());
}

TEST(SvfConfigTest, CanonicalConfigContainsAllFields) {
  const auto config = SvfConfig::Default();
  const auto canonical = config.CanonicalAnalyzerConfig();

  EXPECT_NE(canonical.find("pointer_analysis="), std::string::npos);
  EXPECT_NE(canonical.find("soft_analysis_budget_seconds="), std::string::npos);
  EXPECT_NE(canonical.find("max_graph_nodes="), std::string::npos);
  EXPECT_NE(canonical.find("max_emitted_facts="), std::string::npos);
  EXPECT_NE(canonical.find("field_sensitive="), std::string::npos);
}

}  // namespace
}  // namespace veritas::analysis::svf
