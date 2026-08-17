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

#include "veritas/build/AnalysisManifest.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace veritas::build {
namespace {

TranslationUnitCommand MakeTu(std::string source, std::string command_hash) {
  TranslationUnitCommand unit;
  unit.translation_unit_id = "tu:sha256:" + command_hash;
  unit.revision_id = "rev:sha256:constant";
  unit.build_variant_id = "bv:sha256:constant";
  unit.source_path =
      TaggedPath{PathRootKind::kRepository, "repository", std::move(source)};
  unit.working_directory =
      TaggedPath{PathRootKind::kRepository, "repository", "."};
  unit.arguments = {"clang++", "-std=c++20", "-c"};
  unit.command_hash = std::move(command_hash);
  unit.preprocessor_hash = "pp:sha256:constant";
  return unit;
}

AnalysisManifest MakeManifest(std::vector<TranslationUnitCommand> units) {
  AnalysisManifest manifest;
  manifest.context.repository_id = "repo:sha256:constant";
  manifest.context.revision_id = "rev:sha256:constant";
  manifest.context.build_variant_id = "bv:sha256:constant";
  manifest.context.source_tree_hash = "tree:sha256:constant";
  manifest.context.compilation_database_hash = "cdb:sha256:constant";
  manifest.context.target_triple = "arm64-apple-darwin";
  manifest.context.compiler_id = "clang";
  manifest.context.compiler_version = "22.0.0";
  manifest.context.compile_options_hash = "opts:sha256:constant";
  manifest.context.macro_set_hash = "macros:sha256:constant";
  manifest.context.include_closure_hash = "inc:sha256:constant";
  manifest.context.type_layout_hash = "types:sha256:constant";
  manifest.translation_units = std::move(units);
  return manifest;
}

TEST(AnalysisManifestTest, TranslationUnitOrderDoesNotChangeCanonicalBytes) {
  auto first = MakeManifest({MakeTu("b.cpp", "hash-b"),
                             MakeTu("a.cpp", "hash-a")});
  auto second = MakeManifest({MakeTu("a.cpp", "hash-a"),
                              MakeTu("b.cpp", "hash-b")});
  EXPECT_EQ(ToCanonicalBytes(first), ToCanonicalBytes(second));
  EXPECT_EQ(ToDiagnosticJson(first), ToDiagnosticJson(second));
}

TEST(AnalysisManifestTest, OutputRootIsNotSerializedAsSemanticInput) {
  auto first = MakeManifest({MakeTu("a.cpp", "hash-a")});
  first.context.project_root = "/tmp/checkout-a";
  auto second = MakeManifest({MakeTu("a.cpp", "hash-a")});
  second.context.project_root = "/tmp/checkout-b";
  EXPECT_EQ(ToCanonicalBytes(first), ToCanonicalBytes(second));
  EXPECT_EQ(ToDiagnosticJson(first), ToDiagnosticJson(second));
}

TEST(AnalysisManifestTest, DifferentSourceTreeHashChangesCanonicalBytes) {
  auto first = MakeManifest({MakeTu("a.cpp", "hash-a")});
  auto second = MakeManifest({MakeTu("a.cpp", "hash-a")});
  second.context.source_tree_hash = "tree:sha256:different";
  EXPECT_NE(ToCanonicalBytes(first), ToCanonicalBytes(second));
}

TEST(AnalysisManifestTest, DifferentCommandHashChangesCanonicalBytes) {
  auto first = MakeManifest({MakeTu("a.cpp", "hash-a")});
  auto second = MakeManifest({MakeTu("a.cpp", "hash-different")});
  EXPECT_NE(ToCanonicalBytes(first), ToCanonicalBytes(second));
}

TEST(AnalysisManifestTest, DifferentArgumentOrderChangesCanonicalBytes) {
  auto first = MakeManifest({MakeTu("a.cpp", "hash-a")});
  auto second = MakeManifest({MakeTu("a.cpp", "hash-a")});
  second.translation_units[0].arguments = {"-c", "clang++", "-std=c++20"};
  EXPECT_NE(ToCanonicalBytes(first), ToCanonicalBytes(second));
}

TEST(AnalysisManifestTest, EmptyManifestIsStable) {
  AnalysisManifest empty;
  const auto bytes = ToCanonicalBytes(empty);
  EXPECT_EQ(bytes, ToCanonicalBytes(empty));
  const auto json = ToDiagnosticJson(empty);
  EXPECT_EQ(json, ToDiagnosticJson(empty));
  EXPECT_FALSE(bytes.empty());
  EXPECT_FALSE(json.empty());
}

TEST(AnalysisManifestTest, DiagnosticJsonKeysAreSortedAndStable) {
  auto manifest = MakeManifest({MakeTu("a.cpp", "hash-a"),
                                MakeTu("b.cpp", "hash-b")});
  const auto json = ToDiagnosticJson(manifest);
  const auto compiler_id_pos = json.find("\"compiler_id\"");
  const auto compiler_version_pos = json.find("\"compiler_version\"");
  ASSERT_NE(compiler_id_pos, std::string::npos);
  ASSERT_NE(compiler_version_pos, std::string::npos);
  EXPECT_LT(compiler_id_pos, compiler_version_pos);
}

}  // namespace
}  // namespace veritas::build
