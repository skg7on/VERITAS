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

// StoreEquivalence.h — canonical, comparable dumps of a metadata store.
//
// This is an instrument, not a feature: it exists so two stores produced by
// different builds of the same input can be compared by digest rather than by
// argument. It must therefore be able to fail its own controls — a digest that
// cannot change proves nothing — and it must never mutate the store it
// inspects. Every read goes through `MetadataStore::OpenReadOnly`.

#ifndef VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_
#define VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"

namespace veritas::summarydb {

// How one table is projected into a canonical, comparable form.
//
// `order_by` is the column or key the rows are sorted by before digesting.
// Sorting is what makes the digest independent of insertion order and of the
// physical fan-out the writer happened to produce.
//
// `excluded_columns` are omitted from the digest. They hold run-scoped
// identities that move between runs of unchanged input by construction, so
// including them would make every comparison fail.
struct TableProjection {
  std::string order_by;
  std::vector<std::string> excluded_columns;
};

// The determined projection for `table`. Tables with no recorded entry get
// `{"rowid", {}}`. The entries for the published tables are the projection
// documented in
// docs/specs/veritas-build-analyze-round3-performance-design-spec.md section
// 9.1, which round 3 determined by measurement rather than assumed.
//
// A column named in an entry that does not exist in the table is an error at
// dump time, not silently ignored: a stale exclusion would otherwise widen the
// comparison without anyone noticing.
TableProjection ResolveTableProjection(std::string_view table);

// One table's canonical dump.
struct TableDump {
  std::string table;
  std::string order_by;
  std::vector<std::string> excluded_columns;
  std::size_t row_count = 0;
  // SHA-256 over the canonical row stream: for each row in `order_by` order, the
  // retained column values joined by '|', each row terminated by '\n'.
  //
  // A value carrying one of the stream's own bytes is escaped rather than passed
  // through: `\` becomes `\\`, `|` becomes `\|`, and a newline becomes `\n`. The
  // escape is prefix-free and invertible, so column and row boundaries are
  // unambiguous by construction and the dump is total — no value is refused.
  //
  // An earlier design refused such values with FailedPrecondition, on the theory
  // that escaping is a second thing that can be wrong. Real stores refuted it:
  // the round-3 baseline stores hold embedded newlines in
  // `cpg_edge_support.provenance_ref` (17,963 rows) and in two `cpg_projections`
  // columns, so refusal made the dump partial and a whole-store sweep impossible.
  // Do not re-derive refusal as the obvious design.
  //
  // For a table whose values carry none of the three escaped bytes — which
  // includes `analysis_facts`, the milestone's anchor — escaping is a no-op and
  // this digest is exactly the one `sqlite3 … "SELECT …" | shasum -a 256`
  // produces, so section 9.1's recorded literals are checkable by hand. For a
  // table that does carry a newline there is no section 9.1 digest to reproduce:
  // that instrument cannot distinguish a newline inside a value from a row
  // boundary, so it is ambiguous there by construction. Escaping is what closes
  // the gap the earlier instrument left, not a departure from it.
  std::string sha256;
};

// A canonical dump of every table in a store.
struct StoreDump {
  std::vector<TableDump> tables;  // sorted by table name
  // SHA-256 over the concatenation of each table's name, row count, and digest,
  // in `tables` order, each entry newline-terminated. Cell values are escaped as
  // `TableDump::sha256` describes; table names are not, so this outer stream
  // assumes names are plain SQLite identifiers, as every schema in this
  // repository produces.
  std::string sha256;

  // Limitation, inherited from `MetadataStore::Query`: SQL NULL is returned as
  // the empty string, so this dump cannot distinguish a NULL cell from an empty
  // one. Two stores that differ only in that respect compare equal. Closing
  // that gap needs a NULL-aware read path, not a different dump.
};

// Dump every user table of the store at `metadata_db_path` in canonical form.
//
// The store is opened read-only; nothing is created, migrated, or written.
// SQLite's own internal tables (names beginning `sqlite_`) are included, because
// `sqlite_sequence` records AUTOINCREMENT high-water marks that differ between
// runs and a caller should see that rather than have it hidden.
//
// Cell values carrying the stream's own bytes are escaped, never refused, so
// the dump is total: any store this can open it can dump.
//
// Failures, none of which are silent:
//   * NotFound — no store exists at the path;
//   * FailedPrecondition — a table's determined projection names a column the
//     table does not have, or every one of a table's columns is excluded;
//   * whatever the first query reports — `OpenReadOnly` succeeds for a path that
//     exists but is not a SQLite database, because SQLite defers validation, so
//     a file that is not a store is diagnosed here rather than at open time.
StatusOr<StoreDump> DumpStore(const std::filesystem::path& metadata_db_path);

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_
