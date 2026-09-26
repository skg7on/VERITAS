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

#include "veritas/observability/StoreSummary.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::observability {
namespace {

namespace fs = std::filesystem;

// A unique temp directory per case, so the cases never share a store.
fs::path FreshDir(const std::string& tag) {
  const fs::path dir = fs::temp_directory_path() /
                       ("veritas-store-summary-" + tag + "-" +
                        std::to_string(std::rand()));
  std::error_code error;
  fs::remove_all(dir, error);
  return dir;
}

TEST(StoreSummaryTest, CountsEveryPublishedTableIncludingEmptyOnes) {
  // A schema-applied store has the tables but no rows. This is the test that
  // catches a misspelled table name: a name the schema does not define makes
  // the count query fail, and CollectStoreSummary propagates that failure
  // rather than reporting zero.
  const fs::path output_root = FreshDir("empty");
  ASSERT_TRUE(fs::create_directories(output_root));
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();

  // The four published fact tables plus the component-state table the
  // cross-check reads. Ordering is by name, which the next assertion pins.
  ASSERT_EQ(summary->tables.size(), 5u);
  for (const TableRowCount& table : summary->tables) {
    EXPECT_EQ(table.rows, 0u) << table.table;
  }
  EXPECT_TRUE(std::is_sorted(
      summary->tables.begin(), summary->tables.end(),
      [](const TableRowCount& left, const TableRowCount& right) {
        return left.table < right.table;
      }));
  // An empty store is a success, not a failure. That is already asserted by
  // reaching this line at all: CollectStoreSummary returns non-OK if any count
  // query fails.
  //
  // Do NOT add an assertion about `cross_checks` here. This collector leaves
  // that vector empty by design — comparing against the in-memory count is the
  // caller's job — so any claim about its contents is vacuously true in this
  // test and can never fail. The cross-check assertions belong where the vector
  // is actually populated.
}

TEST(StoreSummaryTest, CountsInsertedRows) {
  const fs::path output_root = FreshDir("rows");
  ASSERT_TRUE(fs::create_directories(output_root));
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  // provenance_nodes has NO foreign-key parent — only five NOT NULL columns and
  // the composite primary key (run_id, output_fact_id, witness_id) — so one
  // synthetic row needs no other table to exist first, and the insert cannot
  // fail for a reason unrelated to counting. Read the columns from
  // `src/summarydb/schema/v3.sql` rather than assuming them: an earlier draft
  // of this plan inserted into a `node_id` column this table does not have.
  ASSERT_TRUE(store
                  ->Execute("INSERT INTO provenance_nodes "
                            "(run_id, output_fact_id, witness_id, selected, "
                            "producer_kind) VALUES (?, ?, ?, ?, ?)",
                            {"run:sha256:test", "fact:sha256:test",
                             "witness:sha256:test", "0", "0"})
                  .ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();
  const auto found = std::find_if(
      summary->tables.begin(), summary->tables.end(),
      [](const TableRowCount& table) {
        return table.table == "provenance_nodes";
      });
  ASSERT_NE(found, summary->tables.end());
  EXPECT_EQ(found->rows, 1u);
}

TEST(StoreSummaryTest, GroupsStoreBytesByTopLevelEntryWithoutAbsolutePaths) {
  const fs::path output_root = FreshDir("bytes");
  ASSERT_TRUE(fs::create_directories(output_root / "cas"));
  std::ofstream(output_root / "cas" / "one.bin") << "12345678";

  // `metadata.db` must be a REAL store, not a placeholder file. The collector
  // queries it, and it propagates a failed count query rather than reporting
  // zero, so a four-byte text file makes it fail with SQLite's "file is not a
  // database" and this test unsatisfiable. Create the schema instead.
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();
  ASSERT_FALSE(summary->bytes.empty());
  EXPECT_TRUE(std::is_sorted(
      summary->bytes.begin(), summary->bytes.end(),
      [](const NamedBytes& left, const NamedBytes& right) {
        return left.name < right.name;
      }));
  for (const NamedBytes& entry : summary->bytes) {
    // Names are relative to the output root; an absolute path here would make
    // two machines' artifacts differ for no semantic reason.
    EXPECT_EQ(entry.name.find(output_root.string()), std::string::npos);
    EXPECT_EQ(entry.name.find('/'), std::string::npos);
  }
}

TEST(StoreSummaryTest, ReportsFailureRatherThanZeroForAMissingStore) {
  const auto summary = CollectStoreSummary(FreshDir("absent"));
  EXPECT_FALSE(summary.ok());
}

TEST(StoreSummaryTest, FillsEnvironmentFromTheMachine) {
  RunEnvironment environment;
  FillEnvironment(&environment);
  EXPECT_FALSE(environment.os.empty());
  EXPECT_FALSE(environment.arch.empty());
  EXPECT_GT(environment.cores, 0u);
  EXPECT_GT(environment.ram_bytes, 0u);
  EXPECT_FALSE(environment.host_compiler.empty());
  // A version string with no dot would mean the fields were never filled.
  EXPECT_NE(environment.veritas_version.find('.'), std::string::npos);
}

TEST(StoreSummaryTest, FillsInputInventoryFromTheManifest) {
  // The manifest is built by the same ingestion the analyzer uses, so this
  // asserts the copy, not the ingestion.
  const auto project = testing::FixtureProject("multiple_tus");
  auto input = veritas::build::ResolveProjectInput(
      veritas::analysis::ProjectAnalysisRequest{
          .project_root = project,
          .output_root = FreshDir("inventory")});
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = veritas::build::LoadProjectManifest(*input);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  RunInputInventory inventory;
  FillInventoryFromManifest(*manifest, &inventory);
  EXPECT_EQ(inventory.translation_units, manifest->translation_units.size());
  EXPECT_FALSE(inventory.compiler_id.empty());
  EXPECT_FALSE(inventory.target_triple.empty());
}

}  // namespace
}  // namespace veritas::observability
