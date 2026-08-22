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

#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/CallGraph.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
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
  EXPECT_EQ(*first_scc->SccForFunction(a), *first_scc->SccForFunction(b));
  EXPECT_EQ(*first_scc->SccForFunction(a), *second_scc->SccForFunction(a));
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
  ASSERT_TRUE(graph
                  .AddUnknownCall({.caller = a,
                                   .call_site_anchor_id = "site:unknown",
                                   .callee_symbol = "vendor_validate",
                                   .provenance_ref = "test:unknown"})
                  .ok());

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

TEST(SccGraphTest, DeepAcyclicChainUsesBoundedNativeStack) {
  constexpr std::size_t kFunctionCount = 20000;
  std::vector<core::StableId> chain;
  chain.reserve(kFunctionCount);
  for (std::size_t index = 0; index < kFunctionCount; ++index)
    chain.push_back(FunctionId("deep-" + std::to_string(index)));

  auto insertion_order = chain;
  std::ranges::sort(insertion_order);
  CallGraph graph;
  for (const auto &function : insertion_order)
    ASSERT_TRUE(graph.AddFunction(function).ok());
  for (std::size_t index = 1; index < chain.size(); ++index) {
    ASSERT_TRUE(graph
                    .AddCall(MayCall(chain[index - 1], chain[index],
                                     "site:" + std::to_string(index)))
                    .ok());
  }

  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok()) << scc.status().message();
  ASSERT_EQ(scc->ReverseTopologicalOrder().size(), kFunctionCount);
  EXPECT_EQ(scc->ReverseTopologicalOrder().front(),
            *scc->SccForFunction(chain.back()));
  EXPECT_EQ(scc->ReverseTopologicalOrder().back(),
            *scc->SccForFunction(chain.front()));
}

TEST(CallGraphTest, SummaryWithoutResolvedCalleeProducesScopedUnknown) {
  const auto a = FunctionId("A");
  v1::FunctionSummary summary;
  summary.mutable_identity()->set_function_variant_id(core::ToString(a));
  auto *call = summary.add_calls();
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
  auto *call = summary.add_calls();
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
  auto conflicting = MayCall(a, b, "site:call");
  conflicting.epistemic = v1::EPISTEMIC_STATE_MUST;
  EXPECT_EQ(graph.AddCall(std::move(conflicting)).code(),
            StatusCode::kInvalidArgument);
}

TEST(CallGraphTest, RefinedIndirectCallAllowsMultipleTargetsAtOneSite) {
  const auto caller = FunctionId("caller");
  const auto first = FunctionId("first");
  const auto second = FunctionId("second");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(caller).ok());
  ASSERT_TRUE(graph.AddFunction(first).ok());
  ASSERT_TRUE(graph.AddFunction(second).ok());

  EXPECT_TRUE(graph.AddCall(MayCall(caller, first, "site:indirect")).ok());
  EXPECT_TRUE(graph.AddCall(MayCall(caller, second, "site:indirect")).ok());
  ASSERT_EQ(graph.Outgoing(caller).size(), 2u);
}

TEST(CallGraphTest, MultipleTargetsAtOneSiteRequireMayEdges) {
  const auto caller = FunctionId("caller");
  const auto first = FunctionId("first");
  const auto second = FunctionId("second");

  for (const bool must_edge_first : {false, true}) {
    CallGraph graph;
    ASSERT_TRUE(graph.AddFunction(caller).ok());
    ASSERT_TRUE(graph.AddFunction(first).ok());
    ASSERT_TRUE(graph.AddFunction(second).ok());

    auto first_edge = MayCall(caller, first, "site:indirect");
    auto second_edge = MayCall(caller, second, "site:indirect");
    (must_edge_first ? first_edge : second_edge).epistemic =
        v1::EPISTEMIC_STATE_MUST;
    ASSERT_TRUE(graph.AddCall(std::move(first_edge)).ok());
    EXPECT_EQ(graph.AddCall(std::move(second_edge)).code(),
              StatusCode::kInvalidArgument);
  }
}

TEST(CallGraphTest, MalformedResolvedCalleeIdentityIsRejected) {
  auto caller = v1::FunctionSummary{};
  caller.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId("caller")));
  auto *call = caller.add_calls();
  call->set_call_site_anchor_id("site:bad");
  call->set_callee_symbol("bad");
  call->set_resolved_callee_function_variant_id("not-a-stable-id");
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);

  const std::array malformed{caller};
  auto malformed_graph = CallGraph::FromSummaries(malformed);
  ASSERT_FALSE(malformed_graph.ok());
  EXPECT_EQ(malformed_graph.status().code(), StatusCode::kInvalidArgument);

  caller.mutable_calls(0)->set_resolved_callee_function_variant_id(
      core::ToString(
          core::MakeStableId(core::IdKind::kFunctionSummary,
                             std::as_bytes(std::span("wrong-kind", 10)))));
  const std::array wrong_kind{caller};
  auto wrong_kind_graph = CallGraph::FromSummaries(wrong_kind);
  ASSERT_FALSE(wrong_kind_graph.ok());
  EXPECT_EQ(wrong_kind_graph.status().code(), StatusCode::kInvalidArgument);

  caller.mutable_calls(0)->set_resolved_callee_function_variant_id(
      "funcvar:sha256:" + std::string(64, 'z'));
  const std::array non_hex{caller};
  auto non_hex_graph = CallGraph::FromSummaries(non_hex);
  ASSERT_FALSE(non_hex_graph.ok());
  EXPECT_EQ(non_hex_graph.status().code(), StatusCode::kInvalidArgument);
}

TEST(CallGraphTest, AddFunctionRejectsNonCanonicalStableIds) {
  CallGraph graph;
  EXPECT_EQ(
      graph.AddFunction({core::IdKind::kFunctionVariant, std::string(64, 'g')})
          .code(),
      StatusCode::kInvalidArgument);
  EXPECT_EQ(
      graph.AddFunction({core::IdKind::kFunctionVariant, std::string(64, 'A')})
          .code(),
      StatusCode::kInvalidArgument);
}

} // namespace
} // namespace veritas::wpa
