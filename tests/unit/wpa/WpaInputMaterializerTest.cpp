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

#include "veritas/wpa/WpaInputMaterializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;
namespace semantic = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId MemoryId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kMemoryRef,
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

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v2::FunctionSummary* summary, std::string_view site,
             const std::string& resolved_target, v2::DispatchKind dispatch,
             v1::EpistemicState epistemic) {
  auto* call = summary->add_calls();
  call->set_call_site_id(core::ToString(CallSiteId(site)));
  call->set_callee_symbol(std::string(site));
  call->set_resolved_callee_function_variant_id(resolved_target);
  call->set_dispatch(dispatch);
  call->set_epistemic(epistemic);
  call->set_provenance_ref("test:call");
}

void AddWrite(v2::FunctionSummary* summary, std::string_view memory) {
  auto* effect = summary->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
  auto* location = effect->mutable_location();
  location->set_memory_location_id(core::ToString(MemoryId(memory)));
}

std::size_t CountRows(std::span<const facts::ExecutionRow> edb,
                      facts::RelationId relation) {
  return static_cast<std::size_t>(std::ranges::count_if(
      edb, [&](const auto& row) { return row.relation == relation; }));
}

const facts::ExecutionRow* FirstRow(std::span<const facts::ExecutionRow> edb,
                                    facts::RelationId relation) {
  const auto it = std::ranges::find_if(
      edb, [&](const auto& row) { return row.relation == relation; });
  return it == edb.end() ? nullptr : &*it;
}

core::StableId SccIdFor(std::span<const summary::SummaryArtifact> artifacts,
                        std::string_view function) {
  auto graph = CallGraph::FromSummaries(artifacts);
  auto scc = SccGraph::Build(*graph);
  return *scc->SccForFunction(FunctionId(function));
}

// A caller whose indirect call SVF resolved as a MAY candidate, plus a second
// call that stayed unresolved.
std::vector<summary::SummaryArtifact> CallSummaryWithMayTargetAndUnknown() {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_INDIRECT, v1::EPISTEMIC_STATE_MAY);
  AddCall(&caller, "opaque", /*resolved_target=*/"", v2::DISPATCH_KIND_INDIRECT,
          v1::EPISTEMIC_STATE_UNKNOWN);
  return {caller, V2Summary("target")};
}

WpaMaterializationRequest Request(
    std::span<const summary::SummaryArtifact> artifacts,
    WpaComponentKind component, std::string_view root_function) {
  WpaMaterializationRequest request;
  request.semantics = Semantics();
  request.scc_id = SccIdFor(artifacts, root_function);
  request.component = component;
  request.summaries = artifacts;
  request.models = nullptr;
  return request;
}

