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

#include "veritas/analysis/ProjectAnalyzer.h"

#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"

namespace veritas::analysis {
namespace {

ProjectAnalysisRequest FixtureRequest() {
  return ProjectAnalysisRequest{
      .project_root = testing::FixtureProject("multiple_tus"),
      .output_root = testing::FixtureProject("multiple_tus") / ".veritas",
  };
}

TEST(ProjectAnalyzerWpaTest, DefaultAnalysisPublishesSouffleRunIdentity) {
  ProjectAnalyzer analyzer;
  auto result = analyzer.AnalyzeProject(FixtureRequest(), AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kSouffle);
  EXPECT_FALSE(result->wpa_run_id.empty());
  EXPECT_TRUE(result->wpa_diagnostics.empty());
}

TEST(ProjectAnalyzerWpaTest, ExplicitEmergencyUsesDistinctRunIdentity) {
  ProjectAnalyzer analyzer;

  auto souffle = analyzer.AnalyzeProject(FixtureRequest(), AnalysisConfig::Default());
  ASSERT_TRUE(souffle.ok()) << souffle.status().message();

  auto config = AnalysisConfig::Default();
  config.wpa_engine = WpaEngineMode::kCppEmergency;
  auto emergency = analyzer.AnalyzeProject(FixtureRequest(), config);
  ASSERT_TRUE(emergency.ok()) << emergency.status().message();

  EXPECT_EQ(emergency->wpa_engine, WpaEngineMode::kCppEmergency);
  EXPECT_FALSE(emergency->wpa_run_id.empty());
  // The emergency engine is a distinct run with a degraded-mode diagnostic.
  EXPECT_NE(emergency->wpa_run_id, souffle->wpa_run_id);
  EXPECT_FALSE(emergency->wpa_diagnostics.empty());
}

}  // namespace
}  // namespace veritas::analysis
