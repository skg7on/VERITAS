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

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::analysis {
namespace {

TEST(WpaEmergencyModeTest, ExplicitCppModeIsDistinctAndDegraded) {
  const auto project = testing::FixtureProject("multiple_tus");
  const ProjectAnalysisRequest request{.project_root = project,
                                       .output_root = project / ".veritas"};

  ProjectAnalyzer analyzer;
  auto production = analyzer.AnalyzeProject(request, AnalysisConfig::Default());
  ASSERT_TRUE(production.ok()) << production.status().message();

  auto config = AnalysisConfig::Default();
  config.wpa_engine = WpaEngineMode::kCppEmergency;

  auto result = analyzer.AnalyzeProject(request, config);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kCppEmergency);
  EXPECT_FALSE(result->wpa_run_id.empty());
  EXPECT_NE(result->wpa_run_id, production->wpa_run_id);
  EXPECT_THAT(result->wpa_diagnostics, ::testing::HasSubstr("degraded"));
}

} // namespace
} // namespace veritas::analysis
