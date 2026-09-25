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

#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

core::StableId FunctionId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(text.data(), text.size())));
}

std::string Hash(char digit) { return std::string(64, digit); }

// How many convergence-state rows the store currently exposes. Only committed
// rows are visible, so this is how a test reads the batch window.
std::size_t VisibleStateRowCount(summarydb::MetadataStore& store) {
  auto rows = store.Query("SELECT COUNT(*) FROM wpa_component_states", {});
  if (!rows.ok() || rows->empty() || (*rows)[0].empty()) {
    return std::numeric_limits<std::size_t>::max();
  }
  std::size_t parsed = 0;
  const std::string& text = (*rows)[0][0];
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::numeric_limits<std::size_t>::max();
  }
  return parsed;
}

class SccStateRepositoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    // One directory per case, so a parallel `ctest -j` run of this binary's
    // cases cannot have one case's teardown delete another's store mid-schema.
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    directory_ = std::filesystem::temp_directory_path() /
                 ("veritas_scc_state_repository_test_" +
                  std::string(info->name()));
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
                     .status = SccStatus::kConverged};
  }

  // A converged result for an arbitrary SCC, so a test can store more than the
  // one key the fixture's call graph produces.
  SccResult ResultFor(core::StableId scc_id, char external) const {
    return SccResult{.scc_id = std::move(scc_id),
                     .component_kind =
                         summary::v1::COMPONENT_KIND_MEMORY_EFFECTS,
                     .input_hash = Hash('a'),
                     .fixpoint_hash = Hash('b'),
                     .externally_visible_hash = Hash(external),
                     .iteration_count = 1,
                     .status = SccStatus::kConverged};
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

  // A store queues its row; the batch flush is what commits it, so the row is
  // readable once the batch has been flushed.
  ASSERT_TRUE(repository_->FlushStateCache().ok());

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

// The convergence-state rows are committed a batch at a time rather than one
// commit per component, because a run stores one per component — 13,716 of
// them — and a SQLite commit is a durability barrier. This test pins the
// window: a row `StoreState` has stored is not visible to a reader until a
// commit covers it. It reads through a second connection to the same database,
// so it observes exactly what any other reader of the store — the next
// incremental run included — observes.
TEST_F(SccStateRepositoryTest, AStoredStateRowIsNotVisibleBeforeABatchCommits) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());

  auto observer_store =
      summarydb::MetadataStore::Open(directory_ / "metadata.db");
  ASSERT_TRUE(observer_store.ok()) << observer_store.status().message();
  SccStateRepository observer(*observer_store);

  const SccResult result = Result(Hash('a'), Hash('b'), Hash('c'), 3);
  auto change = repository_->StoreState(context_, result);
  ASSERT_TRUE(change.ok()) << change.status().message();
  EXPECT_EQ(*change, ExternalChange::kChanged);

  // Stored, but queued rather than committed: no reader sees it yet.
  auto observer_view =
      observer.LoadState(context_, result.scc_id, result.component_kind);
  ASSERT_TRUE(observer_view.ok());
  EXPECT_FALSE(observer_view->has_value());
  EXPECT_EQ(VisibleStateRowCount(*observer_store), 0u);
}

// The flush commits the whole batch in one transaction, so every queued row
// becomes readable at once, and a flush with nothing queued commits nothing.
TEST_F(SccStateRepositoryTest, FlushCommitsTheWholeQueuedBatch) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  auto observer_store =
      summarydb::MetadataStore::Open(directory_ / "metadata.db");
  ASSERT_TRUE(observer_store.ok()) << observer_store.status().message();
  SccStateRepository observer(*observer_store);

  // Nothing is queued yet, so a flush has nothing to commit.
  ASSERT_TRUE(repository_->FlushStateCache().ok());
  EXPECT_EQ(VisibleStateRowCount(*observer_store), 0u);

  // Three distinct keys of the one SCC the fixture builds.
  const std::array<summary::v1::ComponentKind, 3> kinds = {
      summary::v1::COMPONENT_KIND_CALLS,
      summary::v1::COMPONENT_KIND_MEMORY_EFFECTS,
      summary::v1::COMPONENT_KIND_VALUE_FLOW};
  for (const auto kind : kinds) {
    SccResult result = Result(Hash('a'), Hash('b'), Hash('c'), 3);
    result.component_kind = kind;
    auto change = repository_->StoreState(context_, result);
    ASSERT_TRUE(change.ok()) << change.status().message();
    EXPECT_EQ(*change, ExternalChange::kChanged);
  }
  EXPECT_EQ(VisibleStateRowCount(*observer_store), 0u);

  ASSERT_TRUE(repository_->FlushStateCache().ok());

  // One commit covered all three: each row is readable and carries the
  // convergence fields it was stored with.
  EXPECT_EQ(VisibleStateRowCount(*observer_store), kinds.size());
  for (const auto kind : kinds) {
    auto view = observer.LoadState(context_, scc_id_, kind);
    ASSERT_TRUE(view.ok());
    ASSERT_TRUE(view->has_value());
    EXPECT_EQ((*view)->input_hash, Hash('a'));
    EXPECT_EQ((*view)->fixpoint_hash, Hash('b'));
    EXPECT_EQ((*view)->externally_visible_hash, Hash('c'));
    EXPECT_EQ((*view)->iteration_count, 3u);
    EXPECT_EQ((*view)->status, SccStatus::kConverged);
  }

  // The batch was drained rather than left queued: a second flush has nothing
  // to commit and does not add a row.
  ASSERT_TRUE(repository_->FlushStateCache().ok());
  EXPECT_EQ(VisibleStateRowCount(*observer_store), kinds.size());
}

