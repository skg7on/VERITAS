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

#include "veritas/evidence/FactStoreEvidenceBackend.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

using namespace veritas;
using namespace veritas::facts;

namespace {

core::StableId Id(core::IdKind kind, std::string_view seed) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(seed.data(), seed.size())));
}

AnalysisFact Reachable(std::string_view from, std::string_view to) {
  return MakeFact({RelationId::kReachableCall,
                   {Id(core::IdKind::kFunctionVariant, from),
                    Id(core::IdKind::kFunctionVariant, to),
                    semantic::EpistemicState::kMay}})
      .value();
}

CellEquality Function(std::uint32_t column, std::string_view name) {
  return {column, ColumnDomain::kFunctionId,
          Id(core::IdKind::kFunctionVariant, name)};
}

class FactPageTest : public testing::Test {
protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "veritas-factpage-XXXXXX")
            .string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    path = pattern;
    auto opened = FactStore::Open(path);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    store.emplace(std::move(*opened));
    batch.run.run_id = Id(core::IdKind::kAnalysisRun, "run");
    batch.batch_id = Id(core::IdKind::kFact, "batch");
    batch.facts = {Reachable("fixed", "one"), Reachable("fixed", "two")};
  }

  void TearDown() override {
    store.reset();
    std::filesystem::remove_all(path);
  }

  StatusOr<std::unique_ptr<evidence::EvidenceReadSnapshot>> Snapshot() {
    evidence::SnapshotDescriptor descriptor{};
    descriptor.analysis_run_id = batch.run.run_id;
    return evidence::FactStoreEvidenceBackend(*store, descriptor)
        .OpenSnapshot(batch.run.run_id);
  }

  std::filesystem::path path;
  std::optional<FactStore> store;
  AnalysisFactBatch batch{};
  FactSelector selector{RelationId::kReachableCall, {Function(0, "fixed")}};
};

TEST_F(FactPageTest, ExactLimitDoesNotMeanTruncated) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot();
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  PageRequest request;
  request.limit = 2;
  auto page = (*snapshot)->ReadFacts(selector, request);
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_TRUE(page->exhausted);
  EXPECT_FALSE(page->continuation);
  EXPECT_EQ(page->examined, 2u);
  std::ranges::sort(batch.facts);
  EXPECT_EQ(page->facts, batch.facts);
}

TEST_F(FactPageTest, LookaheadRemainsEligibleAndIsCharged) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.limit = 1;
  const auto first = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_EQ(first->facts.size(), 1u);
  EXPECT_EQ(first->examined, 2u);
  EXPECT_FALSE(first->exhausted);
  EXPECT_FALSE(first->work_exhausted);
  ASSERT_TRUE(first->continuation);
  EXPECT_EQ(first->continuation->last_consumed, first->facts.front().fact_id);
  request.after = first->continuation;
  const auto second = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_TRUE(second->exhausted);
  EXPECT_EQ(second->examined, 1u);
  ASSERT_EQ(second->facts.size(), 1u);
  std::ranges::sort(batch.facts);
  EXPECT_EQ(first->facts.front(), batch.facts[0]);
  EXPECT_EQ(second->facts.front(), batch.facts[1]);
}

