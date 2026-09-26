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

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/core/Status.h"

namespace veritas::build {
namespace {

namespace fs = std::filesystem;

TEST(ProjectInputTest, ResolvesCompileDatabaseInsideProjectRoot) {
  const auto project = testing::FixtureProject("smoke");
  const analysis::ProjectAnalysisRequest request{
      .project_root = project,
      .output_root = {},
  };

  auto resolved = ResolveProjectInput(request);
  ASSERT_TRUE(resolved.ok()) << resolved.status().message();

  EXPECT_EQ(resolved->project_root, project);
  EXPECT_EQ(resolved->compile_database_path,
            resolved->project_root / "compile_commands.json");
  EXPECT_EQ(resolved->output_root, resolved->project_root / ".veritas");
}

TEST(ProjectInputTest, DefaultsOutputRootToDotVeritas) {
  const auto project = testing::FixtureProject("smoke");
  const analysis::ProjectAnalysisRequest request{
      .project_root = project,
      .output_root = {},
  };

  ASSERT_TRUE(ResolveProjectInput(request).ok());
  auto resolved = *ResolveProjectInput(request);
  EXPECT_TRUE(resolved.output_root.is_absolute());
  EXPECT_EQ(resolved.output_root.filename(), ".veritas");
}

TEST(ProjectInputTest, HonorsExplicitOutputRoot) {
  const auto project = testing::FixtureProject("smoke");
  const auto output = fs::temp_directory_path() / "veritas-cli-output-explicit";
  const analysis::ProjectAnalysisRequest request{
      .project_root = project,
      .output_root = output,
  };

  auto resolved = ResolveProjectInput(request);
  ASSERT_TRUE(resolved.ok()) << resolved.status().message();
  EXPECT_TRUE(resolved->output_root.is_absolute());
  EXPECT_NE(resolved->output_root, resolved->project_root / ".veritas");
}

TEST(ProjectInputTest, RejectsProjectWithoutCompileDatabase) {
  const auto project = testing::FixtureProject("missing_compile_database");
  const analysis::ProjectAnalysisRequest request{
      .project_root = project,
      .output_root = {},
  };

  auto resolved = ResolveProjectInput(request);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.status().code(), StatusCode::kFailedPrecondition);
}

TEST(ProjectInputTest, RejectsMissingProjectDirectory) {
  const auto missing =
      fs::temp_directory_path() / "veritas-fixture-does-not-exist";
  const analysis::ProjectAnalysisRequest request{
      .project_root = missing,
      .output_root = {},
  };

  auto resolved = ResolveProjectInput(request);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.status().code(), StatusCode::kInvalidArgument);
}

TEST(ProjectInputTest, RejectsProjectPathThatIsAFile) {
  const auto project = testing::FixtureProject("smoke");
  const auto file_path = project / "smoke.cpp";
  const analysis::ProjectAnalysisRequest request{
      .project_root = file_path,
      .output_root = {},
  };

  auto resolved = ResolveProjectInput(request);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.status().code(), StatusCode::kInvalidArgument);
}

TEST(ProjectInputTest, RejectsEmptyProjectPath) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = {},
      .output_root = {},
  };
  auto resolved = ResolveProjectInput(request);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace veritas::build
