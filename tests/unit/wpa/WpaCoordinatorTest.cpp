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

#include "veritas/wpa/WpaCoordinator.h"

#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

core::StableId Id(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(
      kind, std::as_bytes(std::span(text.data(), text.size())));
}

TEST(WpaCoordinatorTest, ExternalChangeSchedulesEachPredecessorOnce) {
  const auto caller = Id(core::IdKind::kFunctionVariant, "caller");
  const auto callee = Id(core::IdKind::kFunctionVariant, "callee");
  CallGraph call_graph;
  ASSERT_TRUE(call_graph.AddFunction(caller).ok());
  ASSERT_TRUE(call_graph.AddFunction(callee).ok());
  ASSERT_TRUE(call_graph.AddCall(
      {.caller = caller,
       .callee = callee,
       .call_site_anchor_id = "site",
       .epistemic = summary::v1::EPISTEMIC_STATE_MUST,
       .provenance_ref = "test"}).ok());
  auto graph = SccGraph::Build(call_graph);
  ASSERT_TRUE(graph.ok());
  const auto caller_scc = *graph->SccForFunction(caller);
  const auto callee_scc = *graph->SccForFunction(callee);
  const auto delta_id = Id(core::IdKind::kFact, "delta");
  const SccContext context{.revision_id = "rev", .build_variant_id = "bv"};
  runtime::WorklistScheduler scheduler;

  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kChanged, callee_scc,
      summary::v1::COMPONENT_KIND_MEMORY_EFFECTS, context, {delta_id}, *graph,
      &scheduler).ok());
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kChanged, callee_scc,
      summary::v1::COMPONENT_KIND_MEMORY_EFFECTS, context, {delta_id}, *graph,
      &scheduler).ok());
  EXPECT_EQ(scheduler.PendingCount(), 1u);
  const auto item = scheduler.PopNext();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->kind, runtime::WorkItemKind::kWpaComponent);
  EXPECT_EQ(item->target_id, caller_scc);
}

TEST(WpaCoordinatorTest, UnchangedExternalHashSchedulesNothing) {
  CallGraph call_graph;
  const auto function = Id(core::IdKind::kFunctionVariant, "A");
  ASSERT_TRUE(call_graph.AddFunction(function).ok());
  auto graph = SccGraph::Build(call_graph);
  ASSERT_TRUE(graph.ok());
  const auto scc_id = *graph->SccForFunction(function);
  runtime::WorklistScheduler scheduler;
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kUnchanged, scc_id,
      summary::v1::COMPONENT_KIND_MEMORY_EFFECTS,
      {.revision_id = "rev", .build_variant_id = "bv"}, {}, *graph,
      &scheduler).ok());
  EXPECT_TRUE(scheduler.Empty());
}

}  // namespace
}  // namespace veritas::wpa
