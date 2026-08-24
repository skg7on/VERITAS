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
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/summarydb/SummaryRepository.h"

#include <filesystem>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ProjectFixture.h"

namespace veritas::analysis {
namespace {

namespace v1 = veritas::summary::v1;
namespace v2 = veritas::summary::v2;

// Materialize a fixture exactly once per name so the analysis and the
// repository-open share the same on-disk project (FixtureProject returns a
// fresh directory on every call).
const std::filesystem::path& FixtureRoot(std::string_view name) {
  static std::map<std::string, std::filesystem::path> cache;
  auto [it, inserted] = cache.emplace(std::string(name), std::filesystem::path{});
  if (inserted) {
    it->second = testing::FixtureProject(name);
  }
  return it->second;
}

StatusOr<ProjectAnalysisResult> AnalyzeProjectFixture(std::string_view name) {
  const auto project = FixtureRoot(name);
  ProjectAnalysisRequest request{
      .project_root = project,
      .output_root = project / ".veritas",
  };
  ProjectAnalyzer analyzer;
  return analyzer.AnalyzeProject(request, AnalysisConfig::Default());
}

std::unique_ptr<summarydb::SummaryRepository> OpenFixtureRepository(
    std::string_view name) {
  const auto root = FixtureRoot(name) / ".veritas";
  auto repository = summarydb::SummaryRepository::Open(root.string());
  if (!repository.ok()) {
    return nullptr;
  }
  return std::move(*repository);
}

// True when any V2 summary carries an indirect call target published as a
// stable MAY call (a function-pointer target resolved by SVF).
bool ContainsIndirectMayTarget(
    const std::vector<summary::SummaryArtifact>& summaries) {
  for (const auto& artifact : summaries) {
    const auto* summary = std::get_if<v2::FunctionSummary>(&artifact);
    if (!summary) {
      continue;
    }
    for (const auto& call : summary->calls()) {
      if (call.dispatch() == v2::DISPATCH_KIND_INDIRECT &&
          call.epistemic() == v1::EPISTEMIC_STATE_MAY) {
        return true;
      }
    }
  }
  return false;
}

TEST(SummaryV2PipelineTest, PublishesIndirectTargetsAndStructuredMemory) {
  auto result = AnalyzeProjectFixture("function_pointer");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto repository = OpenFixtureRepository("function_pointer");
  ASSERT_NE(repository, nullptr);
  auto summaries = repository->ListCurrentSummaryArtifacts(
      result->revision_id, result->build_variant_id);
  ASSERT_TRUE(summaries.ok()) << summaries.status().message();
  EXPECT_TRUE(std::ranges::all_of(*summaries, [](const auto& artifact) {
    return std::holds_alternative<v2::FunctionSummary>(artifact);
  }));
  EXPECT_TRUE(ContainsIndirectMayTarget(*summaries));
}

}  // namespace
}  // namespace veritas::analysis
