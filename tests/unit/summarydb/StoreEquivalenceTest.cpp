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

// The row stream joins values with '\t' and terminates rows with '\n', so a
// value carrying either byte would make two different stores dump alike. The
// dump refuses such a value instead of escaping it: the stream is unambiguous
// by construction, not by an assumption a caller has to check.
TEST_F(StoreEquivalenceTest, ValuesTheRowStreamCannotRepresentAreRefused) {
  MakeStore("has\ttab");
  const auto tabbed = DumpStore(db_path_);
  ASSERT_FALSE(tabbed.ok()) << "a value containing a tab was dumped anyway";
  EXPECT_EQ(tabbed.status().code(), StatusCode::kFailedPrecondition);

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("has\nnewline");
  const auto newlined = DumpStore(db_path_);
  ASSERT_FALSE(newlined.ok()) << "a value containing a newline was dumped anyway";
  EXPECT_EQ(newlined.status().code(), StatusCode::kFailedPrecondition);
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