// The batch window is bounded. A batch commits as soon as it holds
// `kStateBatchSize` rows, so a crash can never cost more than one batch, and
// the next store starts a fresh window rather than joining a committed one.
TEST_F(SccStateRepositoryTest, AFullBatchCommitsWithoutAnExplicitFlush) {
  // One function per SCC, so every state row below is a distinct key.
  constexpr std::size_t kBatch = SccStateRepository::kStateBatchSize;
  CallGraph graph;
  std::vector<core::StableId> functions;
  functions.reserve(kBatch);
  for (std::size_t i = 0; i < kBatch; ++i) {
    const auto id = FunctionId("F" + std::to_string(i));
    ASSERT_TRUE(graph.AddFunction(id).ok());
    functions.push_back(id);
  }
  auto built = SccGraph::Build(graph);
  ASSERT_TRUE(built.ok()) << built.status().message();
  ASSERT_TRUE(repository_->PublishGraph(context_, graph, *built).ok());

  for (std::size_t i = 0; i < kBatch - 1; ++i) {
    const auto scc_id = built->SccForFunction(functions[i]);
    ASSERT_TRUE(scc_id.ok());
    auto change = repository_->StoreState(context_, ResultFor(*scc_id, 'c'));
    ASSERT_TRUE(change.ok()) << change.status().message();
  }
  // One short of a full batch: still queued, nothing committed.
  EXPECT_EQ(VisibleStateRowCount(*store_), 0u);

  // The store that fills the batch commits it without a caller-supplied flush.
  const auto last_scc = built->SccForFunction(functions[kBatch - 1]);
  ASSERT_TRUE(last_scc.ok());
  auto change = repository_->StoreState(context_, ResultFor(*last_scc, 'c'));
  ASSERT_TRUE(change.ok()) << change.status().message();
  EXPECT_EQ(VisibleStateRowCount(*store_), kBatch);
}

TEST_F(SccStateRepositoryTest, InternalOnlyChangeDoesNotPropagate) {
  ASSERT_TRUE(
      repository_->PublishGraph(context_, call_graph_, *scc_graph_).ok());
  auto initial_change = repository_->StoreState(
      context_, Result(Hash('a'), Hash('b'), Hash('c'), 1));
  ASSERT_TRUE(initial_change.ok()) << initial_change.status().message();
  ASSERT_EQ(*initial_change, ExternalChange::kChanged);
  // A run flushes its last batch before it completes, so the second store here
  // stands for a second run and sees the first run's committed row.
  ASSERT_TRUE(repository_->FlushStateCache().ok());
  auto change = repository_->StoreState(
      context_, Result(Hash('d'), Hash('e'), Hash('c'), 2));
  ASSERT_TRUE(change.ok());
  EXPECT_EQ(*change, ExternalChange::kUnchanged);
  ASSERT_TRUE(repository_->FlushStateCache().ok());
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

  // The first run flushed its last batch before it completed; the second run
  // republishes the same graph and re-stores the same component.
  ASSERT_TRUE(repository_->FlushStateCache().ok());
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

  // The store only queues its row, so the failing statement is the flush's.
  auto change = repository_->StoreState(
      context_, Result(Hash('a'), Hash('b'), Hash('c'), 1));
  ASSERT_TRUE(change.ok()) << change.status().message();
  auto flushed = repository_->FlushStateCache();
  ASSERT_FALSE(flushed.ok());

  // The failed flush rolled its whole batch back and cleared it, so with the
  // failing trigger gone a later flush has nothing to write: had the batch
  // survived, this flush would commit the row it was holding.
  ASSERT_TRUE(store_->Execute("DROP TRIGGER fail_wpa_commit", {}).ok());
  ASSERT_TRUE(repository_->FlushStateCache().ok());
  EXPECT_TRUE(store_->BeginTransaction().ok());
  EXPECT_TRUE(store_->RollbackTransaction().ok());
  auto loaded = repository_->LoadState(
      context_, scc_id_, summary::v1::COMPONENT_KIND_MEMORY_EFFECTS);
  ASSERT_TRUE(loaded.ok());
  EXPECT_FALSE(loaded->has_value());
}

} // namespace
} // namespace veritas::wpa
