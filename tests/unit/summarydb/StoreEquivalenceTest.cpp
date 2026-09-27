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
#include <system_error>

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

  // A one-table store whose table carries a recorded exclusion: the entry for
  // `provenance_nodes` orders by `rowid` and excludes `run_id`. `body` is the
  // retained content, kept in its own column so a case can move the excluded
  // column without touching the digest — which is the only way to see whether
  // the exclusion reaches the SELECT rather than merely satisfying the
  // stale-exclusion guard.
  void MakeProvenanceStore(const std::string& run_id, const std::string& body) {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    ASSERT_TRUE(
        store->Execute("CREATE TABLE provenance_nodes (body TEXT, run_id TEXT)",
                       {})
            .ok());
    const std::string insert =
        "INSERT INTO provenance_nodes (body, run_id) VALUES ('" + body + "','" +
        run_id + "')";
    ASSERT_TRUE(store->Execute(insert, {}).ok());
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

// A value carrying one of the stream's own bytes is escaped, not stripped and not
// refused. Stripping is the failure these assertions exist to catch: a dump that
// dropped the byte would render two genuinely different stores identical, which
// is the one outcome the instrument must never produce.
TEST_F(StoreEquivalenceTest, ValuesCarryingBoundaryBytesAreEscapedNotStripped) {
  MakeStore("has|pipe");
  const auto with_pipe = DumpStore(db_path_);
  ASSERT_TRUE(with_pipe.ok()) << with_pipe.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("haspipe");
  const auto without_pipe = DumpStore(db_path_);
  ASSERT_TRUE(without_pipe.ok()) << without_pipe.status().message();
  EXPECT_NE(with_pipe->sha256, without_pipe->sha256)
      << "the '|' was dropped from the stream rather than escaped";

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("has\nnewline");
  const auto with_newline = DumpStore(db_path_);
  ASSERT_TRUE(with_newline.ok()) << with_newline.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("hasnewline");
  const auto without_newline = DumpStore(db_path_);
  ASSERT_TRUE(without_newline.ok()) << without_newline.status().message();
  EXPECT_NE(with_newline->sha256, without_newline->sha256)
      << "the newline was dropped from the stream rather than escaped";

  // A tab is neither a separator nor escaped, so it survives verbatim and the
  // dump stays total.
  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("has\ttab");
  EXPECT_TRUE(DumpStore(db_path_).ok()) << "a tab was refused";
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

// The escape itself, pinned by known answers. The two cases above cannot pin it:
// they would still pass under an escape that dropped a byte but stayed
// self-consistent, and the backslash is the byte most likely to be forgotten.
// Each expected value is the digest of one row of `beta` — id `b1`, the note
// below — computed independently of this implementation with
//   printf '%s\n' '<escaped row>' | shasum -a 256
// over these three notes:
//   'p|q'      -> b1|p\|q
//   'm' LF 'n' -> b1|m\nn
//   'x|y' LF 'z\w' -> b1|x\|y\nz\\w
// and the store digest as
//   printf '%s\n' "alpha:2:<alpha-digest>" "beta:1:<beta-digest>" | shasum -a 256
// where <alpha-digest> is the unescaped fixture's 6f20be56…, unchanged here
// because alpha's values carry nothing to escape.
TEST_F(StoreEquivalenceTest, TheEscapeIsPinnedByKnownAnswers) {
  MakeStore("p|q");
  const auto piped = DumpStore(db_path_);
  ASSERT_TRUE(piped.ok()) << piped.status().message();
  ASSERT_EQ(piped->tables.size(), 2u);
  EXPECT_EQ(piped->tables[1].sha256,
            "9370b3dee6a807ad5b89bd83a6ba2ab25d494a31a2ddef818ce75bbb772c6139");

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("m\nn");
  const auto newlined = DumpStore(db_path_);
  ASSERT_TRUE(newlined.ok()) << newlined.status().message();
  ASSERT_EQ(newlined->tables.size(), 2u);
  EXPECT_EQ(newlined->tables[1].sha256,
            "589041ca8ad82cef9526d13828b2a4000d13c8b4200cd25a2c27935ea345dd66");

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("x|y\nz\\w");
  const auto mixed = DumpStore(db_path_);
  ASSERT_TRUE(mixed.ok()) << mixed.status().message();
  ASSERT_EQ(mixed->tables.size(), 2u);
  EXPECT_EQ(mixed->tables[1].sha256,
            "2034bdd31aa302a48ff4203be37277a94a1ca09a69d1f4e0e461822978697258");
  EXPECT_EQ(mixed->sha256,
            "b142516db8c87cc0fa07cf803625e337f230815d1fc39d61115ea0bdc2ea420f");
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

// Comparison, and the perturbations it must detect.
//
// Every case below is a perturbation the instrument has to *report*. A control
// the implementation cannot fail proves nothing about the implementation, so
// each one is described with the mutation that would break it in the task
// report: the controls are the milestone's acceptance argument, not decoration.

// The base case, and the control for the five below: an instrument that called
// every pair of stores different would satisfy all of them without comparing
// anything.
TEST_F(StoreEquivalenceTest, IdenticalStoresCompareEqual) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  const auto comparison = CompareDumps(*left, *left);
  EXPECT_TRUE(comparison.equal);
  EXPECT_TRUE(comparison.differing.empty());
  EXPECT_TRUE(comparison.left_only.empty());
  EXPECT_TRUE(comparison.right_only.empty());
}

// A single changed cell in one row of one table, with the row count unchanged.
// This is the perturbation that a row-count-only check would miss, so it is what
// pins the digest itself rather than the count.
TEST_F(StoreEquivalenceTest, ASingleChangedCellIsDetected) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("n2");
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "beta");
  EXPECT_EQ(comparison.differing[0].left_rows, 1u);
  EXPECT_EQ(comparison.differing[0].right_rows, 1u);
}

