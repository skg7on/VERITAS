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

#include "veritas/summarydb/SummaryRepository.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

class SummaryRepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() /
                "veritas_summary_repo_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  // Helper to insert parent rows required for foreign key constraints
  void SetupParentRows(const PublicationContext& context) {
    auto metadata_result = MetadataStore::Open(test_dir_ / "metadata.db");
    ASSERT_TRUE(metadata_result.ok());
    auto metadata = std::move(*metadata_result);

    // Insert repository
    auto repo_status = metadata.Execute(
        "INSERT OR IGNORE INTO repositories (repository_id, vcs_kind, "
        "vcs_revision, source_tree_hash) VALUES (?, ?, ?, ?)",
        {"repo:sha256:abc", "git", "abc123", "hash123"});
    ASSERT_TRUE(repo_status.ok());

    // Insert revision
    auto rev_status = metadata.Execute(
        "INSERT OR IGNORE INTO revisions (revision_id, repository_id, "
        "vcs_revision) VALUES (?, ?, ?)",
        {context.revision_id, "repo:sha256:abc", "def456"});
    ASSERT_TRUE(rev_status.ok());

    // Insert build variant
    auto variant_status = metadata.Execute(
        "INSERT OR IGNORE INTO build_variants (build_variant_id, target_triple, "
        "compiler_id, compiler_version, compile_options_hash, macro_set_hash, "
        "include_closure_hash, type_layout_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        {context.build_variant_id, "arm64-apple-darwin", "clang", "17.0.6",
         "hash1", "hash2", "hash3", "hash4"});
    ASSERT_TRUE(variant_status.ok());
  }

  std::filesystem::path test_dir_;
};

summary::v1::FunctionSummary MakeSyntheticSummary(int64_t range_max = 1024) {
  summary::v1::FunctionSummary summary;

  auto* header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(1234567890);

  auto* identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id("rev:sha256:def");
  identity->set_build_variant_id("variant:sha256:ghi");
  identity->set_function_variant_id("funcvar:sha256:jkl");
  identity->set_function_body_id("funcbody:sha256:mno");

  auto* range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(0);
  range->set_max_value(range_max);
  range->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);

  return summary;
}

TEST_F(SummaryRepositoryTest, PublishAndRetrieveSummary) {
  auto repo_result = SummaryRepository::Open(test_dir_.string());
  ASSERT_TRUE(repo_result.ok()) << repo_result.status().message();
  auto repo = std::move(*repo_result);

  auto summary = MakeSyntheticSummary();

  PublicationContext context;
  context.revision_id = "rev:sha256:def";
  context.build_variant_id = "variant:sha256:ghi";
  context.function_variant_id = "funcvar:sha256:jkl";

  SetupParentRows(context);

  auto publish_result = repo->PublishSummary(summary, context);
  ASSERT_TRUE(publish_result.ok()) << "Error: " << publish_result.status().message();

  auto summary_id = *publish_result;

  // Retrieve by ID
  auto get_result = repo->GetSummary(summary_id);
  ASSERT_TRUE(get_result.ok()) << get_result.status().message();

  auto retrieved = *get_result;
  EXPECT_EQ(retrieved.header().schema_version(), summary.header().schema_version());
  ASSERT_EQ(retrieved.range_facts_size(), 1);
  EXPECT_EQ(retrieved.range_facts(0).variable(), "buffer_size");
  EXPECT_EQ(retrieved.range_facts(0).max_value(), 1024);
}

TEST_F(SummaryRepositoryTest, PublishTwiceIsIdempotent) {
  auto repo_result = SummaryRepository::Open(test_dir_.string());
  ASSERT_TRUE(repo_result.ok());
  auto repo = std::move(*repo_result);

  auto summary = MakeSyntheticSummary();

  PublicationContext context;
  context.revision_id = "rev:sha256:def";
  context.build_variant_id = "variant:sha256:ghi";
  context.function_variant_id = "funcvar:sha256:jkl";

  SetupParentRows(context);

  auto result1 = repo->PublishSummary(summary, context);
  ASSERT_TRUE(result1.ok());

  auto result2 = repo->PublishSummary(summary, context);
  ASSERT_TRUE(result2.ok());

  // Same summary should produce same ID
  EXPECT_EQ(*result1, *result2);
}

TEST_F(SummaryRepositoryTest, NewerSummaryReplacesCurrentBinding) {
  auto repo_result = SummaryRepository::Open(test_dir_.string());
  ASSERT_TRUE(repo_result.ok());
  auto repo = std::move(*repo_result);

  auto summary1 = MakeSyntheticSummary(1024);
  auto summary2 = MakeSyntheticSummary(2048);

  PublicationContext context;
  context.revision_id = "rev:sha256:def";
  context.build_variant_id = "variant:sha256:ghi";
  context.function_variant_id = "funcvar:sha256:jkl";

  SetupParentRows(context);

  auto id1 = repo->PublishSummary(summary1, context);
  ASSERT_TRUE(id1.ok());

  auto id2 = repo->PublishSummary(summary2, context);
  ASSERT_TRUE(id2.ok());

  EXPECT_NE(*id1, *id2);

  // Current binding should point to summary2
  auto current = repo->GetCurrentSummary(context.function_variant_id);
  ASSERT_TRUE(current.ok());
  ASSERT_EQ(current->range_facts_size(), 1);
  EXPECT_EQ(current->range_facts(0).max_value(), 2048);

  // Historical summary1 should still be readable
  auto historical = repo->GetSummary(*id1);
  ASSERT_TRUE(historical.ok());
  ASSERT_EQ(historical->range_facts_size(), 1);
  EXPECT_EQ(historical->range_facts(0).max_value(), 1024);
}

TEST_F(SummaryRepositoryTest, GetCurrentSummaryReturnsNotFoundForUnknownVariant) {
  auto repo_result = SummaryRepository::Open(test_dir_.string());
  ASSERT_TRUE(repo_result.ok());
  auto repo = std::move(*repo_result);

  auto result = repo->GetCurrentSummary("nonexistent:variant");
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace veritas::summarydb
