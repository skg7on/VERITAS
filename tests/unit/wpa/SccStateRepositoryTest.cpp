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

#include "veritas/wpa/SccStateRepository.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

core::StableId FunctionId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(text.data(), text.size())));
}

std::string Hash(char digit) { return std::string(64, digit); }

class SccStateRepositoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 "veritas_scc_state_repository_test";
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
    auto opened = summarydb::MetadataStore::Open(directory_ / "metadata.db");
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    store_ = std::make_unique<summarydb::MetadataStore>(std::move(*opened));
    ASSERT_TRUE(store_->ApplySchema().ok());
    ASSERT_TRUE(store_
                    ->Execute("INSERT INTO repositories(repository_id, "
                              "vcs_kind, vcs_revision, "
                              "source_tree_hash) VALUES(?, ?, ?, ?)",
                              {"repo:test", "git", "r", "tree"})
                    .ok());
    ASSERT_TRUE(store_
                    ->Execute("INSERT INTO revisions(revision_id, "
                              "repository_id, vcs_revision) "
                              "VALUES(?, ?, ?)",
                              {context_.revision_id, "repo:test", "r"})
                    .ok());
    ASSERT_TRUE(
        store_
            ->Execute(
                "INSERT INTO build_variants(build_variant_id, target_triple, "
                "compiler_id, compiler_version, compile_options_hash, "
                "macro_set_hash, "
                "include_closure_hash, type_layout_hash) VALUES(?, ?, ?, ?, ?, "
                "?, ?, ?)",
                {context_.build_variant_id, "arm64", "clang", "24", "a", "b",
                 "c", "d"})
            .ok());
    repository_ = std::make_unique<SccStateRepository>(*store_);

    ASSERT_TRUE(call_graph_.AddFunction(FunctionId("A")).ok());
    auto built = SccGraph::Build(call_graph_);
    ASSERT_TRUE(built.ok());
    scc_graph_ = std::make_unique<SccGraph>(std::move(*built));
    scc_id_ = *scc_graph_->SccForFunction(FunctionId("A"));
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  SccResult Result(std::string input, std::string fixpoint,
                   std::string external, std::size_t iterations) const {
    return SccResult{.scc_id = scc_id_,
                     .component_kind =
                         summary::v1::COMPONENT_KIND_MEMORY_EFFECTS,
                     .input_hash = std::move(input),
                     .fixpoint_hash = std::move(fixpoint),
                     .externally_visible_hash = std::move(external),
                     .iteration_count = iterations,
                     .status = SccStatus::kConverged,
                     .facts = {}};
  }

  std::filesystem::path directory_;
  SccContext context_{.revision_id = "rev:test", .build_variant_id = "bv:test"};
  std::unique_ptr<summarydb::MetadataStore> store_;
  std::unique_ptr<SccStateRepository> repository_;
  CallGraph call_graph_;
  std::unique_ptr<SccGraph> scc_graph_;
  core::StableId scc_id_;
};

TEST_F(SccStateRepositoryTest, PersistsAndReloadsAllConvergenceFields) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  const SccResult result = Result(Hash('a'), Hash('b'), Hash('c'), 3);
  auto change = repository_->StoreState(context_, result);
  ASSERT_TRUE(change.ok()) << change.status().message();
  EXPECT_EQ(*change, ExternalChange::kChanged);

  auto loaded =
      repository_->LoadState(context_, result.scc_id, result.component_kind);
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->input_hash, Hash('a'));
  EXPECT_EQ((*loaded)->fixpoint_hash, Hash('b'));
  EXPECT_EQ((*loaded)->externally_visible_hash, Hash('c'));
  EXPECT_EQ((*loaded)->iteration_count, 3u);
  EXPECT_EQ((*loaded)->status, SccStatus::kConverged);
}

TEST_F(SccStateRepositoryTest, InternalOnlyChangeDoesNotPropagate) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  auto initial_change = repository_->StoreState(
      context_, Result(Hash('a'), Hash('b'), Hash('c'), 1));
  ASSERT_TRUE(initial_change.ok()) << initial_change.status().message();
  ASSERT_EQ(*initial_change, ExternalChange::kChanged);
  auto change = repository_->StoreState(
      context_, Result(Hash('d'), Hash('e'), Hash('c'), 2));
  ASSERT_TRUE(change.ok());
  EXPECT_EQ(*change, ExternalChange::kUnchanged);
  auto loaded = repository_->LoadState(
      context_, scc_id_, summary::v1::COMPONENT_KIND_MEMORY_EFFECTS);
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->input_hash, Hash('d'));
  EXPECT_EQ((*loaded)->fixpoint_hash, Hash('e'));
}

