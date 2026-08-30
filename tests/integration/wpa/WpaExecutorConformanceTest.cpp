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

// WpaExecutorConformanceTest.cpp — the two engines agree on V2 semantics.
//
// The one test here is the M8R.3 acceptance criterion that has been unproven
// since the bundles first type-checked: compiled Souffle and the C++ evaluator
// consume the same engine-neutral logical input and publish the same canonical
// facts. It runs only when the vendored Souffle worker is built.

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/CppConformanceExecutor.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

facts::AnalysisRunSemanticDescriptor Semantics() {
  facts::AnalysisRunSemanticDescriptor semantics;
  semantics.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  semantics.summary_schema_version = "summary.v2";
  semantics.relation_schema_version = "relations.v2";
  semantics.rule_bundle_version = "rules.v2";
  semantics.model_bundle_version = "models.v1";
  semantics.svf_configuration_hash = std::string(64, 'a');
  semantics.wpa_configuration_hash = std::string(64, 'b');
  return semantics;
}

facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev", 3)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = engine;
  descriptor.engine_toolchain_identity = "test-toolchain";
  auto manifest = facts::MakeAnalysisRun(descriptor);
  return std::move(manifest).value();
}

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v2::FunctionSummary* summary, std::string_view from,
             std::string_view to) {
  auto* call = summary->add_calls();
  call->set_call_site_id(
      core::ToString(CallSiteId(std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(core::ToString(FunctionId(to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

StatusOr<WpaLogicalComponentInput> InputFor(
    const std::vector<summary::SummaryArtifact>& artifacts,
    WpaComponentKind component, std::string_view root) {
  auto graph = CallGraph::FromSummaries(artifacts);
  if (!graph.ok())
    return graph.status();
  auto scc = SccGraph::Build(*graph);
  if (!scc.ok())
    return scc.status();
  auto scc_id = scc->SccForFunction(FunctionId(root));
  if (!scc_id.ok())
    return scc_id.status();

  WpaMaterializationRequest request;
  request.semantics = Semantics();
  request.scc_id = *scc_id;
  request.component = component;
  request.summaries = artifacts;
  return WpaInputMaterializer::Build(request);
}

// f and g are mutually recursive (one SCC); g also calls h, so reaching h from
// f requires the local transitive rule, not just a direct edge.
std::vector<summary::SummaryArtifact> RecursiveReachabilityProgram() {
  auto f = V2Summary("f");
  AddCall(&f, "f", "g");
  auto g = V2Summary("g");
  AddCall(&g, "g", "f");
  AddCall(&g, "g", "h");
  return {f, g, V2Summary("h")};
}

StatusOr<facts::CanonicalizedResult> Canonicalize(
    const WpaLogicalComponentInput& logical,
    const facts::RawWpaEvaluation& raw) {
  facts::CanonicalizationRequest request;
  request.local_roots = logical.local_roots;
  request.successor_roots = logical.successor_roots;
  request.evaluation = &raw;
  return facts::ResultCanonicalizer::Canonicalize(request);
}

TEST(WpaExecutorConformanceTest, EnginesProduceSameCanonicalFacts) {
  auto logical =
      InputFor(RecursiveReachabilityProgram(), WpaComponentKind::kReachability,
               "f");
  ASSERT_TRUE(logical.ok());

  const auto souffle_manifest =
      MakeManifest(facts::EngineIdentity::kSouffle);
  const auto cpp_manifest =
      MakeManifest(facts::EngineIdentity::kCppConformance);

  WpaExecutionEnvelope souffle_envelope{souffle_manifest, *logical};
  WpaExecutionEnvelope cpp_envelope{cpp_manifest, *logical};

  SouffleWpaExecutor souffle(VERITAS_SOUFFLE_WORKER);
  auto cpp = CppConformanceExecutor::Create(facts::EngineIdentity::kCppConformance);
  ASSERT_TRUE(cpp.ok());

  const WpaExecutionLimits limits{std::chrono::seconds(30), 0, 1};
  auto souffle_raw = souffle.Execute(souffle_envelope, limits);
  ASSERT_TRUE(souffle_raw.ok()) << souffle_raw.status().message();
  auto cpp_raw = cpp->Execute(cpp_envelope, limits);
  ASSERT_TRUE(cpp_raw.ok()) << cpp_raw.status().message();

  auto souffle_canonical = Canonicalize(*logical, *souffle_raw);
  ASSERT_TRUE(souffle_canonical.ok()) << souffle_canonical.status().message();
  auto cpp_canonical = Canonicalize(*logical, *cpp_raw);
  ASSERT_TRUE(cpp_canonical.ok()) << cpp_canonical.status().message();

  EXPECT_EQ(souffle_canonical->facts, cpp_canonical->facts);
  EXPECT_EQ(souffle_canonical->external_hash, cpp_canonical->external_hash);
  // The recursive program must actually produce reachability, not merely agree
  // on an empty result.
  EXPECT_FALSE(souffle_canonical->facts.empty());
}

}  // namespace
}  // namespace veritas::wpa
