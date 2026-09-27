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

// The end-to-end statement M13 exists to make: two independent analyses of the
// same input publish equivalent content, asserted as a test rather than as a
// shell transcript.
//
// "Content" is half the claim, so it is asserted and not assumed: two schema-
// identical empty stores compare equal under any projection, so the case
// asserts the fixture actually published facts and provenance *before* it
// asserts the two runs agree. Without that half the test is satisfiable by a
// pipeline that publishes nothing.
//
// It lives in its own translation unit because it links `veritas_analysis` as
// well as `veritas_summarydb`, and because it is an integration test with a
// per-test timeout — `StoreEquivalenceTest` links only the second and is
// discovered with `gtest_discover_tests`, which has no per-test timeout.
// `tests/unit/summarydb/` holds it because the subject under test is the
// summarydb store-equivalence instrument the sibling file covers; only its link
// line and its runtime differ.

#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/summarydb/StoreEquivalence.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

#include "ProjectFixture.h"

namespace veritas {
namespace {

namespace fs = std::filesystem;

// Both runs share one materialized fixture: the point is that the *input* is
// held fixed while everything run-scoped about the analysis moves, so the two
// stores differ only where a run legitimately differs.
//
// Every filesystem call here is the `std::error_code` overload. The throwing
// ones are `std::terminate` under this project's `-fno-exceptions` build, which
// would abort the binary instead of failing the test — and a test whose failure
// mode is a crash reports nothing about the assertion that tripped.
TEST(StoreEquivalenceEndToEndTest, TwoRunsOfOneFixtureAreEquivalent) {
  std::error_code temp_error;
  const fs::path temp_root = fs::temp_directory_path(temp_error);
  ASSERT_FALSE(temp_error)
      << "cannot resolve the temporary directory: " << temp_error.message();

  // The names are per-process. A fixed name in the shared temporary directory
  // is not safe here: this repository runs several worktrees at once, and two
  // suites running this binary would otherwise clear each other's stores — which
  // the reset below turns from a stale file into one process deleting another's
  // store mid-analysis.
  //
  // The reset itself is still asserted rather than hoped for: a run that left a
  // store behind must be measured against a fresh one, or the comparison is
  // against stale content. Each call gets its own error code, because one shared
  // code would let the second call erase the first's.
  const std::string token =
      std::to_string(static_cast<unsigned long long>(::getpid()));
  const fs::path first_root = temp_root / ("veritas_m13_e2e_first_" + token);
  const fs::path second_root = temp_root / ("veritas_m13_e2e_second_" + token);
  std::error_code first_clean_error;
  fs::remove_all(first_root, first_clean_error);
  ASSERT_FALSE(first_clean_error)
      << "cannot clear " << first_root.string() << ": "
      << first_clean_error.message();
  std::error_code second_clean_error;
  fs::remove_all(second_root, second_clean_error);
  ASSERT_FALSE(second_clean_error)
      << "cannot clear " << second_root.string() << ": "
      << second_clean_error.message();

  const fs::path project = testing::FixtureProject("semantic_zoo");

  analysis::ProjectAnalyzer analyzer;
  const auto first = analyzer.AnalyzeProject(
      analysis::ProjectAnalysisRequest{.project_root = project,
                                       .output_root = first_root},
      analysis::AnalysisConfig::Default());
  ASSERT_TRUE(first.ok()) << first.status().message();
  // The second run reuses the materialized project but writes a fresh store.
  const auto second = analyzer.AnalyzeProject(
      analysis::ProjectAnalysisRequest{.project_root = project,
                                       .output_root = second_root},
      analysis::AnalysisConfig::Default());
  ASSERT_TRUE(second.ok()) << second.status().message();

  // Equality alone is satisfiable without publishing anything: two empty stores
  // with the same schema take the same projection and compare equal, and this
  // test would pass while proving nothing about content. So the claim is not
  // "these two agree" but "these two agree *about published content*", and the
  // content is asserted before the agreement is.
  //
  // The row counts are the fixture's, not literals, because what this test
  // pins is that both runs published the same non-empty content — pinning the
  // exact numbers would make it fail on an unrelated fixture edit and say
  // nothing extra about equivalence. That the two stores carry the same *set* of
  // tables is already asserted by the comparison's `left_only`/`right_only`
  // being empty below.
  const auto first_dump = summarydb::DumpStore(first_root / "metadata.db");
  ASSERT_TRUE(first_dump.ok()) << first_dump.status().message();
  const auto second_dump = summarydb::DumpStore(second_root / "metadata.db");
  ASSERT_TRUE(second_dump.ok()) << second_dump.status().message();
  EXPECT_GT(first_dump->tables.size(), 0u) << "the first store has no tables";
  EXPECT_EQ(first_dump->tables.size(), second_dump->tables.size());

  std::size_t fact_rows = 0;
  std::size_t edge_rows = 0;
  for (const auto& table : first_dump->tables) {
    if (table.table == "analysis_facts") fact_rows = table.row_count;
    if (table.table == "provenance_edges") edge_rows = table.row_count;
  }
  EXPECT_GT(fact_rows, 0u)
      << "the first run published no facts, so the comparison below is vacuous";
  EXPECT_GT(edge_rows, 0u)
      << "the first run published no provenance, so the comparison below is "
         "vacuous";

  const auto comparison =
      summarydb::CompareStoreFiles(first_root / "metadata.db",
                                   second_root / "metadata.db");
  ASSERT_TRUE(comparison.ok()) << comparison.status().message();

  // Name every differing table rather than only counting them. The projection in
  // `StoreEquivalence.cpp` is what decides equivalence, so when this fails the
  // useful output is which table moved — that is the name a reader takes back to
  // the projection table in the M13 acceptance record.
  std::string differences;
  for (const auto& table : comparison->differing) {
    differences += "\n  differs: " + table.table + " (left " +
                   std::to_string(table.left_rows) + " rows, right " +
                   std::to_string(table.right_rows) + " rows)";
  }
  for (const auto& table : comparison->left_only) {
    differences += "\n  left only: " + table;
  }
  for (const auto& table : comparison->right_only) {
    differences += "\n  right only: " + table;
  }
  EXPECT_TRUE(comparison->equal)
      << "two runs of one fixture published different content:" << differences;

  // Cleanup is reported rather than assumed. A failure here does not affect the
  // verdict above, but it does mean this test left a store in the temporary
  // directory, and an assertion that hides that is the same silence the rest of
  // this file exists to avoid. Each call gets its own error code for the reason
  // given above.
  std::error_code first_remove_error;
  fs::remove_all(first_root, first_remove_error);
  EXPECT_FALSE(first_remove_error)
      << "cannot remove " << first_root.string() << ": "
      << first_remove_error.message();
  std::error_code second_remove_error;
  fs::remove_all(second_root, second_remove_error);
  EXPECT_FALSE(second_remove_error)
      << "cannot remove " << second_root.string() << ": "
      << second_remove_error.message();
}

}  // namespace
}  // namespace veritas