TEST_F(FactPageTest, RejectsCursorFromAnotherSnapshotOrSelector) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.limit = 1;
  const auto first = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(first->continuation);
  request.after = first->continuation;
  const FactSelector unbound{selector.relation, {}};
  EXPECT_EQ(snapshot->ReadFacts(unbound, request).status().code(),
            StatusCode::kInvalidArgument);
  ++request.after->ordering_version;
  EXPECT_EQ(snapshot->ReadFacts(selector, request).status().code(),
            StatusCode::kInvalidArgument);
  request.after = first->continuation;
  snapshot.reset();
  batch.batch_id = Id(core::IdKind::kFact, "next-batch");
  ASSERT_TRUE(store->Publish(batch).ok());
  snapshot = Snapshot().value();
  EXPECT_EQ(snapshot->ReadFacts(selector, request).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(FactPageTest, UnrelatedRowsDoNotIncreaseExaminedCount) {
  for (int i = 0; i < 10; ++i)
    batch.facts.push_back(Reachable("unrelated", std::to_string(i)));
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  auto before = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(before.ok()) << before.status().message();
  ASSERT_EQ(before->facts.size(), 2u);
  EXPECT_EQ(before->examined, 2u);
  snapshot.reset();
  batch.facts.clear();
  for (int i = 10; i < 100; ++i)
    batch.facts.push_back(Reachable("unrelated", std::to_string(i)));
  batch.batch_id = Id(core::IdKind::kFact, "10x");
  ASSERT_TRUE(store->Publish(batch).ok());
  snapshot = Snapshot().value();
  const auto after = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(after.ok()) << after.status().message();
  EXPECT_EQ(after->facts, before->facts);
  EXPECT_EQ(after->examined, before->examined);
  EXPECT_TRUE(after->exhausted);
  // Both unbound and bound streams are seeks in the covering primary key.
  auto plan = store->metadata_store().Query(
      "EXPLAIN QUERY PLAN SELECT fact_id FROM fact_lookup.fact_lookup_v1 "
      "WHERE run_id = ? AND relation_id = ? AND column_index = ? "
      "AND value_digest = ? AND fact_id > ? ORDER BY fact_id LIMIT 1",
      {core::ToString(batch.run.run_id), "ReachableCall", "-1", "", ""});
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  ASSERT_EQ(plan->size(), 1u);
  EXPECT_NE((*plan)[0][3].find("USING PRIMARY KEY"), std::string::npos);
}

TEST_F(FactPageTest, SparseIntersectionAdvancesFromLastExamined) {
  for (int i = 0; i < 7; ++i)
    batch.facts.push_back(Reachable("fixed", std::to_string(i)));
  std::ranges::sort(batch.facts);
  selector.equalities.push_back(
      {1, ColumnDomain::kFunctionId, batch.facts.back().row.cells[1]});
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.max_examined = 3;
  for (const auto index : {2u, 5u}) {
    auto page = snapshot->ReadFacts(selector, request);
    ASSERT_TRUE(page.ok()) << page.status().message();
    EXPECT_TRUE(page->facts.empty());
    EXPECT_EQ(page->examined, 3u);
    EXPECT_TRUE(page->work_exhausted);
    EXPECT_FALSE(page->exhausted);
    ASSERT_TRUE(page->continuation);
    EXPECT_EQ(page->continuation->last_consumed, batch.facts[index].fact_id);
    request.after = page->continuation;
    // The conjunction's identity and driving order are canonical.
    std::ranges::reverse(selector.equalities);
  }
  auto page = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_EQ(page->facts, std::vector<AnalysisFact>{batch.facts.back()});
  EXPECT_EQ(page->examined, 3u);
  EXPECT_TRUE(page->work_exhausted);
  request.after = page->continuation;
  page = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_TRUE(page->exhausted);
  EXPECT_EQ(page->examined, 0u);
}

TEST_F(FactPageTest, CollisionDoesNotReturnWrongValue) {
  for (int i = 0; i < 7; ++i)
    batch.facts.push_back(Reachable("collision", std::to_string(i)));
  ASSERT_TRUE(store->Publish(batch).ok());
  auto &sql = store->metadata_store();
  // Model SHA collisions by assigning every other column-0 entry the digest of
  // the requested value, then delete the true matches from that index stream.
  const auto fixed = core::ToString(batch.facts[0].fact_id);
  const auto fixed2 = core::ToString(batch.facts[1].fact_id);
  auto digest = sql.Query("SELECT value_digest FROM fact_lookup.fact_lookup_v1 "
                          "WHERE column_index = 0 AND fact_id = ?",
                          {fixed});
  ASSERT_TRUE(digest.ok());
  ASSERT_EQ(digest->size(), 1u);
  ASSERT_TRUE(sql.Execute("DELETE FROM fact_lookup.fact_lookup_v1 WHERE "
                          "column_index = 0 AND fact_id IN (?, ?)",
                          {fixed, fixed2})
                  .ok());
  ASSERT_TRUE(
      sql.Execute("UPDATE fact_lookup.fact_lookup_v1 SET value_digest = ? "
                  "WHERE column_index = 0",
                  {(*digest)[0][0]})
          .ok());
  batch.facts.erase(batch.facts.begin(), batch.facts.begin() + 2);
  std::ranges::sort(batch.facts);
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.max_examined = 3;
  for (const auto index : {2u, 5u}) {
    auto page = snapshot->ReadFacts(selector, request);
    ASSERT_TRUE(page.ok()) << page.status().message();
    EXPECT_TRUE(page->facts.empty());
    EXPECT_EQ(page->examined, 3u);
    EXPECT_TRUE(page->work_exhausted);
    ASSERT_TRUE(page->continuation);
    EXPECT_EQ(page->continuation->last_consumed, batch.facts[index].fact_id);
    request.after = page->continuation;
  }
  auto page = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_TRUE(page->facts.empty());
  EXPECT_EQ(page->examined, 1u);
  EXPECT_TRUE(page->exhausted);
}

TEST_F(FactPageTest, ByteFrontierDoesNotConsumeOversizedFirstRow) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.limit = 1;
  request.max_output_bytes = 1;
  auto page = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_EQ(page->examined, 1u);
  EXPECT_TRUE(page->facts.empty());
  EXPECT_FALSE(page->exhausted);
  EXPECT_FALSE(page->continuation);
  ASSERT_TRUE(page->required_output_bytes);
  request.max_output_bytes = *page->required_output_bytes;
  auto retry = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(retry.ok()) << retry.status().message();
  EXPECT_EQ(retry->facts.size(), 1u);
  EXPECT_EQ(retry->output_bytes, request.max_output_bytes);
}

