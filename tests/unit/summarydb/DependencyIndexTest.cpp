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

#include "veritas/summarydb/DependencyIndex.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "veritas/core/Ids.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

using summary::v1::ComponentKind;

core::StableId SummaryId(char c) {
  return core::MakeStableId(
      core::IdKind::kFunctionSummary,
      std::as_bytes(std::span<const char>(&c, static_cast<std::size_t>(1))));
}

DependencyEdge MakeEdge(char consumer, ComponentKind consumer_component,
                        char producer, ComponentKind producer_component,
                        Sensitivity sensitivity) {
  DependencyEdge edge;
  edge.consumer_id = SummaryId(consumer);
  edge.consumer_component = consumer_component;
  edge.producer_id = SummaryId(producer);
  edge.producer_component = producer_component;
  edge.dependency_kind = DependencyKind::kCall;
  edge.sensitivity = sensitivity;
  return edge;
}

core::SHA256Digest MakeDigest(std::uint8_t value) {
  core::SHA256Digest digest{};
  digest.fill(std::byte{value});
  return digest;
}

ComponentDelta SemanticDelta(ComponentKind kind) {
  ComponentDelta delta;
  delta.component_kind = kind;
  delta.old_semantic_hash = MakeDigest(0);
  delta.new_semantic_hash = MakeDigest(1);
  delta.old_evidence_hash = MakeDigest(0);
  delta.new_evidence_hash = MakeDigest(1);
  return delta;
}

ComponentDelta EvidenceDelta(ComponentKind kind) {
  ComponentDelta delta;
  delta.component_kind = kind;
  delta.old_semantic_hash = MakeDigest(0);
  delta.new_semantic_hash = MakeDigest(0);
  delta.old_evidence_hash = MakeDigest(0);
  delta.new_evidence_hash = MakeDigest(1);
  return delta;
}

class DependencyIndexTest : public ::testing::Test {
protected:
  void SetUp() override {
    db_path_ =
        std::filesystem::temp_directory_path() /
        ("veritas_dep_index_" + std::to_string(::getpid()) + "_" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name() +
         ".db");
    std::filesystem::remove(db_path_);

    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    metadata_ = std::make_unique<MetadataStore>(std::move(*store));
    ASSERT_TRUE(metadata_->ApplySchema().ok());
    index_ = std::make_unique<DependencyIndex>(*metadata_);
  }

  void TearDown() override { std::filesystem::remove(db_path_); }

  std::filesystem::path db_path_;
  std::unique_ptr<MetadataStore> metadata_;
  std::unique_ptr<DependencyIndex> index_;
};

TEST_F(DependencyIndexTest, UsersOfReturnsOnlyConsumersOfProducerComponent) {
  // decode.value_flow depends on validate.range; other.calls depends on
  // validate.calls. Lookup by (validate, RANGE_FACTS) must return only decode.
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('o'),
                      {MakeEdge('o', ComponentKind::COMPONENT_KIND_CALLS, 'v',
                                ComponentKind::COMPONENT_KIND_CALLS,
                                Sensitivity::kSemantic)})
                  .ok());

  auto range_users = index_->UsersOf(SummaryId('v'),
                                     ComponentKind::COMPONENT_KIND_RANGE_FACTS);
  ASSERT_TRUE(range_users.ok()) << range_users.status().message();
  ASSERT_EQ(range_users->size(), 1u);
  EXPECT_EQ((*range_users)[0].consumer_id, SummaryId('d'));
  EXPECT_EQ((*range_users)[0].consumer_component,
            ComponentKind::COMPONENT_KIND_VALUE_FLOW);

  auto call_users =
      index_->UsersOf(SummaryId('v'), ComponentKind::COMPONENT_KIND_CALLS);
  ASSERT_TRUE(call_users.ok());
  ASSERT_EQ(call_users->size(), 1u);
  EXPECT_EQ((*call_users)[0].consumer_id, SummaryId('o'));
}

