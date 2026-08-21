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

#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(
      core::IdKind::kFunctionVariant,
      std::as_bytes(std::span(name.data(), name.size())));
}

CallEdge MayCall(core::StableId caller, core::StableId callee,
                 std::string anchor) {
  return CallEdge{.caller = std::move(caller),
                  .callee = std::move(callee),
                  .call_site_anchor_id = std::move(anchor),
                  .epistemic = v1::EPISTEMIC_STATE_MAY,
                  .provenance_ref = "test:call"};
}

TEST(SccGraphTest, MutualRecursionHasOneInsertionIndependentScc) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");

  CallGraph first;
  ASSERT_TRUE(first.AddFunction(a).ok());
  ASSERT_TRUE(first.AddFunction(b).ok());
  ASSERT_TRUE(first.AddCall(MayCall(a, b, "site:a-b")).ok());
  ASSERT_TRUE(first.AddCall(MayCall(b, a, "site:b-a")).ok());

  CallGraph second;
  ASSERT_TRUE(second.AddFunction(b).ok());
  ASSERT_TRUE(second.AddFunction(a).ok());
  ASSERT_TRUE(second.AddCall(MayCall(b, a, "site:b-a")).ok());
  ASSERT_TRUE(second.AddCall(MayCall(a, b, "site:a-b")).ok());

  auto first_scc = SccGraph::Build(first);
  auto second_scc = SccGraph::Build(second);
  ASSERT_TRUE(first_scc.ok());
  ASSERT_TRUE(second_scc.ok());
  EXPECT_EQ(*first_scc->SccForFunction(a),
            *first_scc->SccForFunction(b));
  EXPECT_EQ(*first_scc->SccForFunction(a),
            *second_scc->SccForFunction(a));
}

TEST(SccGraphTest, AcyclicGraphOrdersCalleesBeforeCallers) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  const auto c = FunctionId("C");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddFunction(b).ok());
  ASSERT_TRUE(graph.AddFunction(c).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(a, b, "site:a-b")).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(b, c, "site:b-c")).ok());

  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());
  const auto order = scc->ReverseTopologicalOrder();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], *scc->SccForFunction(c));
  EXPECT_EQ(order[1], *scc->SccForFunction(b));
  EXPECT_EQ(order[2], *scc->SccForFunction(a));
}

TEST(SccGraphTest, UnknownCallDoesNotFanOutOrMergeFunctions) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  const auto c = FunctionId("C");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddFunction(b).ok());
  ASSERT_TRUE(graph.AddFunction(c).ok());
  ASSERT_TRUE(graph.AddUnknownCall(
      {.caller = a,
       .call_site_anchor_id = "site:unknown",
       .callee_symbol = "vendor_validate",
       .provenance_ref = "test:unknown"}).ok());

  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());
  EXPECT_NE(*scc->SccForFunction(a), *scc->SccForFunction(b));
  EXPECT_NE(*scc->SccForFunction(a), *scc->SccForFunction(c));
  EXPECT_TRUE(graph.Outgoing(a).empty());
  EXPECT_EQ(graph.UnknownCalls(a).size(), 1u);
}

TEST(SccGraphTest, SelfRecursiveFunctionFormsOneMemberRecursiveScc) {
  const auto a = FunctionId("A");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(a, a, "site:self")).ok());

  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());
  auto members = scc->Members(*scc->SccForFunction(a));
  ASSERT_TRUE(members.ok());
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ((*members)[0], a);
}

TEST(CallGraphTest, SummaryWithoutResolvedCalleeProducesScopedUnknown) {
  const auto a = FunctionId("A");
  v1::FunctionSummary summary;
  summary.mutable_identity()->set_function_variant_id(core::ToString(a));
  auto* call = summary.add_calls();
  call->set_call_site_anchor_id("site:unknown");
  call->set_callee_symbol("vendor_validate");
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:summary");

  const std::array summaries{summary};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  EXPECT_TRUE(graph->Outgoing(a).empty());
  ASSERT_EQ(graph->UnknownCalls(a).size(), 1u);
  EXPECT_EQ(graph->UnknownCalls(a)[0].callee_symbol, "vendor_validate");
}

TEST(CallGraphTest, ResolvedButUnavailableCalleeRemainsScopedUnknown) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  v1::FunctionSummary summary;
  summary.mutable_identity()->set_function_variant_id(core::ToString(a));
  auto* call = summary.add_calls();
  call->set_call_site_anchor_id("site:a-b");
  call->set_callee_symbol("B");
  call->set_resolved_callee_function_variant_id(core::ToString(b));
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);

  const std::array summaries{summary};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  EXPECT_TRUE(graph->Outgoing(a).empty());
  EXPECT_EQ(graph->UnknownCalls(a).size(), 1u);
}

TEST(CallGraphTest, ConflictingFactsAtOneCallSiteAreRejected) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddFunction(b).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(a, b, "site:call")).ok());
  auto conflicting = MayCall(a, a, "site:call");
  EXPECT_EQ(graph.AddCall(std::move(conflicting)).code(),
            StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace veritas::wpa
