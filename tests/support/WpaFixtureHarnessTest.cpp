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
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "WpaFixtureHarness.h"

namespace veritas::testing {
namespace {

namespace fs = std::filesystem;

std::set<fs::path> FixtureRoots(std::string_view fixture_name) {
  const std::string prefix = "veritas-fixture-" +
                             std::string(fixture_name) + "-";
  std::set<fs::path> roots;
  for (const auto& entry : fs::directory_iterator(fs::temp_directory_path())) {
    if (entry.is_directory() &&
        entry.path().filename().string().starts_with(prefix)) {
      roots.insert(entry.path());
    }
  }
  return roots;
}

void RemoveNewFixtureRoots(const std::set<fs::path>& before,
                           const std::set<fs::path>& after) {
  for (const auto& root : after) {
    if (!before.contains(root)) {
      std::error_code error;
      fs::remove_all(root, error);
    }
  }
}

TEST(WpaFixtureHarnessTest, AnalysisAndReloadShareOneFixtureCopy) {
  auto snapshot = AnalyzeAndLoadFixture("function_pointer",
                                        analysis::AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_EQ(snapshot->project_root / ".veritas", snapshot->output_root);
  EXPECT_FALSE(snapshot->analysis.published_summary_ids.empty());
  EXPECT_EQ(snapshot->summaries.size(),
            snapshot->analysis.published_summary_ids.size());
  EXPECT_TRUE(AllArtifactsAreV2(snapshot->summaries));
}

TEST(WpaFixtureHarnessTest, AnalyzedSnapshotCleansFixtureAfterScope) {
  fs::path project_root;
  {
    auto snapshot = AnalyzeAndLoadFixture("function_pointer",
                                          analysis::AnalysisConfig::Default());
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
    project_root = snapshot->project_root;
    EXPECT_TRUE(fs::exists(project_root));
  }
  EXPECT_FALSE(fs::exists(project_root));
  std::error_code error;
  fs::remove_all(project_root, error);
}

TEST(WpaFixtureHarnessTest, SvfSnapshotCleansFixtureAfterScope) {
  fs::path project_root;
  {
    auto snapshot = MapFixtureWithSvf("function_pointer");
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
    project_root = snapshot->project_root;
    EXPECT_TRUE(fs::exists(project_root));
  }
  EXPECT_FALSE(fs::exists(project_root));
  std::error_code error;
  fs::remove_all(project_root, error);
}

TEST(WpaFixtureHarnessTest, SnapshotMoveTransfersFixtureCleanup) {
  auto snapshot = AnalyzeAndLoadFixture("function_pointer",
                                        analysis::AnalysisConfig::Default());
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  const fs::path project_root = snapshot->project_root;
  const auto calls = CallsWithDispatch(
      snapshot->summaries, summary::v2::DISPATCH_KIND_INDIRECT);
  ASSERT_FALSE(calls.empty());
  const auto* call = calls.front();
  const std::string call_site_id = call->call_site_id();

  {
    AnalyzedFixtureSnapshot moved = std::move(*snapshot);
    EXPECT_TRUE(fs::exists(project_root));
    EXPECT_EQ(call->call_site_id(), call_site_id);
  }

  EXPECT_FALSE(fs::exists(project_root));
  std::error_code error;
  fs::remove_all(project_root, error);
}

TEST(WpaFixtureHarnessTest, SnapshotsAreMoveOnly) {
  EXPECT_FALSE(std::is_copy_constructible_v<SvfFixtureSnapshot>);
  EXPECT_FALSE(std::is_copy_assignable_v<SvfFixtureSnapshot>);
  EXPECT_TRUE(std::is_move_constructible_v<SvfFixtureSnapshot>);
  EXPECT_FALSE(std::is_copy_constructible_v<AnalyzedFixtureSnapshot>);
  EXPECT_FALSE(std::is_copy_assignable_v<AnalyzedFixtureSnapshot>);
  EXPECT_TRUE(std::is_move_constructible_v<AnalyzedFixtureSnapshot>);
}

TEST(WpaFixtureHarnessTest, FailedAnalysisCleansFixtureCopy) {
  const auto before = FixtureRoots("function_pointer");
  auto config = analysis::AnalysisConfig::Default();
  config.wpa_threads = 0;

  auto failed = AnalyzeAndLoadFixture("function_pointer", config);
  ASSERT_FALSE(failed.ok());

  const auto after = FixtureRoots("function_pointer");
  EXPECT_EQ(after, before);
  RemoveNewFixtureRoots(before, after);
}

}  // namespace
}  // namespace veritas::testing
