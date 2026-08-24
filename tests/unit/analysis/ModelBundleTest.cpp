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

#include "veritas/analysis/semantic/ModelBundle.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "ProjectFixture.h"

namespace veritas::analysis::semantic {
namespace {

namespace fs = std::filesystem;

fs::path ModelPath() {
  return testing::TestSourceRoot().parent_path() / "logic" / "models" /
         "models.v1.tsv";
}

fs::path ManifestPath() {
  return testing::TestSourceRoot().parent_path() / "logic" / "models" /
         "models.v1.manifest";
}

// Write a temporary bundle (rows + manifest) into a fresh directory.
struct TempBundle {
  fs::path directory;
  fs::path rows;
  fs::path manifest;
};

TempBundle WriteTempBundle(std::string_view version, std::string_view content) {
  static int counter = 0;
  TempBundle bundle;
  bundle.directory =
      fs::temp_directory_path() /
      ("veritas_model_bundle_test_" + std::to_string(::getpid()) + "_" +
       std::to_string(counter++));
  fs::create_directories(bundle.directory);
  bundle.rows = bundle.directory / "models.tsv";
  bundle.manifest = bundle.directory / "models.manifest";
  {
    std::ofstream out(bundle.rows, std::ios::binary);
    out << content;
  }
  {
    std::ofstream out(bundle.manifest, std::ios::binary);
    out << "model_bundle_version=" << version << '\n';
  }
  return bundle;
}

TEST(ModelBundleTest, VersionAndContentDetermineBundleHash) {
  auto bundle = ModelBundle::Load(ModelPath(), ManifestPath());
  ASSERT_TRUE(bundle.ok());
  EXPECT_EQ(bundle->version(), "models.v1");
  EXPECT_EQ(bundle->hash().size(), 64u);
  EXPECT_FALSE(bundle->Lookup("malloc").empty());
}

TEST(ModelBundleTest, LookupReturnsEmptyForUnknownSymbol) {
  auto bundle = ModelBundle::Load(ModelPath(), ManifestPath());
  ASSERT_TRUE(bundle.ok());
  EXPECT_TRUE(bundle->Lookup("definitely_not_modeled").empty());
}

TEST(ModelBundleTest, LookupGroupsAllModelsForASymbol) {
  auto bundle = ModelBundle::Load(ModelPath(), ManifestPath());
  ASSERT_TRUE(bundle.ok());
  const auto memcpy_models = bundle->Lookup("memcpy");
  ASSERT_EQ(memcpy_models.size(), 2u);
  EXPECT_EQ(memcpy_models[0].symbol, "memcpy");
  EXPECT_EQ(memcpy_models[1].symbol, "memcpy");
}

TEST(ModelBundleTest, RejectsDuplicateModelIds) {
  auto bundle = WriteTempBundle(
      "models.v1",
      "a.memcpy.read\tmemcpy\tread\tsource\tmay\n"
      "a.memcpy.read\tmemcpy\twrite\tdestination\tmay\n");
  auto loaded = ModelBundle::Load(bundle.rows, bundle.manifest);
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status().code(), StatusCode::kInvalidArgument);
}

TEST(ModelBundleTest, RejectsUnknownEffectKind) {
  auto bundle = WriteTempBundle("models.v1",
                                "malloc.alloc\tmalloc\tfrobnicate\treturn\tmay\n");
  auto loaded = ModelBundle::Load(bundle.rows, bundle.manifest);
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status().code(), StatusCode::kInvalidArgument);
}

TEST(ModelBundleTest, RejectsControlCharacters) {
  auto bundle = WriteTempBundle("models.v1",
                                "malloc.alloc\tmalloc\tallocate\tret\x01urn\tmay\n");
  auto loaded = ModelBundle::Load(bundle.rows, bundle.manifest);
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status().code(), StatusCode::kInvalidArgument);
}

TEST(ModelBundleTest, RejectsUnsortedRows) {
  auto bundle = WriteTempBundle("models.v1",
                                "malloc.alloc\tmalloc\tallocate\treturn\tmay\n"
                                "free.dealloc\tfree\tdeallocate\targument\tmay\n");
  auto loaded = ModelBundle::Load(bundle.rows, bundle.manifest);
  ASSERT_FALSE(loaded.ok());
  EXPECT_EQ(loaded.status().code(), StatusCode::kInvalidArgument);
}

TEST(ModelBundleTest, HashChangesWithContent) {
  auto first = WriteTempBundle("models.v1",
                               "malloc.alloc\tmalloc\tallocate\treturn\tmay\n");
  auto second = WriteTempBundle("models.v1",
                                "malloc.alloc\tmalloc\tallocate\treturn\tmust\n");
  auto first_bundle = ModelBundle::Load(first.rows, first.manifest);
  auto second_bundle = ModelBundle::Load(second.rows, second.manifest);
  ASSERT_TRUE(first_bundle.ok());
  ASSERT_TRUE(second_bundle.ok());
  EXPECT_NE(first_bundle->hash(), second_bundle->hash());
}

}  // namespace
}  // namespace veritas::analysis::semantic
