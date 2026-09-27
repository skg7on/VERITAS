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

#include "veritas/summarydb/StoreEquivalence.h"

#include <algorithm>
#include <span>
#include <string_view>

#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

// The determined projection. Entries are recorded from
// docs/specs/veritas-build-analyze-round3-performance-design-spec.md section
// 9.1 and re-derived by the M13 acceptance task. An empty `order_by` never
// appears: missing entries fall through to the default below.
struct RecordedProjection {
  std::string_view table;
  std::string_view order_by;
  std::vector<std::string_view> excluded_columns;
};

const RecordedProjection kRecorded[] = {
    {"analysis_facts", "fact_id", {}},
    {"run_fact_bindings", "rowid", {"run_id", "analyzer_run_id", "binding_id"}},
    {"provenance_nodes", "rowid", {"run_id"}},
    {"provenance_edges", "rowid", {"run_id"}},
};

// SHA-256 of `bytes`, hex-encoded. `Hash.h` offers no one-shot hex helper, so
// this is the composition the rest of the tree uses.
std::string Sha256Hex(std::string_view bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

// Every user table, in stable name order, plus SQLite's own bookkeeping.
StatusOr<std::vector<std::string>> ListTables(MetadataStore* store) {
  auto rows = store->Query(
      "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", {});
  if (!rows.ok()) return rows.status();
  std::vector<std::string> tables;
  tables.reserve(rows->size());
  for (const auto& row : *rows) {
    if (!row.empty()) tables.push_back(row[0]);
  }
  return tables;
}

// The column names of `table`, in declaration order.
StatusOr<std::vector<std::string>> ListColumns(MetadataStore* store,
                                              const std::string& table) {
  // PRAGMA table_info returns (cid, name, type, notnull, dflt_value, pk);
  // column 1 is the name.
  auto rows = store->Query("PRAGMA table_info(\"" + table + "\")", {});
  if (!rows.ok()) return rows.status();
  std::vector<std::string> columns;
  columns.reserve(rows->size());
  for (const auto& row : *rows) {
    if (row.size() < 2) {
      return Status::Internal("PRAGMA table_info returned a short row for " +
                              table);
    }
    columns.push_back(row[1]);
  }
  return columns;
}

// The value's bytes are joined into the row stream verbatim, so a tab or a
// newline inside one would move a boundary and let two different stores dump
// alike. Refusing is the only option that keeps the stream unambiguous: an
// escaping scheme is a second thing that can be wrong, and it would make a
// digest depend on the escaper's version.
Status RequireJoinable(std::string_view value, const std::string& table,
                       const std::string& column) {
  if (value.find_first_of("\t\n") == std::string_view::npos) {
    return Status::Ok();
  }
  return Status::FailedPrecondition(
      "table " + table + " column " + column +
      " holds a tab or newline, which the canonical row stream cannot join "
      "unambiguously");
}

}  // namespace

TableProjection ResolveTableProjection(std::string_view table) {
  for (const auto& recorded : kRecorded) {
    if (recorded.table == table) {
      TableProjection projection;
      projection.order_by = std::string(recorded.order_by);
      projection.excluded_columns.assign(recorded.excluded_columns.begin(),
                                         recorded.excluded_columns.end());
      return projection;
    }
  }
  return TableProjection{.order_by = "rowid", .excluded_columns = {}};
}

StatusOr<StoreDump> DumpStore(const std::filesystem::path& metadata_db_path) {
  auto store = MetadataStore::OpenReadOnly(metadata_db_path);
  if (!store.ok()) return store.status();

  auto tables = ListTables(&*store);
  if (!tables.ok()) return tables.status();

  StoreDump dump;
  std::string combined;
  for (const auto& table : *tables) {
    auto columns = ListColumns(&*store, table);
    if (!columns.ok()) return columns.status();
    const auto projection = ResolveTableProjection(table);

    // Reject a stale exclusion rather than letting it silently widen the
    // comparison. This is the check that would have caught a hand-written
    // exclusion list drifting away from the schema.
    for (const auto& excluded : projection.excluded_columns) {
      if (std::find(columns->begin(), columns->end(), excluded) == columns->end()) {
        return Status::FailedPrecondition(
            "table " + table + " has no column " + excluded +
            ", but its recorded projection excludes it");
      }
    }

    std::vector<std::string> retained;
    std::string select = "SELECT ";
    for (const auto& column : *columns) {
      if (std::find(projection.excluded_columns.begin(),
                    projection.excluded_columns.end(),
                    column) != projection.excluded_columns.end()) {
        continue;
      }
      if (!retained.empty()) select += ", ";
      select += "\"" + column + "\"";
      retained.push_back(column);
    }
    if (retained.empty()) {
      return Status::FailedPrecondition("every column of " + table +
                                       " is excluded from its projection");
    }
    select += " FROM \"" + table + "\" ORDER BY " + projection.order_by;

    auto rows = store->Query(select, {});
    if (!rows.ok()) return rows.status();

    std::string stream;
    for (const auto& row : *rows) {
      if (row.size() != retained.size()) {
        return Status::Internal("query for " + table + " returned " +
                                std::to_string(row.size()) + " of " +
                                std::to_string(retained.size()) + " columns");
      }
      for (std::size_t i = 0; i < row.size(); ++i) {
        Status joinable = RequireJoinable(row[i], table, retained[i]);
        if (!joinable.ok()) return joinable;
        if (i != 0) stream.push_back('\t');
        stream.append(row[i]);
      }
      stream.push_back('\n');
    }

    TableDump table_dump;
    table_dump.table = table;
    table_dump.order_by = projection.order_by;
    table_dump.excluded_columns = projection.excluded_columns;
    table_dump.row_count = rows->size();
    table_dump.sha256 = Sha256Hex(stream);
    combined += table + ":" + std::to_string(table_dump.row_count) + ":" +
                table_dump.sha256 + "\n";
    dump.tables.push_back(std::move(table_dump));
  }

  dump.sha256 = Sha256Hex(combined);
  return dump;
}

}  // namespace veritas::summarydb
