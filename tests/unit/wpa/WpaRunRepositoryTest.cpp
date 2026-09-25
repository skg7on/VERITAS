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

#include "veritas/wpa/WpaRunRepository.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor d;
  d.revision_id = core::MakeStableId(core::IdKind::kRevision,
                                     std::as_bytes(std::span("rev", 3)));
  d.build_variant_id = core::MakeStableId(core::IdKind::kBuildVariant,
                                          std::as_bytes(std::span("bv", 2)));
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'b');
  d.engine = engine;
  d.engine_toolchain_identity = "test-toolchain";
  return std::move(facts::MakeAnalysisRun(d)).value();
}

facts::AnalysisFact ReachableFact(std::string_view from, std::string_view to) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kReachableCall;
  row.cells = {FunctionId(from), FunctionId(to), sem::EpistemicState::kMust};
  return std::move(facts::MakeFact(row)).value();
}

WpaComponentResult ResultFor(std::string_view scc_name) {
  WpaComponentResult result;
  result.scc_id = FunctionId(scc_name);
  result.component = WpaComponentKind::kReachability;
  result.logical_input_hash = "logical";
  result.facts = {ReachableFact("f", "g")};
  const auto hashes =
      facts::ComputeCanonicalResultHashes(result.facts, result.witnesses);
  result.fixpoint_hash = hashes.fixpoint_hash;
  result.external_hash = hashes.external_hash;
  return result;
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

WpaComponentKey ReachabilityKey(std::string_view scc_name) {
  return WpaComponentKey{FunctionId(scc_name),
                         WpaComponentKind::kReachability};
}

// Whether the store currently exposes a reusable result for `name`. A cache row
// is visible only once the batch that holds it has been committed, so this is
// how the tests read the batch window.
bool IsComponentReusable(WpaRunRepository& repo,
                         const facts::AnalysisRunManifest& run,
                         std::string_view name) {
  auto loaded = repo.LoadReusableComponent(
      MakeResultCacheDescriptor(run, ReachabilityKey(name), "logical"));
  if (!loaded.ok() || !loaded->has_value()) {
    return false;
  }
  return true;
}

// Whether the store currently exposes the run's own state row for `name`.
bool HasComponentState(WpaRunRepository& repo,
                       const facts::AnalysisRunManifest& run,
                       std::string_view name) {
  auto object_key = repo.ResultObjectKey(run.run_id, ReachabilityKey(name));
  if (!object_key.ok() || !object_key->has_value()) {
    return false;
  }
  return true;
}

std::size_t CountRows(WpaRunRepository& repo, const std::string& sql,
                      const std::vector<std::string>& params) {
  auto rows = repo.metadata_store().Query(sql, params);
  if (!rows.ok() || rows->empty() || (*rows)[0].empty()) {
    return std::numeric_limits<std::size_t>::max();
  }
  std::size_t parsed = 0;
  const std::string& text = (*rows)[0][0];
  const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                      parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::numeric_limits<std::size_t>::max();
  }
  return parsed;
}

std::size_t VisibleCacheRowCount(WpaRunRepository& repo) {
  return CountRows(repo, "SELECT COUNT(*) FROM wpa_component_result_cache_v2",
                   {});
}

// The number of components one commit covers, named here because the boundary
// test stores exactly this many.
constexpr std::size_t kComponentBatchSize =
    WpaRunRepository::kComponentCacheBatchSize;

TEST(WpaRunRepositoryTest, StoresAndLoadsAComponentResult) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  const WpaComponentKey key{FunctionId("f"), WpaComponentKind::kReachability};
  const WpaComponentResult result = ResultFor("f");
  auto stored = repo->StoreSuccessfulComponent(run, key, result);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored->key, key);

  // A store queues its cache and state rows; the batch flush is what commits
  // them, so the result is loadable once the batch has been flushed.
  ASSERT_TRUE(repo->FlushComponentCache().ok());

  auto loaded = repo->LoadReusableComponent(
      MakeResultCacheDescriptor(run, key, "logical"));
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().facts, result.facts);
  EXPECT_EQ(loaded->value().external_hash, result.external_hash);
  EXPECT_EQ(loaded->value().scc_id, key.scc_id);

  std::filesystem::remove_all(db);
}

