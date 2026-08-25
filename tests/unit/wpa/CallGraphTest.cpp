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

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/summary/SummaryArtifact.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

// A call whose target SVF resolved only as a MAY indirect candidate.
void AddCall(v2::FunctionSummary* summary, std::string_view callee_symbol,
             const std::string& resolved_target, v2::DispatchKind dispatch,
             v1::EpistemicState epistemic) {
  auto* call = summary->add_calls();
  call->set_call_site_id("site:" + std::string(callee_symbol));
  call->set_callee_symbol(std::string(callee_symbol));
  call->set_resolved_callee_function_variant_id(resolved_target);
  call->set_dispatch(dispatch);
  call->set_epistemic(epistemic);
  call->set_provenance_ref("test:call");
}

// SVF resolves an indirect call to a MAY candidate. The candidate is a real
// function, so it must enter the graph as an edge -- otherwise the SCC
// scheduler never sees the recursion an indirect call can create.
TEST(CallGraphTest, V2IndirectMayTargetBecomesEdge) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_INDIRECT, v1::EPISTEMIC_STATE_MAY);

  const std::vector<summary::SummaryArtifact> artifacts = {
      caller, V2Summary("target")};

  auto graph = CallGraph::FromSummaries(artifacts);
  ASSERT_TRUE(graph.ok());
  const auto outgoing = graph->Outgoing(FunctionId("caller"));
  ASSERT_EQ(outgoing.size(), 1u);
  EXPECT_EQ(outgoing[0].callee, FunctionId("target"));
  EXPECT_EQ(outgoing[0].epistemic, v1::EPISTEMIC_STATE_MAY);
  EXPECT_TRUE(graph->UnknownCalls(FunctionId("caller")).empty());
}

// An unresolved target stays a scoped unknown-call effect. It must never fan
// out to every function in the program.
TEST(CallGraphTest, V2UnresolvedTargetStaysScopedUnknownCall) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "opaque", /*resolved_target=*/"",
          v2::DISPATCH_KIND_INDIRECT, v1::EPISTEMIC_STATE_UNKNOWN);

  const std::vector<summary::SummaryArtifact> artifacts = {
      caller, V2Summary("other")};

  auto graph = CallGraph::FromSummaries(artifacts);
  ASSERT_TRUE(graph.ok());
  EXPECT_TRUE(graph->Outgoing(FunctionId("caller")).empty());
  const auto unknown = graph->UnknownCalls(FunctionId("caller"));
  ASSERT_EQ(unknown.size(), 1u);
  EXPECT_EQ(unknown[0].callee_symbol, "opaque");
}

// A V2 call may be ASSUMED (for example supplied by a model) and still names a
// real target, so it must be admitted as an edge rather than downgraded.
TEST(CallGraphTest, V2AssumedTargetBecomesEdge) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_ASSUMED);

  const std::vector<summary::SummaryArtifact> artifacts = {
      caller, V2Summary("target")};

  auto graph = CallGraph::FromSummaries(artifacts);
  ASSERT_TRUE(graph.ok());
  ASSERT_EQ(graph->Outgoing(FunctionId("caller")).size(), 1u);
  EXPECT_EQ(graph->Outgoing(FunctionId("caller"))[0].epistemic,
            v1::EPISTEMIC_STATE_ASSUMED);
}

// Tagged V1 artifacts remain readable and keep their MUST/MAY semantics; a V1
// projection can supply legacy calls but cannot fabricate V2 precision.
TEST(CallGraphTest, V1ArtifactsRemainSupported) {
  v1::FunctionSummary caller;
  caller.mutable_header()->set_schema_version("summary.v1");
  caller.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId("caller")));
  auto* call = caller.add_calls();
  call->set_callee_symbol("target");
  call->set_resolved_callee_function_variant_id(
      core::ToString(FunctionId("target")));
  call->set_call_site_anchor_id("site:target");
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);

  v1::FunctionSummary target;
  target.mutable_header()->set_schema_version("summary.v1");
  target.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId("target")));

  const std::vector<summary::SummaryArtifact> artifacts = {caller, target};

  auto graph = CallGraph::FromSummaries(artifacts);
  ASSERT_TRUE(graph.ok());
  ASSERT_EQ(graph->Outgoing(FunctionId("caller")).size(), 1u);
  EXPECT_EQ(graph->Outgoing(FunctionId("caller"))[0].epistemic,
            v1::EPISTEMIC_STATE_MUST);
}

}  // namespace
}  // namespace veritas::wpa
