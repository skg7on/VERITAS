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

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/summarydb/MetadataStore.h"

namespace {

using veritas::build::AnalysisManifest;
using veritas::build::LoadProjectManifest;
using veritas::build::ResolveProjectInput;
using veritas::summarydb::MetadataStore;

std::filesystem::path TempDbPath(const std::string& suffix) {
  return std::filesystem::temp_directory_path() /
         ("veritas_manifest_test_" + std::to_string(::getpid()) + "_" +
          suffix + ".db");
}

veritas::StatusOr<AnalysisManifest> LoadFixtureManifest(
    const std::string& fixture_name) {
  const veritas::analysis::ProjectAnalysisRequest request{
      .project_root = veritas::testing::FixtureProject(fixture_name),
      .output_root = {},
  };
  auto input = ResolveProjectInput(request);
  if (!input.ok()) {
    return input.status();
  }
  return LoadProjectManifest(*input);
}

int CountRows(const std::filesystem::path& db_path, const std::string& table) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return -1;
  }
  std::string sql = "SELECT COUNT(*) FROM " + table;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  int count = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return count;
}

class ManifestPersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    db_path_ = TempDbPath(::testing::UnitTest::GetInstance()
                              ->current_test_info()
                              ->name());
    std::filesystem::remove(db_path_);
  }

  void TearDown() override { std::filesystem::remove(db_path_); }

  std::filesystem::path db_path_;
};

}  // namespace

TEST_F(ManifestPersistenceTest, PersistsSmokeManifest) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  auto status = store.value().PutManifestContext(*manifest);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(CountRows(db_path_, "repositories"), 1);
  EXPECT_EQ(CountRows(db_path_, "revisions"), 1);
  EXPECT_EQ(CountRows(db_path_, "build_variants"), 1);
  EXPECT_EQ(CountRows(db_path_, "translation_units"), 1);
}

TEST_F(ManifestPersistenceTest, StoringSameManifestTwiceIsIdempotent) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  ASSERT_TRUE(store.value().PutManifestContext(*manifest).ok());
  ASSERT_TRUE(store.value().PutManifestContext(*manifest).ok());

  EXPECT_EQ(CountRows(db_path_, "repositories"), 1);
  EXPECT_EQ(CountRows(db_path_, "revisions"), 1);
  EXPECT_EQ(CountRows(db_path_, "build_variants"), 1);
  EXPECT_EQ(CountRows(db_path_, "translation_units"), 1);
}

TEST_F(ManifestPersistenceTest, PersistsMultiTranslationUnitManifest) {
  auto manifest = LoadFixtureManifest("multiple_tus");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  ASSERT_EQ(manifest->translation_units.size(), 2u);

  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  ASSERT_TRUE(store.value().PutManifestContext(*manifest).ok());

  EXPECT_EQ(CountRows(db_path_, "repositories"), 1);
  EXPECT_EQ(CountRows(db_path_, "revisions"), 1);
  EXPECT_EQ(CountRows(db_path_, "build_variants"), 1);
  EXPECT_EQ(CountRows(db_path_, "translation_units"), 2);
}

TEST_F(ManifestPersistenceTest, RollsBackWhenTranslationUnitFkFails) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  // Break the manifest: point the translation unit at a revision_id that
  // doesn't match the manifest's own context. With foreign keys enabled the
  // TU insert must fail, and the whole transaction (repository, revision,
  // build variant, prior TUs) must roll back.
  auto broken = *manifest;
  ASSERT_FALSE(broken.translation_units.empty());
  broken.translation_units[0].revision_id = "rev:sha256:does_not_exist";

  auto status = store.value().PutManifestContext(broken);
  EXPECT_FALSE(status.ok()) << "expected FK violation to reject the manifest";

  EXPECT_EQ(CountRows(db_path_, "repositories"), 0);
  EXPECT_EQ(CountRows(db_path_, "revisions"), 0);
  EXPECT_EQ(CountRows(db_path_, "build_variants"), 0);
  EXPECT_EQ(CountRows(db_path_, "translation_units"), 0);
}