TEST_F(FactPageTest, RejectsInvalidTypedSelectorsAndLimits) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto snapshot = Snapshot().value();
  auto malformed = selector;
  malformed.equalities.front().value = std::uint64_t{1};
  EXPECT_EQ(snapshot->ReadFacts(malformed, {}).status().code(),
            StatusCode::kInvalidArgument);
  malformed = selector;
  malformed.equalities.push_back(malformed.equalities.front());
  EXPECT_EQ(snapshot->ReadFacts(malformed, {}).status().code(),
            StatusCode::kInvalidArgument);
  PageRequest request;
  request.limit = kMaxFactPageRows + 1;
  EXPECT_EQ(snapshot->ReadFacts(selector, request).status().code(),
            StatusCode::kInvalidArgument);
  request.limit = 1;
  request.max_examined = 0;
  EXPECT_EQ(snapshot->ReadFacts(selector, request).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(FactPageTest, InterruptedBackfillStaysUnavailableAcrossReopen) {
  ASSERT_TRUE(store->Publish(batch).ok());
  auto &sql = store->metadata_store();
  ASSERT_TRUE(
      sql.Execute("UPDATE fact_lookup.index_meta SET status = 'BUILDING'", {})
          .ok());
  ASSERT_TRUE(
      sql.Execute("CREATE TRIGGER fact_lookup.fail_seal BEFORE INSERT "
                  "ON index_meta BEGIN SELECT RAISE(ABORT,'interrupted'); END",
                  {})
          .ok());
  auto interrupted = store->RebuildFactLookup(batch.run.run_id);
  EXPECT_FALSE(interrupted.ok());
  EXPECT_NE(interrupted.message().find("interrupted"), std::string_view::npos);
  store.reset();
  auto reopened = FactStore::Open(path);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  store.emplace(std::move(*reopened));
  auto snapshot = Snapshot().value();
  auto page = snapshot->ReadFacts(selector, {});
  EXPECT_FALSE(page.ok());
  EXPECT_NE(page.status().message().find("UNSUPPORTED"),
            std::string_view::npos);
  auto legacy = snapshot->GetCurrentFacts();
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy->size(), 2u);
  snapshot.reset();
  ASSERT_TRUE(store->metadata_store()
                  .Execute("DROP TRIGGER fact_lookup.fail_seal", {})
                  .ok());
  auto rebuilt = store->RebuildFactLookup(batch.run.run_id);
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.message();
  snapshot = Snapshot().value();
  page = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(page.ok()) << page.status().message();
  std::ranges::sort(batch.facts);
  EXPECT_EQ(page->facts, batch.facts);
  EXPECT_TRUE(page->exhausted);
}

TEST_F(FactPageTest, LegacyStoreRefusesUntilExplicitRebuild) {
  ASSERT_TRUE(store->Publish(batch).ok());
  store.reset();
  ASSERT_TRUE(std::filesystem::remove(path / "fact_lookup.v1.db"));
  auto reopened = FactStore::Open(path);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  store.emplace(std::move(*reopened));
  auto snapshot = Snapshot().value();
  auto page = snapshot->ReadFacts(selector, {});
  EXPECT_FALSE(page.ok());
  EXPECT_NE(page.status().message().find("UNSUPPORTED"),
            std::string_view::npos);
  snapshot.reset();
  auto status = store->RebuildFactLookup(batch.run.run_id);
  ASSERT_TRUE(status.ok()) << status.message();
  snapshot = Snapshot().value();
  page = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_EQ(page->facts.size(), 2u);
  EXPECT_TRUE(page->exhausted);
}