// Writes a throwaway model bundle so the test does not depend on the shipped
// models, whose symbols no fixture currently exercises.
class ModelBundleFixture {
 public:
  ModelBundleFixture() {
    dir_ = std::filesystem::temp_directory_path() /
           ("veritas_materializer_models_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
    std::ofstream rows(dir_ / "models.tsv");
    // Sorted by (symbol, seed), as ModelBundle::Load requires.
    rows << "memcpy.read.memory\tmemcpy\tread\tsource\tmay\n";
    rows << "memcpy.write.memory\tmemcpy\twrite\tdestination\tmay\n";
    rows.close();
    std::ofstream manifest(dir_ / "models.manifest");
    manifest << "model_bundle_version=models.test\n";
  }
  ~ModelBundleFixture() { std::filesystem::remove_all(dir_); }

  StatusOr<analysis::semantic::ModelBundle> Load() const {
    return analysis::semantic::ModelBundle::Load(dir_ / "models.tsv",
                                                 dir_ / "models.manifest");
  }

 private:
  std::filesystem::path dir_;
};

// A model describes an external function that has no summary, so it has no
// function-variant id and cannot occupy the relation's FunctionId column. The
// column therefore names the member that invokes the modeled function, which
// is also why the relation carries no call-site column: attribution is per
// function, not per call.
TEST(WpaInputMaterializerTest, AttributesModeledEffectsToTheCallingMember) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "memcpy", /*resolved_target=*/"", v2::DISPATCH_KIND_EXTERNAL,
          v1::EPISTEMIC_STATE_UNKNOWN);
  const std::vector<summary::SummaryArtifact> artifacts = {caller};

  ModelBundleFixture models;
  auto bundle = models.Load();
  ASSERT_TRUE(bundle.ok()) << bundle.status().message();

  auto request = Request(artifacts, WpaComponentKind::kReachability, "caller");
  request.models = &*bundle;
  auto input = WpaInputMaterializer::Build(request);
  ASSERT_TRUE(input.ok()) << input.status().message();

  // Two model rows for memcpy: a read of source and a write of destination.
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kModeledEffect), 2u);

  const auto* modeled = FirstRow(input->edb, facts::RelationId::kModeledEffect);
  ASSERT_NE(modeled, nullptr);
  // function_id resolves to the caller, not to memcpy.
  auto function = input->mappings.functions.ToStable(
      std::get<facts::FunctionId>(modeled->cells[1]));
  ASSERT_TRUE(function.ok());
  EXPECT_EQ(*function, FunctionId("caller"));

  // The bundle states `may`, but ModeledEffect admits only MUST or ASSUMED. A
  // model is an assumption, so it enters as ASSUMED rather than being dropped
  // or silently coerced to MUST.
  EXPECT_EQ(std::get<semantic::EpistemicState>(modeled->cells[4]),
            semantic::EpistemicState::kAssumed);
}

// Without a bundle the relation is simply absent; no model is invented.
TEST(WpaInputMaterializerTest, EmitsNoModeledEffectsWithoutABundle) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "memcpy", /*resolved_target=*/"", v2::DISPATCH_KIND_EXTERNAL,
          v1::EPISTEMIC_STATE_UNKNOWN);
  const std::vector<summary::SummaryArtifact> artifacts = {caller};

  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kReachability, "caller"));
  ASSERT_TRUE(input.ok());
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kModeledEffect), 0u);
}

// An indirect MAY target must reach the EDB as a DirectCall row carrying its
// dispatch and epistemic state independently -- MAY does not make the dispatch
// unknown, and an indirect dispatch does not make the target unknown. The
// unresolved call becomes an explicit UnknownCall row rather than vanishing.
TEST(WpaInputMaterializerTest, EmitsIndirectCallAndExplicitUnknownCall) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();
  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kReachability, "caller"));
  ASSERT_TRUE(input.ok());

  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kDirectCall), 1u);
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kUnknownCall), 1u);

  const auto* call = FirstRow(input->edb, facts::RelationId::kDirectCall);
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(std::get<semantic::DispatchKind>(call->cells[3]),
            semantic::DispatchKind::kIndirect);
  EXPECT_EQ(std::get<semantic::EpistemicState>(call->cells[4]),
            semantic::EpistemicState::kMay);
}

TEST(WpaInputMaterializerTest, DenseIdsRoundTripToStableIds) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();
  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kReachability, "caller"));
  ASSERT_TRUE(input.ok());

  const auto& functions = input->mappings.functions;
  ASSERT_FALSE(functions.StableIds().empty());
  for (const auto& stable : functions.StableIds()) {
    auto dense = functions.ToDense(stable);
    ASSERT_TRUE(dense.ok());
    auto round_tripped = functions.ToStable(*dense);
    ASSERT_TRUE(round_tripped.ok());
    EXPECT_EQ(*round_tripped, stable);
  }
}