// A dropped row in one table, reported with both counts: a caller has to be able
// to tell "same content, fewer rows" from "different content", which a single
// boolean cannot express.
TEST_F(StoreEquivalenceTest, ADroppedRowIsDetected) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("DELETE FROM alpha WHERE id = 'a2'", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "alpha");
  EXPECT_EQ(comparison.differing[0].left_rows, 2u);
  EXPECT_EQ(comparison.differing[0].right_rows, 1u);
}

// A table present on one side only. Reporting this separately from "differing"
// is what lets a caller distinguish a schema change from a content change, and it
// is a case the `differing` loop alone cannot produce: the table has no
// counterpart to disagree with.
TEST_F(StoreEquivalenceTest, ATablePresentOnOneSideIsReportedSeparately) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("CREATE TABLE gamma (x TEXT)", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  EXPECT_TRUE(comparison.differing.empty());
  ASSERT_EQ(comparison.right_only.size(), 1u);
  EXPECT_EQ(comparison.right_only[0], "gamma");
  EXPECT_TRUE(comparison.left_only.empty());
}

// Reordering must be *visible*, and it is one of the four perturbations issue
// #141 names. The default projection orders by `rowid`, and deleting both rows
// and re-inserting them in the opposite order reassigns rowids (a2 -> 1,
// a1 -> 2), so the two row streams differ while the row count does not.
//
// The plan expected this case to compare equal, on the theory that row order is
// not semantic. That expectation was false for the instrument as built, and it
// contradicts the milestone's own acceptance criterion; the accepted outcome is
// detection. The reordering is not hidden — it shows up as a digest difference
// with both counts equal, which is what a caller sees and can act on.
TEST_F(StoreEquivalenceTest, ReorderedRowsAreDetected) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("DELETE FROM alpha", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha VALUES ('a2','p2')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha VALUES ('a1','p1')", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal) << "reinserting the same rows in the opposite "
                                    "order did not change the digest";
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "alpha");
  EXPECT_EQ(comparison.differing[0].left_rows, 2u);
  EXPECT_EQ(comparison.differing[0].right_rows, 2u);
}

// `excluded_columns` must reach the SELECT, not merely satisfy the
// stale-exclusion guard. No other fixture table has a recorded exclusion whose
// values differ between the two stores being compared, so an implementation that
// kept the exclusion list only for the guard and selected every column anyway
// would leave every other case in this file green. `provenance_nodes` is the
// recorded projection with the smallest exclusion (`run_id`), which lets the
// excluded column and a retained column move independently.
//
// Both halves matter. The first is the one that fails under the mutation; the
// second is the control for the first, since an implementation that dropped
// whole tables from the dump would also make the first half pass.
TEST_F(StoreEquivalenceTest, ExcludedColumnsAreRemovedFromTheDigest) {
  MakeProvenanceStore("run-a", "n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeProvenanceStore("run-b", "n1");
  const auto moved_run_id = DumpStore(db_path_);
  ASSERT_TRUE(moved_run_id.ok()) << moved_run_id.status().message();
  EXPECT_TRUE(CompareDumps(*left, *moved_run_id).equal)
      << "run_id is excluded from provenance_nodes, yet changing it changed the "
         "digest";

  ASSERT_TRUE(fs::remove(db_path_));
  MakeProvenanceStore("run-a", "n2");
  const auto moved_content = DumpStore(db_path_);
  ASSERT_TRUE(moved_content.ok()) << moved_content.status().message();
  const auto comparison = CompareDumps(*left, *moved_content);
  EXPECT_FALSE(comparison.equal)
      << "a retained column changed and the stores still compared equal";
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "provenance_nodes");
}

// Comparing across files is the form every later milestone uses. The two stores
// are byte-identical copies, so the comparison is vacuous as a perturbation and
// is here only to pin the file-level entry point against the dump-level one.
TEST_F(StoreEquivalenceTest, CompareStoreFilesAgreesWithCompareDumps) {
  MakeStore("n1");
  const auto path = db_path_;
  const auto right_path = fs::temp_directory_path() / "veritas_store_equiv_right.db";
  fs::remove(right_path);
  // The two-argument `fs::copy_file` throws on failure, which under
  // `-fno-exceptions` is `std::terminate`: a failed copy would abort this binary
  // and take the other cases' results with it instead of reporting a failure.
  // The `error_code` overload is the one this project's compilation policy
  // allows, and it makes the assertion an assertion.
  std::error_code copy_error;
  ASSERT_TRUE(fs::copy_file(path, right_path, copy_error))
      << copy_error.message();

  const auto comparison = CompareStoreFiles(path, right_path);
  ASSERT_TRUE(comparison.ok()) << comparison.status().message();
  EXPECT_TRUE(comparison->equal);

  fs::remove(right_path);
}

}  // namespace
}  // namespace veritas::summarydb