TEST_F(FactPageTest, ForeignSidecarWithSameReceiptsCannotHideLegacyBindings) {
  const auto run = batch.run.run_id;
  // Simulate a pre-v4 native history followed by a v4 batch. The sibling
  // store has the same sealed receipt but different older current bindings.
  const auto initial = batch;
  ASSERT_TRUE(store->Publish(initial).ok());
  ASSERT_TRUE(store->metadata_store()
                  .Execute("DELETE FROM fact_batch_receipts", {})
                  .ok());
  batch.batch_id = Id(core::IdKind::kFact, "common-v4-batch");
  batch.facts = {Reachable("fixed", "three")};
  ASSERT_TRUE(store->Publish(batch).ok());
  ASSERT_TRUE(store->RebuildFactLookup(run).ok());
  auto snapshot = Snapshot().value();
  PageRequest request;
  request.limit = 1;
  const auto first = snapshot->ReadFacts(selector, request);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(first->continuation);
  request.after = first->continuation;
  snapshot.reset();

  const auto sibling = path / "sibling";
  {
    auto other = FactStore::Open(sibling);
    ASSERT_TRUE(other.ok()) << other.status().message();
    auto other_initial = initial;
    other_initial.facts = {Reachable("fixed", "sibling-only")};
    ASSERT_TRUE(other->Publish(other_initial).ok());
    ASSERT_TRUE(other->metadata_store()
                    .Execute("DELETE FROM fact_batch_receipts", {})
                    .ok());
    ASSERT_TRUE(other->Publish(batch).ok());
  }
  store.reset();
  ASSERT_TRUE(std::filesystem::copy_file(
      path / "fact_lookup.v1.db", sibling / "fact_lookup.v1.db",
      std::filesystem::copy_options::overwrite_existing));
  auto other = FactStore::Open(sibling);
  ASSERT_TRUE(other.ok()) << other.status().message();
  store.emplace(std::move(*other));
  snapshot = Snapshot().value();
  const auto foreign = snapshot->ReadFacts(selector, {});
  EXPECT_FALSE(foreign.ok());
  EXPECT_NE(foreign.status().message().find("UNSUPPORTED"),
            std::string_view::npos);
  snapshot.reset();
  ASSERT_TRUE(store->RebuildFactLookup(run).ok());
  snapshot = Snapshot().value();
  EXPECT_EQ(snapshot->ReadFacts(selector, request).status().code(),
            StatusCode::kInvalidArgument);
  const auto rebuilt = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(rebuilt.ok()) << rebuilt.status().message();
  EXPECT_TRUE(rebuilt->exhausted);
  auto expected = batch.facts;
  expected.push_back(Reachable("fixed", "sibling-only"));
  std::ranges::sort(expected);
  EXPECT_EQ(rebuilt->facts, expected);
}

TEST_F(FactPageTest, SplitCommitRefusesStaleSealUntilRebuilt) {
  ASSERT_TRUE(store->Publish(batch).ok());
  // Simulate the durable native half of an interrupted cross-file commit.
  const auto run = core::ToString(batch.run.run_id);
  ASSERT_TRUE(
      store->metadata_store()
          .Execute("INSERT INTO fact_batch_receipts VALUES (?, ?, ?)",
                   {run, core::ToString(Id(core::IdKind::kFact, "new")), run})
          .ok());
  auto snapshot = Snapshot().value();
  const auto stale = snapshot->ReadFacts(selector, {});
  EXPECT_FALSE(stale.ok());
  EXPECT_NE(stale.status().message().find("UNSUPPORTED"),
            std::string_view::npos);
  snapshot.reset();
  ASSERT_TRUE(store->RebuildFactLookup(batch.run.run_id).ok());
  snapshot = Snapshot().value();
  const auto page = snapshot->ReadFacts(selector, {});
  ASSERT_TRUE(page.ok()) << page.status().message();
  EXPECT_EQ(page->facts.size(), 2u);
  EXPECT_TRUE(page->exhausted);
}

} // namespace
