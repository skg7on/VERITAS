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

#include <string_view>

#include "ProjectFixture.h"
#include "frontend/clang/ProjectAstExtractor.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"

namespace veritas::frontend::clang {
namespace {

StatusOr<build::AnalysisManifest> LoadFixtureManifest(std::string_view name) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = testing::FixtureProject(name),
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok()) return input.status();
  return build::LoadProjectManifest(*input);
}

TEST(ProjectAstExtractorTest, ExtractsFunctionsFromEveryTranslationUnit) {
  auto manifest = LoadFixtureManifest("multiple_tus");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  ProjectAstExtractor extractor;
  auto index = extractor.ExtractProject(*manifest);
  ASSERT_TRUE(index.ok()) << index.status().message();

  EXPECT_EQ(index->processed_translation_units, 2u);
  ASSERT_EQ(index->declarations.size(), 2u);

  bool has_a = false;
  bool has_b = false;
  for (const auto& declaration : index->declarations) {
    has_a |= (declaration.qualified_name.find("a_function") !=
              std::string::npos);
    has_b |= (declaration.qualified_name.find("b_function") !=
              std::string::npos);
  }
  EXPECT_TRUE(has_a);
  EXPECT_TRUE(has_b);
}

TEST(ProjectAstExtractorTest, ResolvesSystemHeadersWithoutExplicitSysroot) {
  auto manifest = LoadFixtureManifest("system_headers");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  ProjectAstExtractor extractor;
  auto index = extractor.ExtractProject(*manifest);
  ASSERT_TRUE(index.ok()) << index.status().message();

  EXPECT_EQ(index->processed_translation_units, 1u);
  // The system headers contribute a handful of inline functions, so don't pin
  // an exact count — assert the fixture's own function was actually extracted.
  bool has_system_header_function = false;
  for (const auto& declaration : index->declarations) {
    has_system_header_function |=
        (declaration.qualified_name.find("system_header_function") !=
         std::string::npos);
  }
  EXPECT_TRUE(has_system_header_function);
}

}  // namespace
}  // namespace veritas::frontend::clang
