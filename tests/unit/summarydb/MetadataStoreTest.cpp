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

#include "veritas/summarydb/MetadataStore.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace veritas::summarydb {

class MetadataStoreTestPeer {
 public:
  static std::size_t CachedStatementCount(const MetadataStore& store) {
    return store.statement_cache_.size();
  }

  static std::size_t CachedStatementCountForSql(const MetadataStore& store,
                                                const std::string& sql) {
    return store.statement_cache_.count(sql);
  }
};

}  // namespace veritas::summarydb

using namespace veritas::summarydb;

namespace {

std::filesystem::path TempDbPath() {
  return std::filesystem::temp_directory_path() /
         ("veritas_metadata_test_" + std::to_string(::getpid()) + ".db");
}

class MetadataStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ = TempDbPath();
    std::filesystem::remove(db_path_);
  }

  void TearDown() override { std::filesystem::remove(db_path_); }

  std::filesystem::path db_path_;
};

} // namespace

TEST_F(MetadataStoreTest, AppliesSchemaToFreshDatabase) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  EXPECT_TRUE(store.value().ApplySchema().ok());
}

TEST_F(MetadataStoreTest, ApplySchemaTwiceIsIdempotent) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  EXPECT_TRUE(store.value().ApplySchema().ok());
  EXPECT_TRUE(store.value().ApplySchema().ok());
}

TEST_F(MetadataStoreTest, PutRepositorySucceeds) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  RepositoryRow row;
  row.repository_id = "repo:sha256:abc123";
  row.vcs_kind = "git";
  row.vcs_revision = "main";
  row.source_tree_hash = "hash123";

  EXPECT_TRUE(store.value().PutRepository(row).ok());
}

TEST_F(MetadataStoreTest, DuplicateRepositoryInsertIsIdempotent) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  RepositoryRow row;
  row.repository_id = "repo:sha256:abc123";
  row.vcs_kind = "git";
  row.vcs_revision = "main";
  row.source_tree_hash = "hash123";

  EXPECT_TRUE(store.value().PutRepository(row).ok());
  EXPECT_TRUE(store.value().PutRepository(row).ok());
}

TEST_F(MetadataStoreTest, PutRevisionSucceeds) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  RepositoryRow repo;
  repo.repository_id = "repo:sha256:abc123";
  repo.vcs_kind = "git";
  repo.vcs_revision = "main";
  repo.source_tree_hash = "hash123";
  ASSERT_TRUE(store.value().PutRepository(repo).ok());

  RevisionRow row;
  row.revision_id = "rev:sha256:def456";
  row.repository_id = "repo:sha256:abc123";
  row.vcs_revision = "abc123def456";

  EXPECT_TRUE(store.value().PutRevision(row).ok());
}

TEST_F(MetadataStoreTest, DuplicateRevisionInsertIsIdempotent) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  RepositoryRow repo;
  repo.repository_id = "repo:sha256:abc123";
  repo.vcs_kind = "git";
  repo.vcs_revision = "main";
  repo.source_tree_hash = "hash123";
  ASSERT_TRUE(store.value().PutRepository(repo).ok());

  RevisionRow row;
  row.revision_id = "rev:sha256:def456";
  row.repository_id = "repo:sha256:abc123";
  row.vcs_revision = "abc123def456";

  EXPECT_TRUE(store.value().PutRevision(row).ok());
  EXPECT_TRUE(store.value().PutRevision(row).ok());
}

TEST_F(MetadataStoreTest, PutBuildVariantSucceeds) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  BuildVariantRow row;
  row.build_variant_id = "build:sha256:variant123";
  row.target_triple = "x86_64-linux-gnu";
  row.compiler_id = "clang";
  row.compiler_version = "24.0.0";
  row.compile_options_hash = "opts123";
  row.macro_set_hash = "macros123";
  row.include_closure_hash = "includes123";
  row.type_layout_hash = "layout123";

  EXPECT_TRUE(store.value().PutBuildVariant(row).ok());
}

