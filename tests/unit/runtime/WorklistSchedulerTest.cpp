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

#include "veritas/runtime/WorklistScheduler.h"

#include <string>

#include <gtest/gtest.h>

namespace veritas::runtime {
namespace {

// A stable, unique-per-char ID with a valid 64-hex digest.
core::StableId Id(char c) {
  return core::StableId{core::IdKind::kFunctionSummary, std::string(64, c)};
}

WorkItem MakeWork(char target, WorkItemKind kind, int priority, char delta) {
  WorkItem item;
  item.kind = kind;
  item.target_id = Id(target);
  item.revision_id = "rev:sha256:r";
  item.build_variant_id = "bv:sha256:b";
  item.consumer_component = summary::v1::COMPONENT_KIND_VALUE_FLOW;
  item.priority = priority;
  item.triggering_delta_ids.push_back(Id(delta));
  return item;
}

TEST(WorklistSchedulerTest, DeduplicatesSameSemanticTarget) {
  WorklistScheduler scheduler;
  scheduler.Enqueue(MakeWork('d', WorkItemKind::kLocalSummary, 0, 'x'));
  scheduler.Enqueue(MakeWork('d', WorkItemKind::kLocalSummary, 0, 'x'));
  EXPECT_EQ(scheduler.PendingCount(), 1u);
}

TEST(WorklistSchedulerTest, MergesTriggeringDeltaIdsOnDuplicate) {
  WorklistScheduler scheduler;
  scheduler.Enqueue(MakeWork('d', WorkItemKind::kLocalSummary, 0, 'x'));
  scheduler.Enqueue(MakeWork('d', WorkItemKind::kLocalSummary, 0, 'y'));

  ASSERT_EQ(scheduler.PendingCount(), 1u);
  auto item = scheduler.PopNext();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->triggering_delta_ids.size(), 2u);
  EXPECT_EQ(item->target_id, Id('d'));
}

TEST(WorklistSchedulerTest, LocalSummaryRunsBeforeWpaItem) {
  WorklistScheduler scheduler;
  scheduler.Enqueue(MakeWork('a', WorkItemKind::kWpaComponent, 2, 'x'));
  scheduler.Enqueue(MakeWork('b', WorkItemKind::kLocalSummary, 0, 'y'));

  auto first = scheduler.PopNext();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->kind, WorkItemKind::kLocalSummary);
  EXPECT_EQ(first->target_id, Id('b'));
}

TEST(WorklistSchedulerTest, DrainsInPriorityOrder) {
  WorklistScheduler scheduler;
  scheduler.Enqueue(MakeWork('a', WorkItemKind::kEvidenceInvalidation, 4, 'x'));
  scheduler.Enqueue(MakeWork('b', WorkItemKind::kWpaComponent, 2, 'x'));
  scheduler.Enqueue(MakeWork('c', WorkItemKind::kLocalSummary, 0, 'x'));

  EXPECT_EQ(scheduler.PopNext()->target_id, Id('c'));
  EXPECT_EQ(scheduler.PopNext()->target_id, Id('b'));
  EXPECT_EQ(scheduler.PopNext()->target_id, Id('a'));
  EXPECT_TRUE(scheduler.Empty());
}

TEST(WorklistSchedulerTest, PopOnEmptyReturnsNullopt) {
  WorklistScheduler scheduler;
  EXPECT_TRUE(scheduler.Empty());
  EXPECT_FALSE(scheduler.PopNext().has_value());
}

}  // namespace
}  // namespace veritas::runtime
