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

// MetadataStore.h — SQLite-backed metadata persistence for M2.
//
// Stores repositories, revisions, build variants, translation units, analyzer
// runs, and function identity tables. Inserts are idempotent; duplicate
// inserts with the same primary key are no-ops. Public APIs return Status,
// never SQLite exceptions.

#ifndef VERITAS_SUMMARYDB_METADATASTORE_H_
#define VERITAS_SUMMARYDB_METADATASTORE_H_

#include <filesystem>
#include <string>

#include "veritas/core/Status.h"

// Forward-declare sqlite3 to avoid pulling sqlite3.h into public headers.
struct sqlite3;

namespace veritas::build {
struct AnalysisManifest;
}

namespace veritas::summarydb {

struct RepositoryRow {
  std::string repository_id;
  std::string vcs_kind;
  std::string vcs_revision;
  std::string source_tree_hash;
};

struct RevisionRow {
  std::string revision_id;
  std::string repository_id;
  std::string vcs_revision;
};

struct BuildVariantRow {
  std::string build_variant_id;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
  std::string compile_options_hash;
  std::string macro_set_hash;
  std::string include_closure_hash;
  std::string type_layout_hash;
};

struct TranslationUnitRow {
  std::string translation_unit_id;
  std::string revision_id;
  std::string build_variant_id;
  int source_path_root_kind;
  std::string source_path_root_id;
  std::string source_path_relative;
  int working_dir_root_kind;
  std::string working_dir_root_id;
  std::string working_dir_relative;
  std::string command_hash;
  std::string preprocessor_hash;
};

struct AnalyzerRunRow {
  std::string analyzer_name;
  std::string analyzer_version;
  int schema_version;
  std::string config_hash;
  std::string trust_level;
};

class MetadataStore {
 public:
  ~MetadataStore();

  // Open or create a metadata database at db_path. Returns InvalidArgument
  // if the path is invalid, Internal if SQLite fails.
  static StatusOr<MetadataStore> Open(const std::filesystem::path& db_path);

  // Apply the V1 schema to a fresh database. Idempotent; safe to call on an
  // already-initialized database. Returns FailedPrecondition if the database
  // has a schema version newer than V1.
  Status ApplySchema();

  // Insert a repository row. Idempotent; duplicate repository_id is a no-op.
  Status PutRepository(const RepositoryRow& row);

  // Insert a revision row. Idempotent.
  Status PutRevision(const RevisionRow& row);

  // Insert a build variant row. Idempotent.
  Status PutBuildVariant(const BuildVariantRow& row);

  // Insert a translation unit row. Idempotent.
  Status PutTranslationUnit(const TranslationUnitRow& row);

  // Insert an analyzer run row. Returns the auto-generated analyzer_run_id.
  StatusOr<int64_t> PutAnalyzerRun(const AnalyzerRunRow& row);

  // Persist an M1 analysis manifest in a single transaction. Inserts the
  // repository, revision, build variant, and every translation unit atomically.
  // If any row fails, the entire transaction is rolled back and no partial
  // manifest context is committed. Idempotent: storing the same manifest twice
  // produces one logical analysis context.
  Status PutManifestContext(const veritas::build::AnalysisManifest& manifest);

  // M3: Execute a SQL statement with parameters. Used for summary metadata.
  Status Execute(const std::string& sql,
                 const std::vector<std::string>& params);

  // M3: Query with parameters, returning rows as vectors of strings.
  StatusOr<std::vector<std::vector<std::string>>> Query(
      const std::string& sql, const std::vector<std::string>& params);

  // M3: Begin a transaction.
  Status BeginTransaction();

  // M3: Commit a transaction.
  Status CommitTransaction();

  // M3: Rollback a transaction.
  Status RollbackTransaction();

  MetadataStore(MetadataStore&&) noexcept;
  MetadataStore& operator=(MetadataStore&&) noexcept;

 private:
  explicit MetadataStore(sqlite3* db);

  sqlite3* db_;

  MetadataStore(const MetadataStore&) = delete;
  MetadataStore& operator=(const MetadataStore&) = delete;
};

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_METADATASTORE_H_