TEST_F(SccStateRepositoryTest,
       RepublishingUnchangedGraphPreservesConvergenceState) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  const SccResult result = Result(Hash('a'), Hash('b'), Hash('c'), 1);
  auto initial_change = repository_->StoreState(context_, result);
  ASSERT_TRUE(initial_change.ok()) << initial_change.status().message();
  ASSERT_EQ(*initial_change, ExternalChange::kChanged);

  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  auto repeated_change = repository_->StoreState(context_, result);
  ASSERT_TRUE(repeated_change.ok()) << repeated_change.status().message();
  EXPECT_EQ(*repeated_change, ExternalChange::kUnchanged);
}

TEST_F(SccStateRepositoryTest, RejectsStateOutsidePublishedTopology) {
  auto change = repository_->StoreState(
      context_, Result(Hash('a'), Hash('b'), Hash('c'), 1));
  ASSERT_FALSE(change.ok());
  EXPECT_EQ(change.status().code(), StatusCode::kNotFound);
}

TEST_F(SccStateRepositoryTest, RejectsUnsupportedOrMalformedResults) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  auto unsupported = Result(Hash('a'), Hash('b'), Hash('c'), 1);
  unsupported.status = SccStatus::kUnsupported;
  EXPECT_EQ(repository_->StoreState(context_, unsupported).status().code(),
            StatusCode::kInvalidArgument);
  auto malformed = Result("", Hash('b'), Hash('c'), 1);
  EXPECT_EQ(repository_->StoreState(context_, malformed).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(SccStateRepositoryTest, RejectsHashesThatAreNotLowercaseSha256Hex) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());

  auto short_input = Result(Hash('a'), Hash('b'), Hash('c'), 1);
  short_input.input_hash.pop_back();
  EXPECT_EQ(repository_->StoreState(context_, short_input).status().code(),
            StatusCode::kInvalidArgument);

  auto long_input = Result(Hash('a'), Hash('b'), Hash('c'), 1);
  long_input.input_hash.push_back('a');
  EXPECT_EQ(repository_->StoreState(context_, long_input).status().code(),
            StatusCode::kInvalidArgument);

  auto uppercase_fixpoint = Result(Hash('a'), Hash('A'), Hash('c'), 1);
  EXPECT_EQ(
      repository_->StoreState(context_, uppercase_fixpoint).status().code(),
      StatusCode::kInvalidArgument);

  auto non_hex_external = Result(Hash('a'), Hash('b'), Hash('g'), 1);
  EXPECT_EQ(repository_->StoreState(context_, non_hex_external).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(SccStateRepositoryTest, CommitFailureRollsBackAndLeavesStoreUsable) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  ASSERT_TRUE(
      store_->Execute("CREATE TABLE commit_parent(id TEXT PRIMARY KEY)", {})
          .ok());
  ASSERT_TRUE(
      store_
          ->Execute(
              "CREATE TABLE commit_child(parent_id TEXT, FOREIGN "
              "KEY(parent_id) "
              "REFERENCES commit_parent(id) DEFERRABLE INITIALLY DEFERRED)",
              {})
          .ok());
  ASSERT_TRUE(store_
                  ->Execute("CREATE TRIGGER fail_wpa_commit AFTER INSERT ON "
                            "wpa_component_states "
                            "BEGIN INSERT INTO commit_child(parent_id) "
                            "VALUES('missing'); END",
                            {})
                  .ok());

  auto change = repository_->StoreState(
      context_, Result(Hash('a'), Hash('b'), Hash('c'), 1));
  ASSERT_FALSE(change.ok());
  EXPECT_TRUE(store_->BeginTransaction().ok());
  EXPECT_TRUE(store_->RollbackTransaction().ok());
  auto loaded = repository_->LoadState(
      context_, scc_id_, summary::v1::COMPONENT_KIND_MEMORY_EFFECTS);
  ASSERT_TRUE(loaded.ok());
  EXPECT_FALSE(loaded->has_value());
}

} // namespace
} // namespace veritas::wpa