// A successor SCC's result enters as an EDB support row and is recorded as a
// rooted input, so the component can cite it without owning it.
TEST(WpaInputMaterializerTest, SuccessorFactsAreExplicitSupportRows) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();

  facts::SemanticRow reachable;
  reachable.relation = facts::RelationId::kReachableCall;
  reachable.cells = {FunctionId("target"), FunctionId("deep"),
                     semantic::EpistemicState::kMay};
  auto support_fact = facts::MakeFact(reachable);
  ASSERT_TRUE(support_fact.ok());
  const std::vector<facts::AnalysisFact> support = {*support_fact};

  auto request = Request(artifacts, WpaComponentKind::kReachability, "caller");
  request.successor_support = support;
  auto input = WpaInputMaterializer::Build(request);
  ASSERT_TRUE(input.ok());

  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kSupportReachableCall),
            1u);
  EXPECT_EQ(input->successor_roots.size(), 1u);
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kReachableCall), 0u);
}

// Every derived fact must terminate its witness chain in a declared input, so
// a member's own base facts have to be recorded as rooted inputs. Identity map
// relations are plumbing, not facts, and are not roots.
TEST(WpaInputMaterializerTest, MemberBaseFactsAreRootedInputs) {
  const auto artifacts = CallSummaryWithMayTargetAndUnknown();
  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kReachability, "caller"));
  ASSERT_TRUE(input.ok());

  // One resolved call and one unresolved call.
  ASSERT_EQ(input->local_roots.size(), 2u);
  for (const auto& root : input->local_roots) {
    EXPECT_FALSE(root.provenance_ref.empty());
    EXPECT_EQ(root.fact.fact_id.kind, core::IdKind::kFact);
  }
}

// The hash covers canonical semantic content, not the order summaries happened
// to arrive in.
TEST(WpaInputMaterializerTest, LogicalInputHashIgnoresSummaryOrder) {
  auto forward = CallSummaryWithMayTargetAndUnknown();
  std::vector<summary::SummaryArtifact> reverse(forward.rbegin(),
                                                forward.rend());

  auto first = WpaInputMaterializer::Build(
      Request(forward, WpaComponentKind::kReachability, "caller"));
  auto second = WpaInputMaterializer::Build(
      Request(reverse, WpaComponentKind::kReachability, "caller"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first->logical_input_hash, second->logical_input_hash);
}

// Two components over the same SCC are distinct execution units and must not
// collide in the content-addressed component cache.
TEST(WpaInputMaterializerTest, LogicalInputHashSeparatesComponents) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST);
  AddWrite(&caller, "heap");
  const std::vector<summary::SummaryArtifact> artifacts = {
      caller, V2Summary("target")};

  auto reachability = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kReachability, "caller"));
  auto memory = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kMemoryEffects, "caller"));
  ASSERT_TRUE(reachability.ok());
  ASSERT_TRUE(memory.ok());
  EXPECT_NE(reachability->logical_input_hash, memory->logical_input_hash);
}

// The memory-effects component needs the writes themselves and the call edges
// the transitive rule walks.
TEST(WpaInputMaterializerTest, MemoryComponentEmitsWritesAndCalls) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST);
  AddWrite(&caller, "heap");
  const std::vector<summary::SummaryArtifact> artifacts = {
      caller, V2Summary("target")};

  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kMemoryEffects, "caller"));
  ASSERT_TRUE(input.ok());
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kDirectWrite), 1u);
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kDirectCall), 1u);
}

// Only the current SCC's members contribute local facts; a non-member's
// effects must not be claimed by this component.
TEST(WpaInputMaterializerTest, OnlyCurrentSccMembersContributeLocalFacts) {
  auto caller = V2Summary("caller");
  AddCall(&caller, "target", core::ToString(FunctionId("target")),
          v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST);
  AddWrite(&caller, "caller_heap");
  auto target = V2Summary("target");
  AddWrite(&target, "target_heap");
  const std::vector<summary::SummaryArtifact> artifacts = {caller, target};

  auto input = WpaInputMaterializer::Build(
      Request(artifacts, WpaComponentKind::kMemoryEffects, "caller"));
  ASSERT_TRUE(input.ok());
  // "caller" and "target" are separate SCCs, so only the caller's own write
  // belongs to this component.
  EXPECT_EQ(CountRows(input->edb, facts::RelationId::kDirectWrite), 1u);
}

}  // namespace
}  // namespace veritas::wpa
