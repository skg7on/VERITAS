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

#include <sqlite3.h>

namespace veritas::summarydb {

namespace {

Status ExecuteSQL(sqlite3* db, const std::string& sql) {
  char* err_msg = nullptr;
  int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err_msg);
  if (rc != SQLITE_OK) {
    std::string error = err_msg ? err_msg : "unknown error";
    sqlite3_free(err_msg);
    return Status::Internal("SQLite exec failed: " + error);
  }
  return Status::Ok();
}

Status BindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  int rc = sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite bind_text failed");
  }
  return Status::Ok();
}

Status BindInt(sqlite3_stmt* stmt, int index, int value) {
  int rc = sqlite3_bind_int(stmt, index, value);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite bind_int failed");
  }
  return Status::Ok();
}

Status StepAndFinalize(sqlite3_stmt* stmt) {
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Status::Internal("SQLite step failed");
  }
  return Status::Ok();
}

}  // namespace

MetadataStore::MetadataStore(sqlite3* db) : db_(db) {}

MetadataStore::~MetadataStore() {
  if (db_) {
    sqlite3_close(db_);
  }
}

MetadataStore::MetadataStore(MetadataStore&& other) noexcept : db_(other.db_) {
  other.db_ = nullptr;
}

MetadataStore& MetadataStore::operator=(MetadataStore&& other) noexcept {
  if (this != &other) {
    if (db_) {
      sqlite3_close(db_);
    }
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

StatusOr<MetadataStore> MetadataStore::Open(
    const std::filesystem::path& db_path) {
  sqlite3* db = nullptr;
  int rc = sqlite3_open(db_path.string().c_str(), &db);
  if (rc != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return Status::Internal("Failed to open SQLite database");
  }
  return MetadataStore(db);
}

Status MetadataStore::ApplySchema() {
  // Load schema from embedded resource or fallback to file.
  // For M2, we embed the schema as a compile-time string to avoid runtime
  // file-path dependencies. Since the schema is short, we inline it here.
  const char* schema_sql = R"(
CREATE TABLE IF NOT EXISTS schema_version (
  version INTEGER PRIMARY KEY
);
INSERT OR IGNORE INTO schema_version (version) VALUES (1);

CREATE TABLE IF NOT EXISTS repositories (
  repository_id TEXT PRIMARY KEY NOT NULL,
  vcs_kind TEXT NOT NULL,
  vcs_revision TEXT NOT NULL,
  source_tree_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS revisions (
  revision_id TEXT PRIMARY KEY NOT NULL,
  repository_id TEXT NOT NULL,
  vcs_revision TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (repository_id) REFERENCES repositories(repository_id)
);
CREATE INDEX IF NOT EXISTS idx_revisions_repository ON revisions(repository_id);

CREATE TABLE IF NOT EXISTS build_variants (
  build_variant_id TEXT PRIMARY KEY NOT NULL,
  target_triple TEXT NOT NULL,
  compiler_id TEXT NOT NULL,
  compiler_version TEXT NOT NULL,
  compile_options_hash TEXT NOT NULL,
  macro_set_hash TEXT NOT NULL,
  include_closure_hash TEXT NOT NULL,
  type_layout_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS translation_units (
  translation_unit_id TEXT PRIMARY KEY NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  source_path_root_kind INTEGER NOT NULL,
  source_path_root_id TEXT NOT NULL,
  source_path_relative TEXT NOT NULL,
  working_dir_root_kind INTEGER NOT NULL,
  working_dir_root_id TEXT NOT NULL,
  working_dir_relative TEXT NOT NULL,
  command_hash TEXT NOT NULL,
  preprocessor_hash TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (revision_id) REFERENCES revisions(revision_id),
  FOREIGN KEY (build_variant_id) REFERENCES build_variants(build_variant_id)
);
CREATE INDEX IF NOT EXISTS idx_translation_units_revision ON translation_units(revision_id);
CREATE INDEX IF NOT EXISTS idx_translation_units_build_variant ON translation_units(build_variant_id);

CREATE TABLE IF NOT EXISTS analyzer_runs (
  analyzer_run_id INTEGER PRIMARY KEY AUTOINCREMENT,
  analyzer_name TEXT NOT NULL,
  analyzer_version TEXT NOT NULL,
  schema_version INTEGER NOT NULL,
  config_hash TEXT NOT NULL,
  trust_level TEXT NOT NULL,
  started_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS analysis_configurations (
  analyzer_run_id INTEGER NOT NULL,
  config_key TEXT NOT NULL,
  config_value TEXT NOT NULL,
  PRIMARY KEY (analyzer_run_id, config_key),
  FOREIGN KEY (analyzer_run_id) REFERENCES analyzer_runs(analyzer_run_id)
);

CREATE TABLE IF NOT EXISTS source_anchors (
  anchor_id INTEGER PRIMARY KEY AUTOINCREMENT,
  translation_unit_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  start_line INTEGER NOT NULL,
  start_column INTEGER NOT NULL,
  end_line INTEGER NOT NULL,
  end_column INTEGER NOT NULL,
  FOREIGN KEY (translation_unit_id) REFERENCES translation_units(translation_unit_id)
);
CREATE INDEX IF NOT EXISTS idx_source_anchors_translation_unit ON source_anchors(translation_unit_id);

CREATE TABLE IF NOT EXISTS function_symbols (
  function_symbol_id TEXT PRIMARY KEY NOT NULL,
  translation_unit_id TEXT NOT NULL,
  mangled_name TEXT NOT NULL,
  canonical_signature TEXT NOT NULL,
  linkage_kind TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (translation_unit_id) REFERENCES translation_units(translation_unit_id)
);
CREATE INDEX IF NOT EXISTS idx_function_symbols_translation_unit ON function_symbols(translation_unit_id);

CREATE TABLE IF NOT EXISTS function_variants (
  function_variant_id TEXT PRIMARY KEY NOT NULL,
  function_symbol_id TEXT NOT NULL,
  template_args TEXT,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (function_symbol_id) REFERENCES function_symbols(function_symbol_id)
);
CREATE INDEX IF NOT EXISTS idx_function_variants_symbol ON function_variants(function_symbol_id);

CREATE TABLE IF NOT EXISTS function_bodies (
  function_body_id TEXT PRIMARY KEY NOT NULL,
  function_variant_id TEXT NOT NULL,
  semantic_body_hash TEXT NOT NULL,
  source_anchor_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  FOREIGN KEY (function_variant_id) REFERENCES function_variants(function_variant_id),
  FOREIGN KEY (source_anchor_id) REFERENCES source_anchors(anchor_id)
);
CREATE INDEX IF NOT EXISTS idx_function_bodies_variant ON function_bodies(function_variant_id);
)";

  return ExecuteSQL(db_, schema_sql);
}

Status MetadataStore::PutRepository(const RepositoryRow& row) {
  const char* sql =
      "INSERT OR IGNORE INTO repositories (repository_id, vcs_kind, "
      "vcs_revision, source_tree_hash) VALUES (?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite prepare failed");
  }

  auto status = BindText(stmt, 1, row.repository_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 2, row.vcs_kind);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 3, row.vcs_revision);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 4, row.source_tree_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }

  return StepAndFinalize(stmt);
}

