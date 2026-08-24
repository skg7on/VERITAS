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
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/summary/SummaryArtifact.h"

namespace veritas::summarydb {
namespace {

namespace v1 = veritas::summary::v1;
namespace v2 = veritas::summary::v2;

constexpr const char *kRevisionId = "rev:sha256:def";
constexpr const char *kBuildVariantId = "variant:sha256:ghi";
constexpr const char *kFunctionVariantId = "funcvar:sha256:jkl";

PublicationContext Context() {
  PublicationContext context;
  context.revision_id = kRevisionId;
  context.build_variant_id = kBuildVariantId;
  context.function_variant_id = kFunctionVariantId;
  return context;
}

std::string FunctionIdText() { return kFunctionVariantId; }

summary::v1::FunctionSummary V1Summary() {
  summary::v1::FunctionSummary summary;

  auto *header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(1234567890);

  auto *identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id(kRevisionId);
  identity->set_build_variant_id(kBuildVariantId);
  identity->set_function_variant_id(kFunctionVariantId);
  identity->set_function_body_id("funcbody:sha256:mno");

  auto *range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(0);
  range->set_max_value(1024);
  range->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);

  return summary;
}

summary::v2::FunctionSummary V2Summary() {
  summary::v2::FunctionSummary summary;

  auto *header = summary.mutable_header();
  header->set_schema_version("summary.v2");
  header->set_creation_epoch_ms(1234567890);

  auto *identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id(kRevisionId);
  identity->set_build_variant_id(kBuildVariantId);
  identity->set_function_variant_id(kFunctionVariantId);
  identity->set_function_body_id("funcbody:sha256:mno");

  auto *range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(0);
  range->set_max_value(1024);
  range->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);

  return summary;
}

// Open a fresh repository in a unique temp directory and seed the parent rows
// (repository, revision, build variant) required by the summary_bindings
// foreign keys so PublishSummary succeeds.
std::unique_ptr<SummaryRepository> OpenRepository() {
  static int counter = 0;
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("veritas_summary_version_test_" + std::to_string(::getpid()) + "_" +
       std::to_string(counter++));
  std::filesystem::create_directories(dir);

  auto repo_result = SummaryRepository::Open(dir.string());
  if (!repo_result.ok()) {
    return nullptr;
  }
  auto repo = std::move(*repo_result);

  MetadataStore &metadata = repo->metadata_store();
  bool ok = metadata
                .Execute("INSERT OR IGNORE INTO repositories (repository_id, "
                         "vcs_kind, vcs_revision, source_tree_hash) "
                         "VALUES (?, ?, ?, ?)",
                         {"repo:sha256:abc", "git", "abc123", "hash123"})
                .ok();
  ok = ok && metadata
                 .Execute("INSERT OR IGNORE INTO revisions (revision_id, "
                          "repository_id, vcs_revision) VALUES (?, ?, ?)",
                          {kRevisionId, "repo:sha256:abc", "def456"})
                 .ok();
  ok = ok && metadata
                 .Execute("INSERT OR IGNORE INTO build_variants "
                          "(build_variant_id, target_triple, compiler_id, "
                          "compiler_version, compile_options_hash, "
                          "macro_set_hash, include_closure_hash, "
                          "type_layout_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                          {kBuildVariantId, "arm64-apple-darwin", "clang",
                           "17.0.6", "hash1", "hash2", "hash3", "hash4"})
                 .ok();
  if (!ok) {
    return nullptr;
  }
  return repo;
}

}  // namespace

TEST(SummaryVersionCompatibilityTest, ReadsHistoricalV1AfterPublishingV2) {
  auto repository = OpenRepository();
  ASSERT_NE(repository, nullptr);

  auto v1_id = repository->PublishSummary(V1Summary(), Context());
  ASSERT_TRUE(v1_id.ok());

  auto v2_id = repository->PublishSummary(V2Summary(), Context());
  ASSERT_TRUE(v2_id.ok());
  EXPECT_NE(*v1_id, *v2_id);

  auto historical = repository->GetSummaryArtifact(*v1_id);
  ASSERT_TRUE(historical.ok());
  EXPECT_TRUE(std::holds_alternative<v1::FunctionSummary>(*historical));

  auto current = repository->GetCurrentSummaryArtifact(FunctionIdText());
  ASSERT_TRUE(current.ok());
  EXPECT_TRUE(std::holds_alternative<v2::FunctionSummary>(*current));
}

TEST(SummaryVersionCompatibilityTest, ReadsV2ArtifactWrittenWithoutMetadata) {
  auto repository = OpenRepository();
  ASSERT_NE(repository, nullptr);

  std::vector<summary::SummaryArtifact> artifacts;
  artifacts.emplace_back(V2Summary());
  auto ids = repository->PutImmutableSummaryArtifacts(artifacts);
  ASSERT_TRUE(ids.ok()) << ids.status().message();
  ASSERT_EQ(ids->size(), 1u);

  // A V2 object written through the CAS-only path (no summary_objects row)
  // must round-trip as V2, not be mis-parsed as V1.
  auto artifact = repository->GetSummaryArtifact((*ids)[0]);
  ASSERT_TRUE(artifact.ok()) << artifact.status().message();
  EXPECT_TRUE(std::holds_alternative<v2::FunctionSummary>(*artifact));

  // The V1 getter must reject it rather than reinterpret V2 bytes as V1.
  auto v1 = repository->GetSummary((*ids)[0]);
  ASSERT_FALSE(v1.ok());
  EXPECT_EQ(v1.status().code(), StatusCode::kFailedPrecondition);
}

}  // namespace veritas::summarydb