// The component cache is a cache: a row costs a recomputation when it is lost
// and nothing else. A run stores 13,716 of them, so the repository commits them
// a batch at a time rather than paying a SQLite commit — a durability barrier —
// once per component. This test pins the batch window itself: a stored
// component is not visible until a commit covers it, and the flush commits the
// whole batch.
TEST(WpaRunRepositoryTest, AComponentIsVisibleOnlyAfterTheBatchCommits) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  const std::vector<std::string> names = {"a", "b", "c"};
  for (const std::string& name : names) {
    ASSERT_TRUE(repo
                    ->StoreSuccessfulComponent(run, ReachabilityKey(name),
                                               ResultFor(name))
                    .ok());
  }

  // Queued, not committed: neither the cache row nor the run's state row is
  // readable yet.
  for (const std::string& name : names) {
    EXPECT_FALSE(IsComponentReusable(*repo, run, name));
    EXPECT_FALSE(HasComponentState(*repo, run, name));
  }
  EXPECT_EQ(VisibleCacheRowCount(*repo), 0u);

  ASSERT_TRUE(repo->FlushComponentCache().ok());

  // The flush commits the whole batch, and every row it commits points at a
  // result object the store already held — so a visible cache row is always
  // loadable, never a dangling reference.
  for (const std::string& name : names) {
    EXPECT_TRUE(IsComponentReusable(*repo, run, name));
    EXPECT_TRUE(HasComponentState(*repo, run, name));
  }
  EXPECT_EQ(VisibleCacheRowCount(*repo), names.size());

  std::filesystem::remove_all(db);
}

// The batch window is bounded. A batch commits as soon as it holds
// `kComponentCacheBatchSize` components, so a crash can never cost more than
// one batch, and the next store starts a fresh window rather than joining a
// batch that has already been committed.
TEST(WpaRunRepositoryTest, AFullBatchCommitsWithoutAnExplicitFlush) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  for (std::size_t i = 0; i < kComponentBatchSize; ++i) {
    const std::string name = "scc-" + std::to_string(i);
    ASSERT_TRUE(
        repo->StoreSuccessfulComponent(run, ReachabilityKey(name),
                                       ResultFor(name))
            .ok());
  }
  // Filling the batch committed it; nothing else has been stored since.
  EXPECT_TRUE(IsComponentReusable(*repo, run, "scc-0"));
  const std::string last = "scc-" + std::to_string(kComponentBatchSize - 1);
  EXPECT_TRUE(IsComponentReusable(*repo, run, last));
  EXPECT_EQ(VisibleCacheRowCount(*repo), kComponentBatchSize);

  // One more store opens the next window and is not committed with the last.
  ASSERT_TRUE(repo
                  ->StoreSuccessfulComponent(run, ReachabilityKey("overflow"),
                                             ResultFor("overflow"))
                  .ok());
  EXPECT_FALSE(IsComponentReusable(*repo, run, "overflow"));
  EXPECT_EQ(VisibleCacheRowCount(*repo), kComponentBatchSize);

  ASSERT_TRUE(repo->FlushComponentCache().ok());
  EXPECT_TRUE(IsComponentReusable(*repo, run, "overflow"));
  EXPECT_EQ(VisibleCacheRowCount(*repo), kComponentBatchSize + 1);

  std::filesystem::remove_all(db);
}

// A completed run's whole cache is durable by the time `Run` returns: the final
// flush is inside `CompleteRun`. Assembly reloads component results from this
// store after `Run` returns, so a component left queued here would be invisible
// to it.
TEST(WpaRunRepositoryTest, CompletedRunLeavesEveryComponentLoadable) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  const std::vector<std::string> names = {"a", "b", "c"};
  for (const std::string& name : names) {
    ASSERT_TRUE(repo
                    ->StoreSuccessfulComponent(run, ReachabilityKey(name),
                                               ResultFor(name))
                    .ok());
  }

  ASSERT_TRUE(repo->CompleteRun(run).ok());

  auto status = repo->RunStatus(run.run_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, WpaRunStatus::kComplete);
  for (const std::string& name : names) {
    EXPECT_TRUE(IsComponentReusable(*repo, run, name));
    EXPECT_TRUE(HasComponentState(*repo, run, name));
  }

  std::filesystem::remove_all(db);
}