TEST_F(MetadataStoreTest, PutTranslationUnitSucceeds) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  RepositoryRow repo;
  repo.repository_id = "repo:sha256:abc123";
  repo.vcs_kind = "git";
  repo.vcs_revision = "main";
  repo.source_tree_hash = "hash123";
  ASSERT_TRUE(store.value().PutRepository(repo).ok());

  RevisionRow rev;
  rev.revision_id = "rev:sha256:def456";
  rev.repository_id = "repo:sha256:abc123";
  rev.vcs_revision = "abc123def456";
  ASSERT_TRUE(store.value().PutRevision(rev).ok());

  BuildVariantRow build;
  build.build_variant_id = "build:sha256:variant123";
  build.target_triple = "x86_64-linux-gnu";
  build.compiler_id = "clang";
  build.compiler_version = "24.0.0";
  build.compile_options_hash = "opts123";
  build.macro_set_hash = "macros123";
  build.include_closure_hash = "includes123";
  build.type_layout_hash = "layout123";
  ASSERT_TRUE(store.value().PutBuildVariant(build).ok());

  TranslationUnitRow row;
  row.translation_unit_id = "tu:sha256:tu123";
  row.revision_id = "rev:sha256:def456";
  row.build_variant_id = "build:sha256:variant123";
  row.source_path_root_kind = 0;
  row.source_path_root_id = "root1";
  row.source_path_relative = "src/main.cpp";
  row.working_dir_root_kind = 0;
  row.working_dir_root_id = "root1";
  row.working_dir_relative = "build";
  row.command_hash = "cmd123";
  row.preprocessor_hash = "pp123";

  EXPECT_TRUE(store.value().PutTranslationUnit(row).ok());
}

TEST_F(MetadataStoreTest, PutAnalyzerRunReturnsId) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(store.value().ApplySchema().ok());

  AnalyzerRunRow row;
  row.analyzer_name = "veritas-analyzer";
  row.analyzer_version = "1.0.0";
  row.schema_version = 1;
  row.config_hash = "config123";
  row.trust_level = "verified";

  auto result = store.value().PutAnalyzerRun(row);
  ASSERT_TRUE(result.ok());
  EXPECT_GT(result.value(), 0);
}

TEST_F(MetadataStoreTest,
       ExecuteCachesExactSqlAndRecoversAfterConstraintFailure) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok()) << store.status().message();

  const std::string create_sql =
      "CREATE TABLE cached_values(value TEXT PRIMARY KEY)";
  const std::string insert_sql =
      "INSERT INTO cached_values(value) VALUES(?)";
  const std::string distinct_sql =
      "DELETE FROM cached_values WHERE value = ?";

  ASSERT_TRUE(store->Execute(create_sql, {}).ok());
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*store), 1u);

  ASSERT_TRUE(store->Execute(insert_sql, {"alpha"}).ok());
  ASSERT_TRUE(store->Execute(insert_sql, {"beta"}).ok());
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCountForSql(*store,
                                                              insert_sql),
            1u);
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*store), 2u);

  EXPECT_FALSE(store->Execute(insert_sql, {"alpha"}).ok());
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*store), 2u);
  ASSERT_TRUE(store->Execute(insert_sql, {"gamma"}).ok());

  ASSERT_TRUE(store->Execute(distinct_sql, {"missing"}).ok());
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*store), 3u);
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCountForSql(*store,
                                                              distinct_sql),
            1u);

  auto rows = store->Query(
      "SELECT value FROM cached_values ORDER BY value", {});
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_EQ(*rows, (std::vector<std::vector<std::string>>{
                       {"alpha"}, {"beta"}, {"gamma"}}));
}

