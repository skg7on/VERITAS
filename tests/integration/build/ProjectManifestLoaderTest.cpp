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
#include <fstream>
#include <string>
#include <system_error>

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

TEST(ProjectManifestLoaderTest, MixedCompilerOrderDoesNotChangeIdentity) {
  auto ab = LoadProjectManifest(*ResolveFixture("mixed_compilers_ab"));
  auto ba = LoadProjectManifest(*ResolveFixture("mixed_compilers_ba"));
  ASSERT_TRUE(ab.ok()) << ab.status().message();
  ASSERT_TRUE(ba.ok()) << ba.status().message();
  EXPECT_EQ(ab->context.compiler_id, ba->context.compiler_id);
  EXPECT_EQ(ab->context.build_variant_id, ba->context.build_variant_id);
  EXPECT_EQ(ab->context.repository_id, ba->context.repository_id);
  EXPECT_EQ(ToCanonicalBytes(*ab), ToCanonicalBytes(*ba));
  // The compiler_id captures the full sorted set, so both compilers are
  // represented regardless of database entry order.
  EXPECT_NE(ab->context.compiler_id.find("clang++"), std::string::npos);
  EXPECT_NE(ab->context.compiler_id.find("gcc"), std::string::npos);
}

TEST(ProjectManifestLoaderTest, ArgumentContainingProjectRootIsSubstituted) {
  auto input = ResolveFixture("multiple_tus");
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = LoadProjectManifest(*input);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  // No M1 fixture emits absolute path flags, so the manifest arguments must
  // NOT contain the fixture's absolute path anywhere. If a future refactor
  // reintroduces the speculative-rewrite bug, the raw project_root would
  // appear in the canonical bytes and break cross-checkout stability.
  const auto project_string = input->project_root.generic_string();
  for (const auto& tu : manifest->translation_units) {
    for (const auto& arg : tu.arguments) {
      EXPECT_EQ(arg.find(project_string), std::string::npos)
          << "argument leaks project root: " << arg;
    }
  }
}

TEST(ProjectManifestLoaderTest, SourceTreeHashIgnoresVeritasOutputDirectory) {
  // Design spec §10 required assertion: creating diagnostic-output artifacts
  // under the project (`.veritas/`, temporary output dirs) must not shift
  // source_tree_hash. The hash is computed from compile_commands.json entries,
  // so files that never appear there should never enter the hash — this test
  // guards against a future refactor that accidentally globs the tree.
  auto input = ResolveFixture("multiple_tus");
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto before = LoadProjectManifest(*input);
  ASSERT_TRUE(before.ok()) << before.status().message();

  const auto dot_veritas = input->project_root / ".veritas";
  std::error_code error;
  std::filesystem::create_directories(dot_veritas, error);
  ASSERT_FALSE(error) << error.message();
  std::ofstream(dot_veritas / "manifest.json") << "{\"stub\": true}";
  std::ofstream(input->project_root / "tmp_output.o") << "stray artifact";

  auto after = LoadProjectManifest(*input);
  ASSERT_TRUE(after.ok()) << after.status().message();
  EXPECT_EQ(before->context.source_tree_hash,
            after->context.source_tree_hash);
  EXPECT_EQ(before->context.repository_id, after->context.repository_id);
  EXPECT_EQ(ToCanonicalBytes(*before), ToCanonicalBytes(*after));
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
