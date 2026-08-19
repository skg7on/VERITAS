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

namespace veritas::analysis {
namespace {

TEST(ProjectAnalyzerTest, DefaultConfigMatchesSvfDefaults) {
  auto config = AnalysisConfig::Default();

  EXPECT_GT(config.svf_soft_analysis_budget.count(), 0);
  EXPECT_GT(config.svf_max_graph_nodes, 0u);
  EXPECT_GT(config.svf_max_emitted_facts, 0u);
}

TEST(ProjectAnalyzerTest, ConstructsAndDestructs) {
  ProjectAnalyzer analyzer;
  // Should construct and destruct cleanly
}

TEST(ProjectAnalyzerTest, AnalyzeProjectReturnsResult) {
  ProjectAnalyzer analyzer;

  ProjectAnalysisRequest request{
      .project_root = "/nonexistent/project",
      .output_root = "/tmp/output",
  };

  // In real implementation with M1/M4, this would fail on missing project
  // For now, placeholder implementation returns success
  auto result = analyzer.AnalyzeProject(request, AnalysisConfig::Default());

  // Verify result has expected structure
  EXPECT_FALSE(result.program_context_id.empty());
}

TEST(ProjectAnalyzerTest, MovableNotCopyable) {
  ProjectAnalyzer analyzer1;
  ProjectAnalyzer analyzer2 = std::move(analyzer1);

  // Should compile and work
  static_assert(!std::is_copy_constructible_v<ProjectAnalyzer>);
  static_assert(std::is_move_constructible_v<ProjectAnalyzer>);
}

}  // namespace
}  // namespace veritas::analysis