// A run abandoned before its final flush leaves no half-written batch behind:
// the queued rows are never committed, so the store holds neither a cache row
// without its state row nor a state row without its cache row, and the run is
// marked incomplete. The cost of the loss is the recomputation the next run
// performs, which is exactly what a cache miss costs.
TEST(WpaRunRepositoryTest, AnIncompleteRunCommitsNoHalfWrittenBatch) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  const std::vector<std::string> names = {"a", "b", "c"};
  for (const std::string& name : names) {
    ASSERT_TRUE(repo
                    ->StoreSuccessfulComponent(run, ReachabilityKey(name),
                                               ResultFor(name))
                    .ok());
  }
  ASSERT_TRUE(repo->MarkIncomplete(run).ok());

  auto status = repo->RunStatus(run.run_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, WpaRunStatus::kIncomplete);
  for (const std::string& name : names) {
    EXPECT_FALSE(IsComponentReusable(*repo, run, name));
    EXPECT_FALSE(HasComponentState(*repo, run, name));
  }
  EXPECT_EQ(VisibleCacheRowCount(*repo), 0u);
  EXPECT_EQ(CountRows(*repo,
                      "SELECT COUNT(*) FROM wpa_component_states_v2 "
                      "WHERE run_id = ?",
                      {core::ToString(run.run_id)}),
            0u);

  std::filesystem::remove_all(db);
}

// One commit covers the batch, so a row that cannot be written discards the
// whole batch rather than leaving the cache table ahead of the run-state table.
// `wpa_component_states_v2` carries a foreign key to `wpa_analysis_runs`, so a
// component stored for a run that was never begun fails its state insert after
// the batch's cache rows have already been written inside the same transaction
// — and those cache rows must not survive the rollback.
TEST(WpaRunRepositoryTest, AFailedBatchFlushLeavesTheStoreUnchanged) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  // Deliberately no BeginRun: the run row this batch's state rows reference
  // does not exist.

  const std::vector<std::string> names = {"a", "b", "c"};
  for (const std::string& name : names) {
    ASSERT_TRUE(repo
                    ->StoreSuccessfulComponent(run, ReachabilityKey(name),
                                               ResultFor(name))
                    .ok());
  }

  auto flushed = repo->FlushComponentCache();
  EXPECT_FALSE(flushed.ok());

  // The failed batch is rolled back whole, and abandoned rather than retried:
  // the run that owned it fails and a later run recomputes these components.
  for (const std::string& name : names) {
    EXPECT_FALSE(IsComponentReusable(*repo, run, name));
    EXPECT_FALSE(HasComponentState(*repo, run, name));
  }
  EXPECT_EQ(VisibleCacheRowCount(*repo), 0u);
  EXPECT_TRUE(repo->FlushComponentCache().ok());

  std::filesystem::remove_all(db);
}

TEST(WpaRunRepositoryTest, FailureMarksRunIncomplete) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());
  ASSERT_TRUE(repo->MarkIncomplete(run).ok());

  auto status = repo->RunStatus(run.run_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, WpaRunStatus::kIncomplete);

  std::filesystem::remove_all(db);
}

TEST(WpaRunRepositoryTest, ReusesUnchangedResultAcrossRevisions) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const WpaComponentKey key{FunctionId("f"), WpaComponentKind::kReachability};
  const WpaComponentResult result = ResultFor("f");

  // Two runs with different revisions but the same logical input and toolchain.
  auto run1 = MakeManifest(facts::EngineIdentity::kSouffle);
  auto run2 = MakeManifest(facts::EngineIdentity::kSouffle);
  run2.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev2", 4)));

  ASSERT_TRUE(repo->BeginRun(run1).ok());
  auto stored1 = repo->StoreSuccessfulComponent(run1, key, result);
  ASSERT_TRUE(stored1.ok());

  ASSERT_TRUE(repo->BeginRun(run2).ok());
  auto stored2 = repo->StoreSuccessfulComponent(run2, key, result);
  ASSERT_TRUE(stored2.ok());

  // The cache key ignores the revision, so both point at the same object.
  EXPECT_EQ(stored1->result_object_key, stored2->result_object_key);

  std::filesystem::remove_all(db);
}

}  // namespace
}  // namespace veritas::wpa