Status MetadataStore::PutRevision(const RevisionRow& row) {
  const char* sql =
      "INSERT OR IGNORE INTO revisions (revision_id, repository_id, "
      "vcs_revision) VALUES (?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite prepare failed");
  }

  auto status = BindText(stmt, 1, row.revision_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 2, row.repository_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 3, row.vcs_revision);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }

  return StepAndFinalize(stmt);
}

Status MetadataStore::PutBuildVariant(const BuildVariantRow& row) {
  const char* sql =
      "INSERT OR IGNORE INTO build_variants (build_variant_id, target_triple, "
      "compiler_id, compiler_version, compile_options_hash, macro_set_hash, "
      "include_closure_hash, type_layout_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite prepare failed");
  }

  auto status = BindText(stmt, 1, row.build_variant_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 2, row.target_triple);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 3, row.compiler_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 4, row.compiler_version);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 5, row.compile_options_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 6, row.macro_set_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 7, row.include_closure_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 8, row.type_layout_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }

  return StepAndFinalize(stmt);
}

Status MetadataStore::PutTranslationUnit(const TranslationUnitRow& row) {
  const char* sql =
      "INSERT OR IGNORE INTO translation_units (translation_unit_id, "
      "revision_id, build_variant_id, source_path_root_kind, "
      "source_path_root_id, source_path_relative, working_dir_root_kind, "
      "working_dir_root_id, working_dir_relative, command_hash, "
      "preprocessor_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite prepare failed");
  }

  auto status = BindText(stmt, 1, row.translation_unit_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 2, row.revision_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 3, row.build_variant_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindInt(stmt, 4, row.source_path_root_kind);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 5, row.source_path_root_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 6, row.source_path_relative);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindInt(stmt, 7, row.working_dir_root_kind);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 8, row.working_dir_root_id);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 9, row.working_dir_relative);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 10, row.command_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 11, row.preprocessor_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }

  return StepAndFinalize(stmt);
}

StatusOr<int64_t> MetadataStore::PutAnalyzerRun(const AnalyzerRunRow& row) {
  const char* sql =
      "INSERT INTO analyzer_runs (analyzer_name, analyzer_version, "
      "schema_version, config_hash, trust_level) VALUES (?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return Status::Internal("SQLite prepare failed");
  }

  auto status = BindText(stmt, 1, row.analyzer_name);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 2, row.analyzer_version);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindInt(stmt, 3, row.schema_version);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 4, row.config_hash);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }
  status = BindText(stmt, 5, row.trust_level);
  if (!status.ok()) {
    sqlite3_finalize(stmt);
    return status;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Status::Internal("SQLite step failed");
  }

  return sqlite3_last_insert_rowid(db_);
}

}  // namespace veritas::summarydb