TEST_F(DependencyIndexTest, RepublishRemovesStaleHotRows) {
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());

  // Republish decode against a different producer.
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'w', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());

  auto old_users = index_->UsersOf(SummaryId('v'),
                                   ComponentKind::COMPONENT_KIND_RANGE_FACTS);
  ASSERT_TRUE(old_users.ok());
  EXPECT_TRUE(old_users->empty());

  auto new_users = index_->UsersOf(SummaryId('w'),
                                   ComponentKind::COMPONENT_KIND_RANGE_FACTS);
  ASSERT_TRUE(new_users.ok());
  ASSERT_EQ(new_users->size(), 1u);
  EXPECT_EQ((*new_users)[0].consumer_id, SummaryId('d'));
}

TEST_F(DependencyIndexTest, HistoricalDependenciesRemainExplainable) {
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'w', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());

  // The historical record retains both publications even though the hot index
  // only holds the latest.
  auto historical = metadata_->Query(
      "SELECT producer_id FROM summary_dependencies WHERE consumer_id = ? "
      "ORDER BY dependency_id",
      {core::ToString(SummaryId('d'))});
  ASSERT_TRUE(historical.ok()) << historical.status().message();
  ASSERT_EQ(historical->size(), 2u);
  EXPECT_EQ((*historical)[0][0], core::ToString(SummaryId('v')));
  EXPECT_EQ((*historical)[1][0], core::ToString(SummaryId('w')));
}

TEST_F(DependencyIndexTest, SemanticDeltaFollowsSemanticEdgesOnly) {
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('e'),
                      {MakeEdge('e', ComponentKind::COMPONENT_KIND_PROVENANCE,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kEvidenceOnly)})
                  .ok());

  SummaryDelta delta;
  delta.old_summary_id = SummaryId('v');
  delta.new_summary_id = SummaryId('v');
  delta.changed_components.push_back(
      SemanticDelta(ComponentKind::COMPONENT_KIND_RANGE_FACTS));

  auto impact = index_->GetImpactSet(delta, ImpactBudget{});
  ASSERT_TRUE(impact.ok()) << impact.status().message();
  ASSERT_EQ(impact->consumers.size(), 1u);
  EXPECT_EQ(impact->consumers[0].consumer_id, SummaryId('d'));
}

TEST_F(DependencyIndexTest, EvidenceOnlyDeltaFollowsEvidenceEdgesOnly) {
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('d'),
                      {MakeEdge('d', ComponentKind::COMPONENT_KIND_VALUE_FLOW,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kSemantic)})
                  .ok());
  ASSERT_TRUE(index_
                  ->ReplaceCurrentDependencies(
                      SummaryId('e'),
                      {MakeEdge('e', ComponentKind::COMPONENT_KIND_PROVENANCE,
                                'v', ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                                Sensitivity::kEvidenceOnly)})
                  .ok());

  SummaryDelta delta;
  delta.old_summary_id = SummaryId('v');
  delta.new_summary_id = SummaryId('v');
  delta.changed_components.push_back(
      EvidenceDelta(ComponentKind::COMPONENT_KIND_RANGE_FACTS));

  auto impact = index_->GetImpactSet(delta, ImpactBudget{});
  ASSERT_TRUE(impact.ok()) << impact.status().message();
  ASSERT_EQ(impact->consumers.size(), 1u);
  EXPECT_EQ(impact->consumers[0].consumer_id, SummaryId('e'));
}

TEST_F(DependencyIndexTest, ImpactBudgetTruncatesExplicitly) {
  // Five consumers depend on validate.range; cap the impact set at two.
  for (char c = 'a'; c <= 'e'; ++c) {
    ASSERT_TRUE(
        index_
            ->ReplaceCurrentDependencies(
                SummaryId(c),
                {MakeEdge(c, ComponentKind::COMPONENT_KIND_VALUE_FLOW, 'v',
                          ComponentKind::COMPONENT_KIND_RANGE_FACTS,
                          Sensitivity::kSemantic)})
            .ok());
  }

  SummaryDelta delta;
  delta.old_summary_id = SummaryId('v');
  delta.new_summary_id = SummaryId('v');
  delta.changed_components.push_back(
      SemanticDelta(ComponentKind::COMPONENT_KIND_RANGE_FACTS));

  ImpactBudget budget;
  budget.max_consumers = 2;
  auto impact = index_->GetImpactSet(delta, budget);
  ASSERT_TRUE(impact.ok()) << impact.status().message();
  EXPECT_TRUE(impact->truncated);
  EXPECT_LE(impact->consumers.size(), 2u);
}

} // namespace
} // namespace veritas::summarydb
