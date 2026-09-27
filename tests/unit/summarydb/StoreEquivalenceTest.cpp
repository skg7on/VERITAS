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

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

namespace fs = std::filesystem;

// SHA-256 of a file's bytes, for "did this change?" checks. Uses the same
// hashing the project already depends on so no new dependency appears.
std::string FileDigest(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  return veritas::core::DigestToHex(veritas::core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

class StoreEquivalenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    db_path_ = fs::temp_directory_path() /
               ("veritas_store_equiv_" + std::string(info->name()) + ".db");
    fs::remove(db_path_);
  }

  void TearDown() override { fs::remove(db_path_); }

  // A minimal two-table store, small enough to reason about by hand.
  void MakeStore(const std::string& second_row) {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    ASSERT_TRUE(store->Execute("CREATE TABLE alpha (id TEXT, payload TEXT)", {}).ok());
    ASSERT_TRUE(store->Execute("CREATE TABLE beta (id TEXT, note TEXT)", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha (id, payload) VALUES ('a1','p1')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha (id, payload) VALUES ('a2','p2')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO beta (id, note) VALUES ('b1','" +
                               second_row + "')", {}).ok());
  }

  fs::path db_path_;
};

// The projection for a table with no determined entry is the default: order by
// the physical row order and exclude nothing. `rowid` is the one key every
// table in this store has, and the writer's own row order is what round 3's
// instrument uses for the run-scoped tables.
TEST_F(StoreEquivalenceTest, UndeterminedTablesUseTheDefaultProjection) {
  const auto projection = ResolveTableProjection("some_future_table");
  EXPECT_EQ(projection.order_by, "rowid");
  EXPECT_TRUE(projection.excluded_columns.empty());
}

// The four published tables carry the projection round 3 determined. These
// entries are the reason the instrument is trustworthy: they are recorded
// rather than guessed, and Task 6 re-derives them.
TEST_F(StoreEquivalenceTest, PublishedTablesCarryTheDeterminedProjection) {
  const auto facts = ResolveTableProjection("analysis_facts");
  EXPECT_EQ(facts.order_by, "fact_id");
  EXPECT_TRUE(facts.excluded_columns.empty());

  const auto bindings = ResolveTableProjection("run_fact_bindings");
  EXPECT_EQ(bindings.order_by, "rowid");
  EXPECT_EQ(bindings.excluded_columns,
            (std::vector<std::string>{"run_id", "analyzer_run_id", "binding_id"}));

  const auto nodes = ResolveTableProjection("provenance_nodes");
  EXPECT_EQ(nodes.order_by, "rowid");
  EXPECT_EQ(nodes.excluded_columns, (std::vector<std::string>{"run_id"}));

  const auto edges = ResolveTableProjection("provenance_edges");
  EXPECT_EQ(edges.order_by, "rowid");
  EXPECT_EQ(edges.excluded_columns, (std::vector<std::string>{"run_id"}));
}

// A dump is a function of content, not of the file's incidental state.
TEST_F(StoreEquivalenceTest, DumpingTheSameStoreTwiceIsIdentical) {
  MakeStore("n1");
  const auto first = DumpStore(db_path_);
  const auto second = DumpStore(db_path_);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(first->sha256, second->sha256);
  ASSERT_EQ(first->tables.size(), second->tables.size());
  EXPECT_EQ(first->tables.size(), 2u)
      << "expected alpha and beta: sqlite_sequence is created only once a table "
         "declares AUTOINCREMENT, and this fixture declares none";
}

// Every table is present exactly once and the order is stable, so a digest is
// comparable across machines.
TEST_F(StoreEquivalenceTest, TablesAreSortedByName) {
  MakeStore("n1");
  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  for (std::size_t i = 1; i < dump->tables.size(); ++i) {
    EXPECT_LT(dump->tables[i - 1].table, dump->tables[i].table);
  }
}

// The digest must be sensitive to content, or every later comparison is
// vacuous. This is the instrument's own negative control.
TEST_F(StoreEquivalenceTest, ADifferentCellChangesTheDigest) {
  MakeStore("n1");
  const auto first = DumpStore(db_path_);
  ASSERT_TRUE(first.ok()) << first.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("n2");
  const auto second = DumpStore(db_path_);
  ASSERT_TRUE(second.ok()) << second.status().message();

  EXPECT_NE(first->sha256, second->sha256);
}

// Row counts are reported so a caller can tell "same content, fewer rows" from
// "different content".
TEST_F(StoreEquivalenceTest, RowCountsAreReported) {
  MakeStore("n1");
  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  std::size_t alpha_rows = 0;
  for (const auto& table : dump->tables) {
    if (table.table == "alpha") alpha_rows = table.row_count;
  }
  EXPECT_EQ(alpha_rows, 2u);
}

// The row stream joins values with '|' and terminates rows with '\n', so a
// value carrying either byte would make two different stores dump alike. The
// dump refuses such a value instead of escaping it: the stream is unambiguous
// by construction, not by an assumption a caller has to check.
TEST_F(StoreEquivalenceTest, ValuesTheRowStreamCannotRepresentAreRefused) {
  MakeStore("has|pipe");
  const auto piped = DumpStore(db_path_);
  ASSERT_FALSE(piped.ok()) << "a value containing a pipe was dumped anyway";
  EXPECT_EQ(piped.status().code(), StatusCode::kFailedPrecondition);

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("has\nnewline");
  const auto newlined = DumpStore(db_path_);
  ASSERT_FALSE(newlined.ok()) << "a value containing a newline was dumped anyway";
  EXPECT_EQ(newlined.status().code(), StatusCode::kFailedPrecondition);

  // A tab is not the separator, so it is an ordinary byte and must survive the
  // dump. Refusing it would be a silent narrowing of which stores are comparable.
  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("has\ttab");
  EXPECT_TRUE(DumpStore(db_path_).ok())
      << "a tab was refused, though it is not the separator";
}

// The canonical form is a specification, not whatever the implementation
// happens to print: without a known-answer case, any self-consistent separator,
// column order, or combined-stream layout stays green, and this format is what
// the milestone's acceptance argument quotes.
//
// The three literals were computed independently of this implementation, over
// the same rows `MakeStore` writes, with:
//   sqlite3 fixture.db "SELECT id, payload FROM alpha ORDER BY rowid" | shasum -a 256
//   sqlite3 fixture.db "SELECT id, note FROM beta ORDER BY rowid" | shasum -a 256
//   printf '%s\n' "alpha:2:<alpha-digest>" "beta:1:<beta-digest>" | shasum -a 256
// so they pin the separator, the retained column order (declaration order), the
// table order (name order), and the combined stream's `name:rows:digest` layout.
TEST_F(StoreEquivalenceTest, TheCanonicalFormIsPinnedByAKnownAnswer) {
  MakeStore("n1");
  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  ASSERT_EQ(dump->tables.size(), 2u);

  ASSERT_EQ(dump->tables[0].table, "alpha");
  EXPECT_EQ(dump->tables[0].sha256,
            "6f20be56595a5c45f0bf79b1380e789c4324d49a937d630a819369f7b0dc244d");

  ASSERT_EQ(dump->tables[1].table, "beta");
  EXPECT_EQ(dump->tables[1].sha256,
            "9fadbe8ef94a40455ebc35ee4841e71796d6b6ecbf441be10c256a47ce425b95");

  EXPECT_EQ(dump->sha256,
            "6102128f0c28b4b11fe81976d75b236ff015e308249077926029406d1fdaf5b6");
}

// A recorded exclusion that names a column the table does not have is stale:
// the projection it belongs to no longer describes this schema, and letting it
// through would silently widen the comparison.
TEST_F(StoreEquivalenceTest, AStaleExclusionIsRefused) {
  auto store = MetadataStore::Open(db_path_);
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(
      store->Execute("CREATE TABLE provenance_nodes (id TEXT)", {}).ok());

  const auto dump = DumpStore(db_path_);
  ASSERT_FALSE(dump.ok()) << "a projection excluding a missing column was applied";
  EXPECT_EQ(dump.status().code(), StatusCode::kFailedPrecondition);
}

// The instrument must never mutate the store it inspects — otherwise a later
// comparison measures the instrument. Task 3 pins this for OpenReadOnly itself;
// this pins it for the dump, which is the path a caller actually invokes.
TEST_F(StoreEquivalenceTest, DumpingLeavesTheStoreByteIdentical) {
  MakeStore("n1");
  const auto before = FileDigest(db_path_);

  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  ASSERT_GT(dump->tables.size(), 0u);

  EXPECT_EQ(FileDigest(db_path_), before) << "DumpStore wrote to the store";
}

}  // namespace
}  // namespace veritas::summarydb
