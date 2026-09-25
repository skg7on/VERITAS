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

  // How many rows the batcher writes in one statement, and how many rows are
  // queued right now (the batcher holds flat values, not rows).
  static std::size_t BatchCapacity(const BulkInsertBatcher& batcher) {
    return batcher.max_rows_;
  }

  static std::size_t PendingRows(const BulkInsertBatcher& batcher) {
    return batcher.columns_ == 0 ? 0 : batcher.pending_.size() / batcher.columns_;
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

// A run publishes millions of rows, so nearly every row travels in a full
// batch. Every other test in this repository exercises only the short tail, so
// the full-batch statement, the drain it forces, and a tail after it are
// otherwise unproven.
TEST_F(MetadataStoreTest, BulkInsertBatchesFullBatchesAndFlushesTheTail) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE bulk_probe (a TEXT, b TEXT, c TEXT)", {})
          .ok());

  BulkInsertBatcher batcher(*store, "INSERT INTO bulk_probe (a, b, c) VALUES",
                            3);
  const std::size_t capacity = MetadataStoreTestPeer::BatchCapacity(batcher);
  ASSERT_GT(capacity, 1u);
  // Two full batches and a tail of three, so both the full-batch and the
  // short-tail statements are issued for one table.
  const std::size_t rows = capacity * 2 + 3;

  for (std::size_t i = 0; i < rows; ++i) {
    ASSERT_TRUE(batcher.Add({std::to_string(i), "b", "c"}).ok());
  }
  // Add itself writes a batch once one is full, so never more than a batch can
  // be outstanding, and the tail is all that is left.
  EXPECT_LE(MetadataStoreTestPeer::PendingRows(batcher), capacity);
  EXPECT_EQ(MetadataStoreTestPeer::PendingRows(batcher), 3u);
  ASSERT_TRUE(batcher.Flush().ok());
  EXPECT_EQ(MetadataStoreTestPeer::PendingRows(batcher), 0u);

  auto counted = store->Query("SELECT COUNT(*) FROM bulk_probe", {});
  ASSERT_TRUE(counted.ok());
  EXPECT_EQ((*counted)[0][0], std::to_string(rows));

  // Rows survive the batch boundary in the order they were queued.
  auto ordered = store->Query(
      "SELECT a FROM bulk_probe ORDER BY CAST(a AS INTEGER)", {});
  ASSERT_TRUE(ordered.ok());
  ASSERT_EQ(ordered->size(), rows);
  EXPECT_EQ((*ordered)[0][0], "0");
  EXPECT_EQ((*ordered)[capacity - 1][0], std::to_string(capacity - 1));
  EXPECT_EQ((*ordered)[capacity][0], std::to_string(capacity));
  EXPECT_EQ((*ordered)[rows - 1][0], std::to_string(rows - 1));
}

// A row whose width disagrees with the batcher's column count is rejected
// rather than bound into the wrong columns, and a rejected row does not
// disturb the queued ones.
TEST_F(MetadataStoreTest, BulkInsertRejectsARowOfTheWrongWidth) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE bulk_probe (a TEXT, b TEXT, c TEXT)", {})
          .ok());

  BulkInsertBatcher batcher(*store, "INSERT INTO bulk_probe (a, b, c) VALUES",
                            3);
  EXPECT_EQ(batcher.Add({"a", "b"}).code(), veritas::StatusCode::kInvalidArgument);
  EXPECT_EQ(batcher.Add({"a", "b", "c", "d"}).code(),
            veritas::StatusCode::kInvalidArgument);
  EXPECT_EQ(MetadataStoreTestPeer::PendingRows(batcher), 0u);

  ASSERT_TRUE(batcher.Add({"a", "b", "c"}).ok());
  ASSERT_TRUE(batcher.Flush().ok());
  auto counted = store->Query("SELECT COUNT(*) FROM bulk_probe", {});
  ASSERT_TRUE(counted.ok());
  EXPECT_EQ((*counted)[0][0], "1");
}

// Flushing with nothing queued is a successful no-op, which is what lets
// publication flush unconditionally before committing.
TEST_F(MetadataStoreTest, BulkInsertFlushOnAnEmptyBatchIsANoOp) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok());
  ASSERT_TRUE(
      store->Execute("CREATE TABLE bulk_probe (a TEXT, b TEXT, c TEXT)", {})
          .ok());
  BulkInsertBatcher batcher(*store, "INSERT INTO bulk_probe (a, b, c) VALUES",
                            3);
  EXPECT_TRUE(batcher.Flush().ok());
  EXPECT_TRUE(batcher.Flush().ok());
  auto counted = store->Query("SELECT COUNT(*) FROM bulk_probe", {});
  ASSERT_TRUE(counted.ok());
  EXPECT_EQ((*counted)[0][0], "0");
}
