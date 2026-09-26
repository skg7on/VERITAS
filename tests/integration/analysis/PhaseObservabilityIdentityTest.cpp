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

#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "ProjectFixture.h"

namespace veritas::analysis {
namespace {

namespace fs = std::filesystem;

TEST(PhaseObservabilityIdentityTest, RecordingDoesNotMoveAnyIdentity) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output_a = fs::temp_directory_path() /
                        ("veritas-metrics-off-" + std::to_string(std::rand()));
  const auto output_b = fs::temp_directory_path() /
                        ("veritas-metrics-on-" + std::to_string(std::rand()));

  auto request_a = veritas::analysis::ProjectAnalysisRequest{
      .project_root = project, .output_root = output_a};
  auto request_b = veritas::analysis::ProjectAnalysisRequest{
      .project_root = project, .output_root = output_b};
  const auto config = veritas::analysis::AnalysisConfig::Default();

  veritas::analysis::ProjectAnalyzer analyzer_a;
  auto result_a = analyzer_a.AnalyzeProject(request_a, config);
  ASSERT_TRUE(result_a.ok()) << result_a.status().message();

  veritas::core::RunMetricsOptions options;
  veritas::core::RunMetrics metrics(options);
  veritas::analysis::ProjectAnalyzer analyzer_b;
  auto result_b = analyzer_b.AnalyzeProject(request_b, config, &metrics);
  ASSERT_TRUE(result_b.ok()) << result_b.status().message();

  EXPECT_EQ(result_a->revision_id, result_b->revision_id);
  EXPECT_EQ(result_a->build_variant_id, result_b->build_variant_id);
  EXPECT_EQ(result_a->program_context_id, result_b->program_context_id);
  EXPECT_EQ(result_a->projection_id, result_b->projection_id);
  EXPECT_EQ(result_a->wpa_run_id, result_b->wpa_run_id);
  EXPECT_EQ(result_a->published_summary_ids, result_b->published_summary_ids);
  EXPECT_EQ(result_a->cpg_node_count, result_b->cpg_node_count);
  EXPECT_EQ(result_a->cpg_edge_count, result_b->cpg_edge_count);
  EXPECT_EQ(result_a->unknowns.size(), result_b->unknowns.size());
  // The recorder actually ran, so an empty tree cannot make this vacuous.
  EXPECT_FALSE(metrics.TakeStats().root.children.empty());
}

}  // namespace
}  // namespace veritas::analysis
