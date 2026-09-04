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

#include "ProjectFixture.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"

namespace veritas::analysis {
namespace {

StatusOr<build::AnalysisManifest> LoadFixtureManifest(
    const std::filesystem::path& root) {
  const ProjectAnalysisRequest request{
      .project_root = root,
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok()) return input.status();
  return build::LoadProjectManifest(*input);
}

TEST(SemanticZooFixtureTest, LinksTwoCAndFourCppTranslationUnits) {
  const auto root = testing::FixtureProject("semantic_zoo");
  auto manifest = LoadFixtureManifest(root);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  EXPECT_EQ(manifest->translation_units.size(), 6u);
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  EXPECT_NE(local->program_ir.GetFunction("zoo_driver"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_callback_parameter"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_virtual_select"), nullptr);
  EXPECT_NE(local->program_ir.GetFunction("zoo_recursive_entry"), nullptr);
}

}  // namespace
}  // namespace veritas::analysis
