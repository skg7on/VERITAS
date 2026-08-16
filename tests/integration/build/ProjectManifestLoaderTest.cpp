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

#include "veritas/build/ProjectManifestLoader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/core/Status.h"

namespace veritas::build {
namespace {

StatusOr<ProjectInput> ResolveFixture(std::string_view name) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = testing::FixtureProject(name),
      .output_root = {},
  };
  return ResolveProjectInput(request);
}

TEST(ProjectManifestLoaderTest, LoadsSmokeProject) {
  auto input = ResolveFixture("smoke");
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = LoadProjectManifest(*input);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  ASSERT_EQ(manifest->translation_units.size(), 1u);
  const auto& tu = manifest->translation_units[0];
  EXPECT_EQ(tu.source_path.root_kind, PathRootKind::kRepository);
  EXPECT_EQ(tu.source_path.root_id, "repository");
  EXPECT_EQ(tu.source_path.relative_path.generic_string(), "smoke.cpp");
  EXPECT_FALSE(tu.command_hash.empty());
  EXPECT_FALSE(tu.translation_unit_id.empty());
  EXPECT_TRUE(tu.translation_unit_id.starts_with("tu:sha256:"));

  EXPECT_TRUE(manifest->context.repository_id.starts_with("repo:sha256:"));
  EXPECT_TRUE(manifest->context.revision_id.starts_with("rev:sha256:"));
  EXPECT_TRUE(manifest->context.build_variant_id.starts_with("bv:sha256:"));
  EXPECT_FALSE(manifest->context.source_tree_hash.empty());
  EXPECT_FALSE(manifest->context.compilation_database_hash.empty());
  EXPECT_EQ(manifest->context.compiler_id, "clang++");
}

TEST(ProjectManifestLoaderTest, LoadsEveryTranslationUnitDeterministically) {
  auto input_first = ResolveFixture("multiple_tus");
  ASSERT_TRUE(input_first.ok()) << input_first.status().message();
  auto input_second = ResolveFixture("multiple_tus");
  ASSERT_TRUE(input_second.ok()) << input_second.status().message();

  auto first = LoadProjectManifest(*input_first);
  ASSERT_TRUE(first.ok()) << first.status().message();
  auto second = LoadProjectManifest(*input_second);
  ASSERT_TRUE(second.ok()) << second.status().message();

  ASSERT_EQ(first->translation_units.size(), 2u);
  EXPECT_EQ(ToCanonicalBytes(*first), ToCanonicalBytes(*second));
  EXPECT_EQ(ToDiagnosticJson(*first), ToDiagnosticJson(*second));

  EXPECT_EQ(first->translation_units[0].source_path.relative_path, "a.cpp");
  EXPECT_EQ(first->translation_units[1].source_path.relative_path, "b.cpp");
}

TEST(ProjectManifestLoaderTest, CanonicalBytesStableAcrossCheckoutRoots) {
  auto first = LoadProjectManifest(*ResolveFixture("multiple_tus"));
  auto second = LoadProjectManifest(*ResolveFixture("multiple_tus"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(ToCanonicalBytes(*first), ToCanonicalBytes(*second));
  EXPECT_EQ(first->context.source_tree_hash,
            second->context.source_tree_hash);
  EXPECT_EQ(first->context.compilation_database_hash,
            second->context.compilation_database_hash);
  EXPECT_EQ(first->context.repository_id, second->context.repository_id);
}

TEST(ProjectManifestLoaderTest, MissingTranslationUnitFailsWholeLoad) {
  auto input = ResolveFixture("missing_source");
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = LoadProjectManifest(*input);
  ASSERT_FALSE(manifest.ok());
  EXPECT_EQ(manifest.status().code(), StatusCode::kFailedPrecondition);
}

}  // namespace
}  // namespace veritas::build
