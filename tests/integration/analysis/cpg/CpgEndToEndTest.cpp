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

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::analysis {
namespace {

namespace fs = std::filesystem;

ProjectAnalysisRequest FixtureRequest(std::string_view name) {
  const auto project = testing::FixtureProject(name);
  return ProjectAnalysisRequest{.project_root = project,
                                .output_root = project / ".veritas"};
}

TEST(CpgEndToEndTest, StandardAnalysisPublishesSummariesAndCpg) {
  ProjectAnalyzer analyzer;
  auto result = analyzer.AnalyzeProject(FixtureRequest("store_load"),
                                        AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->published_summary_ids.empty());
  EXPECT_FALSE(result->projection_id.empty());
  EXPECT_GT(result->cpg_node_count, 0u);
  EXPECT_GT(result->cpg_edge_count, 0u);
}

TEST(CpgEndToEndTest, ModeledExternalFunctionsDoNotBlockCpgProjection) {
  // SVF clones modeled external functions (e.g. malloc) into the module as
  // synthetic definitions that carry no function-variant identity. The CPG
  // projection must skip them rather than fail, so a program that calls the
  // modeled allocator still publishes a CPG.
  ProjectAnalyzer analyzer;
  auto result = analyzer.AnalyzeProject(FixtureRequest("modeled_external_call"),
                                        AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->projection_id.empty());
  EXPECT_GT(result->cpg_node_count, 0u);
}

TEST(CpgEndToEndTest, IdenticalInputsProduceCanonicalEquality) {
  ProjectAnalyzer analyzer;
  auto first = analyzer.AnalyzeProject(FixtureRequest("store_load"),
                                       AnalysisConfig::Default());
  auto second = analyzer.AnalyzeProject(FixtureRequest("store_load"),
                                        AnalysisConfig::Default());
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(first->projection_id, second->projection_id);
  EXPECT_EQ(first->cpg_node_count, second->cpg_node_count);
  EXPECT_EQ(first->cpg_edge_count, second->cpg_edge_count);
}

bool SourceTreeContains(const fs::path& root, const std::string& pattern) {
  if (!fs::exists(root)) return false;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path());
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().find(pattern) != std::string::npos) return true;
  }
  return false;
}

TEST(CpgEndToEndTest, PublicHeadersExposeNoNativeAnalysisTypes) {
  const fs::path include_root =
      testing::TestSourceRoot() / ".." / "include" / "veritas";
  EXPECT_FALSE(SourceTreeContains(include_root, "#include <llvm"));
  EXPECT_FALSE(SourceTreeContains(include_root, "#include <SVF"));
  EXPECT_FALSE(SourceTreeContains(include_root, "llvm::"));
  EXPECT_FALSE(SourceTreeContains(include_root, "SVF::"));
  EXPECT_FALSE(SourceTreeContains(include_root, "Joern"));
  EXPECT_FALSE(SourceTreeContains(include_root, "PhASAR"));
}

}  // namespace
}  // namespace veritas::analysis
