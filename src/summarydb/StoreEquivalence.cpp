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
#include <iterator>
#include <map>
#include <span>
#include <string_view>
#include <utility>

#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

// The determined projection. The measurement behind it, and the corrections it
// makes to round 3's prose, are in
// docs/specs/milestones/m13-scale-profile-acceptance-record.md. An empty
// `order_by` never appears: missing entries fall through to the default below.
//
// Two kinds of column are excluded, and they were determined two different
// ways, so the distinction is worth keeping:
//
//   * wall-clock columns (`created_at`, `updated_at`, `started_at`,
//     `completed_at`, `publication_epoch`), determined by comparing two runs of
//     one input column by column. Every one of them is a SQLite
//     `DEFAULT (strftime('%s', 'now'))` or an equivalent, so it records when the
//     row was written and not what was written.
//   * toolchain-bound identities (`run_id` and what is derived from it,
//     `engine_toolchain_identity`), determined by reproducing round 3 section
//     9.1's recorded digests. These do NOT move between two runs of one input
//     in one build tree, so the column-by-column comparison cannot see them —
//     it would report every one of them as stable. They move between two
//     *builds* of one input, because `run_id` binds the provenance digest of
//     the linked Souffle libraries. Round 3 section 9.1 determined them and
//     this task re-derived them by reproducing its literals; dropping them
//     because a same-tree pair cannot see the motion would make the instrument
//     report `differ` on the baseline-versus-scaled comparison it exists for
//     (docs/plans/veritas-scaling-milestone-roadmap.md 6.2).
struct RecordedProjection {
  std::string_view table;
  std::string_view order_by;
  std::vector<std::string_view> excluded_columns;
};

const RecordedProjection kRecorded[] = {
    // Published tables. Round 3 section 9.1 determined all four, and the
    // measurement reproduced every one of its recorded digests exactly:
    // 452a850e… (analysis_facts, fact_id), d732ec43… (run_fact_bindings),
    // d8410e27… (provenance_nodes), 6691f96f… (provenance_edges).
    {"analysis_facts", "fact_id", {}},
    {"run_fact_bindings", "rowid", {"run_id", "analyzer_run_id", "binding_id"}},
    {"provenance_nodes", "rowid", {"run_id"}},
    {"provenance_edges", "rowid", {"run_id"}},
    // WPA bookkeeping. Round 3 section 9.1 determined the first and the third
    // and both reproduced (bc3a24c4…, bf38b362…). The second did not: section
    // 9.1's prose lists three exclusions for it, and its recorded digest
    // 93c3aef6… is only reproduced with `updated_at` excluded as well, so
    // `updated_at` is the correction this task made to that entry.
    {"wpa_component_states", "rowid", {"updated_at"}},
    {"wpa_component_states_v2",
     "rowid",
     {"run_id", "result_cache_key", "result_object_key", "updated_at"}},
    {"wpa_component_result_cache_v2",
     "rowid",
     {"engine_toolchain_identity", "result_cache_key", "result_object_key"}},
    // Tables round 3 section 9.1 recorded no digest for. Each was determined by
    // comparing the two measured pairs column by column, and each exclusion
    // below is a wall-clock column measured to move in both pairs.
    {"build_variants", "rowid", {"created_at"}},
    {"repositories", "rowid", {"created_at"}},
    {"revisions", "rowid", {"created_at"}},
    {"summary_bindings", "rowid", {"publication_epoch"}},
    {"summary_objects", "rowid", {"created_at"}},
    {"translation_units", "rowid", {"created_at"}},
    {"wpa_analysis_runs", "rowid", {"started_at", "completed_at"}},
    {"wpa_sccs", "rowid", {"created_at"}},
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

// The row stream's separators are '|' between columns and '\n' between rows, so
// no value may carry either byte raw. Escaping is applied per value before the
// join, and the backslash is escaped first: without that, a value holding the
// two characters `\` and `|` would forge the escape sequence that stands for a
// literal '|', and the encoding would not be invertible. With all three mapped,
// the transform is a prefix-free escape, so column and row boundaries are
// unambiguous by construction and nothing has to be refused.
//
// An earlier design did refuse such values. Real stores carry them — the round-3
// baseline stores hold newlines in `cpg_edge_support.provenance_ref` (17,963
// rows) and in two `cpg_projections` columns — so refusal made the dump partial
// and a whole-store sweep impossible.
std::string EscapeValue(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char byte : value) {
    switch (byte) {
      case '\\':
        escaped += "\\\\";
        break;
      case '|':
        escaped += "\\|";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped.push_back(byte);
        break;
    }
  }
  return escaped;
}

}  // namespace

std::vector<std::string> RecordedProjectionTables() {
  std::vector<std::string> tables;
  tables.reserve(std::size(kRecorded));
  for (const auto& recorded : kRecorded) {
    tables.emplace_back(recorded.table);
  }
  return tables;
}

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
        if (i != 0) stream.push_back('|');
        stream.append(EscapeValue(row[i]));
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

StoreComparison CompareDumps(const StoreDump& left, const StoreDump& right) {
  // Keyed by name rather than walked in order: `StoreDump::tables` is documented
  // as sorted, but this is also called on dumps a caller assembled, and a
  // comparison that silently mis-paired tables would be worse than a slow one.
  // The std::map also makes the three output vectors come out in name order.
  std::map<std::string, const TableDump*> left_tables;
  std::map<std::string, const TableDump*> right_tables;
  for (const auto& table : left.tables) left_tables[table.table] = &table;
  for (const auto& table : right.tables) right_tables[table.table] = &table;

  StoreComparison comparison;
  for (const auto& [name, left_table] : left_tables) {
    const auto found = right_tables.find(name);
    if (found == right_tables.end()) {
      comparison.left_only.push_back(name);
      continue;
    }
    const TableDump& right_table = *found->second;
    if (left_table->sha256 == right_table.sha256 &&
        left_table->row_count == right_table.row_count) {
      continue;
    }
    TableComparison differing;
    differing.table = name;
    differing.left_rows = left_table->row_count;
    differing.right_rows = right_table.row_count;
    differing.equal = false;
    comparison.differing.push_back(std::move(differing));
  }
  // A table only on the right is not "differing": it has no counterpart to
  // disagree with, and folding it in would hide a schema change inside a list of
  // content changes.
  for (const auto& entry : right_tables) {
    if (left_tables.find(entry.first) == left_tables.end()) {
      comparison.right_only.push_back(entry.first);
    }
  }

  comparison.equal = comparison.differing.empty() &&
                     comparison.left_only.empty() &&
                     comparison.right_only.empty();
  return comparison;
}

StatusOr<StoreComparison> CompareStoreFiles(const std::filesystem::path& left,
                                            const std::filesystem::path& right) {
  auto left_dump = DumpStore(left);
  if (!left_dump.ok()) return left_dump.status();
  auto right_dump = DumpStore(right);
  if (!right_dump.ok()) return right_dump.status();
  return CompareDumps(*left_dump, *right_dump);
}

}  // namespace veritas::summarydb