TEST_F(MetadataStoreTest, MoveTransfersCachedStatementsAndEmptiesSource) {
  auto source = MetadataStore::Open(db_path_);
  ASSERT_TRUE(source.ok()) << source.status().message();
  const std::string insert_sql =
      "INSERT INTO cached_values(value) VALUES(?)";
  ASSERT_TRUE(
      source->Execute("CREATE TABLE cached_values(value TEXT PRIMARY KEY)", {})
          .ok());
  ASSERT_TRUE(source->Execute(insert_sql, {"alpha"}).ok());

  MetadataStore moved(std::move(*source));
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*source), 0u);
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(moved), 2u);
  ASSERT_TRUE(moved.Execute(insert_sql, {"beta"}).ok());
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(moved), 2u);

  const std::filesystem::path replacement_path =
      db_path_.string() + ".replacement";
  std::filesystem::remove(replacement_path);
  auto replacement = MetadataStore::Open(replacement_path);
  ASSERT_TRUE(replacement.ok()) << replacement.status().message();
  ASSERT_TRUE(replacement->Execute("CREATE TABLE discarded(value TEXT)", {})
                  .ok());

  *replacement = std::move(moved);
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(moved), 0u);
  EXPECT_EQ(MetadataStoreTestPeer::CachedStatementCount(*replacement), 2u);
  ASSERT_TRUE(replacement->Execute(insert_sql, {"gamma"}).ok());
  auto rows = replacement->Query(
      "SELECT value FROM cached_values ORDER BY value", {});
  ASSERT_TRUE(rows.ok()) << rows.status().message();
  EXPECT_EQ(*rows, (std::vector<std::vector<std::string>>{
                       {"alpha"}, {"beta"}, {"gamma"}}));

  std::filesystem::remove(replacement_path);
}

TEST_F(MetadataStoreTest, FailedCommitKeepsTransactionActiveUntilRollback) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE parent(id TEXT PRIMARY KEY)", {}).ok());
  ASSERT_TRUE(
      store
          ->Execute("CREATE TABLE child(parent_id TEXT, FOREIGN KEY(parent_id) "
                    "REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)",
                    {})
          .ok());
  ASSERT_TRUE(store->BeginTransaction().ok());
  ASSERT_TRUE(
      store->Execute("INSERT INTO child(parent_id) VALUES('missing')", {})
          .ok());

  EXPECT_FALSE(store->CommitTransaction().ok());
  EXPECT_EQ(store->BeginTransaction().code(),
            veritas::StatusCode::kFailedPrecondition);
  EXPECT_TRUE(store->RollbackTransaction().ok());
  EXPECT_TRUE(store->BeginTransaction().ok());
  EXPECT_TRUE(store->RollbackTransaction().ok());
}

TEST_F(MetadataStoreTest,
       FailedCommitAfterSQLiteAutoRollbackResynchronizesTransactionState) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE unique_values(value TEXT PRIMARY KEY)", {})
          .ok());
  ASSERT_TRUE(store->BeginTransaction().ok());
  ASSERT_TRUE(
      store->Execute("INSERT INTO unique_values(value) VALUES('duplicate')", {})
          .ok());
  ASSERT_FALSE(store
                   ->Execute("INSERT OR ROLLBACK INTO unique_values(value) "
                             "VALUES('duplicate')",
                             {})
                   .ok());

  EXPECT_FALSE(store->CommitTransaction().ok());
  EXPECT_TRUE(store->BeginTransaction().ok());
  EXPECT_TRUE(store->RollbackTransaction().ok());
}

TEST_F(MetadataStoreTest,
       FailedRollbackAfterSQLiteAutoRollbackResynchronizesTransactionState) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE unique_values(value TEXT PRIMARY KEY)", {})
          .ok());
  ASSERT_TRUE(store->BeginTransaction().ok());
  ASSERT_TRUE(
      store->Execute("INSERT INTO unique_values(value) VALUES('duplicate')", {})
          .ok());
  ASSERT_FALSE(store
                   ->Execute("INSERT OR ROLLBACK INTO unique_values(value) "
                             "VALUES('duplicate')",
                             {})
                   .ok());

  EXPECT_FALSE(store->RollbackTransaction().ok());
  EXPECT_TRUE(store->BeginTransaction().ok());
  EXPECT_TRUE(store->RollbackTransaction().ok());
}
