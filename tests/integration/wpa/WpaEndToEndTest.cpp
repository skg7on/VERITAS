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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/core/Ids.h"
#include "veritas/facts/AnalysisFactBus.h"
#include "veritas/facts/FactSchema.h"
#include "veritas/runtime/WorklistScheduler.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/SummaryRepository.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/CppConformanceExecutor.h"
#include "veritas/wpa/FixpointEngine.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SccStateRepository.h"
#include "veritas/wpa/WpaCoordinator.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

core::StableId Id(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(text.data(), text.size())));
}

v1::FunctionSummary Function(const SccContext &context, std::string_view name) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  auto *identity = summary.mutable_identity();
  identity->set_repository_id(
      core::ToString(Id(core::IdKind::kRepository, "repository")));
  identity->set_revision_id(context.revision_id);
  identity->set_build_variant_id(context.build_variant_id);
  identity->set_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, name)));
  identity->set_function_body_id(
      core::ToString(Id(core::IdKind::kFunctionBody, name)));
  return summary;
}

void AddResolvedCall(v1::FunctionSummary *caller, std::string_view callee) {
  auto *call = caller->add_calls();
  call->set_callee_symbol(std::string(callee));
  call->set_resolved_callee_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, callee)));
  call->set_call_site_anchor_id("site:" + std::string(callee));
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:resolved-call");
}

void AddUnknownCall(v1::FunctionSummary *caller) {
  auto *call = caller->add_calls();
  call->set_callee_symbol("vendor_validate");
  call->set_call_site_anchor_id("site:vendor_validate");
  call->set_epistemic(v1::EPISTEMIC_STATE_UNKNOWN);
  call->set_provenance_ref("test:unknown-call");
}

void AddWrite(v1::FunctionSummary *function, std::string memory) {
  auto *effect = function->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_location(std::move(memory));
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
}

const facts::FactTuple *FindFact(std::span<const SccResult> results,
                                 facts::FactRelation relation,
                                 const std::vector<std::string> &columns) {
  for (const auto &result : results) {
    for (const auto &fact : result.facts) {
      if (fact.relation == relation && fact.columns == columns)
        return &fact;
    }
  }
  return nullptr;
}

class WpaEndToEndTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_directory_ =
        std::filesystem::temp_directory_path() /
        ("veritas_wpa_end_to_end_" +
         std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
         std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(test_directory_);
    auto opened = summarydb::SummaryRepository::Open(test_directory_.string());
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    repository_ = std::move(*opened);

    ASSERT_TRUE(
        repository_->metadata_store()
            .Execute("INSERT INTO repositories(repository_id, vcs_kind, "
                     "vcs_revision, source_tree_hash) VALUES(?, ?, ?, ?)",
                     {repository_id_, "git", "r", "tree"})
            .ok());
    ASSERT_TRUE(
        repository_->metadata_store()
            .Execute("INSERT INTO revisions(revision_id, repository_id, "
                     "vcs_revision) VALUES(?, ?, ?)",
                     {context_.revision_id, repository_id_, "r"})
            .ok());
    ASSERT_TRUE(
        repository_->metadata_store()
            .Execute(
                "INSERT INTO build_variants(build_variant_id, target_triple, "
                "compiler_id, compiler_version, compile_options_hash, "
                "macro_set_hash, include_closure_hash, type_layout_hash) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                {context_.build_variant_id, "arm64", "clang", "24", "a", "b",
                 "c", "d"})
            .ok());
  }

  void TearDown() override {
    repository_.reset();
    std::filesystem::remove_all(test_directory_);
  }

  std::filesystem::path test_directory_;
  std::string repository_id_ =
      core::ToString(Id(core::IdKind::kRepository, "repository"));
  SccContext context_{
      .revision_id = core::ToString(Id(core::IdKind::kRevision, "revision")),
      .build_variant_id =
          core::ToString(Id(core::IdKind::kBuildVariant, "variant"))};
  std::unique_ptr<summarydb::SummaryRepository> repository_;
};

TEST_F(WpaEndToEndTest, PersistsFixpointAndSchedulesExternalChanges) {
  auto a = Function(context_, "A");
  auto b = Function(context_, "B");
  auto c = Function(context_, "C");
  AddResolvedCall(&a, "B");
  AddUnknownCall(&a);
  AddResolvedCall(&b, "C");
  AddWrite(&c, "X");
  auto published = repository_->PublishProjectSummaries(
      context_.revision_id, context_.build_variant_id, {c, b, a});
  ASSERT_TRUE(published.ok()) << published.status().message();

  auto summaries = repository_->ListCurrentSummaries(context_.revision_id,
                                                     context_.build_variant_id);
  ASSERT_TRUE(summaries.ok()) << summaries.status().message();
  ASSERT_EQ(summaries->size(), 3u);
  auto call_graph = CallGraph::FromSummaries(*summaries);
  ASSERT_TRUE(call_graph.ok()) << call_graph.status().message();
  auto scc_graph = SccGraph::Build(*call_graph);
  ASSERT_TRUE(scc_graph.ok()) << scc_graph.status().message();

  const auto a_id = Id(core::IdKind::kFunctionVariant, "A");
  const auto b_id = Id(core::IdKind::kFunctionVariant, "B");
  const auto c_id = Id(core::IdKind::kFunctionVariant, "C");
  ASSERT_EQ(call_graph->UnknownCalls(a_id).size(), 1u);
  EXPECT_EQ(call_graph->UnknownCalls(a_id)[0].callee_symbol, "vendor_validate");
  EXPECT_TRUE(call_graph->UnknownCalls(b_id).empty());
  EXPECT_TRUE(call_graph->UnknownCalls(c_id).empty());

  FixpointEngine engine(*call_graph, *scc_graph, *summaries);
  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                   {.max_iterations = 32});
  ASSERT_TRUE(results.ok()) << results.status().message();
  const facts::FactTuple *a_writes_x = FindFact(
      *results, facts::FactRelation::kMayWrite, {core::ToString(a_id), "X"});
  ASSERT_NE(a_writes_x, nullptr);
  EXPECT_EQ(a_writes_x->epistemic, v1::EPISTEMIC_STATE_MUST);
  EXPECT_FALSE(a_writes_x->rule_id.empty());
  EXPECT_FALSE(a_writes_x->input_tuple_ids.empty());

  SccStateRepository state_repository(repository_->metadata_store());
  ASSERT_TRUE(
      state_repository.PublishGraph(context_, *call_graph, *scc_graph).ok());
  runtime::WorklistScheduler scheduler;
  const auto delta_id = Id(core::IdKind::kFact, "triggering-delta");
  for (const auto &result : *results) {
    auto change = state_repository.StoreState(context_, result);
    ASSERT_TRUE(change.ok()) << change.status().message();
    EXPECT_EQ(*change, ExternalChange::kChanged);
    ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
                    *change, result.scc_id, result.component_kind, context_,
                    {delta_id}, *scc_graph, &scheduler)
                    .ok());
    ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
                    *change, result.scc_id, result.component_kind, context_,
                    {delta_id}, *scc_graph, &scheduler)
                    .ok());
  }
  EXPECT_EQ(scheduler.PendingCount(), 2u);

  auto a_scc = scc_graph->SccForFunction(a_id);
  auto b_scc = scc_graph->SccForFunction(b_id);
  ASSERT_TRUE(a_scc.ok()) << a_scc.status().message();
  ASSERT_TRUE(b_scc.ok()) << b_scc.status().message();
  std::set<core::StableId> scheduled_targets;
  while (auto item = scheduler.PopNext()) {
    EXPECT_EQ(item->kind, runtime::WorkItemKind::kWpaComponent);
    EXPECT_EQ(item->revision_id, context_.revision_id);
    EXPECT_EQ(item->build_variant_id, context_.build_variant_id);
    EXPECT_EQ(item->consumer_component, v1::COMPONENT_KIND_MEMORY_EFFECTS);
    scheduled_targets.insert(item->target_id);
  }
  EXPECT_EQ(scheduled_targets, (std::set<core::StableId>{*a_scc, *b_scc}));

  auto internal_only = results->front();
  internal_only.fixpoint_hash = std::string(64, 'd');
  auto change = state_repository.StoreState(context_, internal_only);
  ASSERT_TRUE(change.ok()) << change.status().message();
  EXPECT_EQ(*change, ExternalChange::kUnchanged);
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
                  *change, internal_only.scc_id, internal_only.component_kind,
                  context_, {delta_id}, *scc_graph, &scheduler)
                  .ok());
  EXPECT_TRUE(scheduler.Empty());
}

namespace v2 = summary::v2;

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, name)));
  return summary;
}

void AddV2Call(v2::FunctionSummary* summary, std::string_view from,
               std::string_view to) {
  auto* call = summary->add_calls();
  call->set_call_site_id(core::ToString(
      Id(core::IdKind::kCallSite, std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

facts::AnalysisRunManifest MakeRunManifest(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor descriptor;
  descriptor.revision_id = Id(core::IdKind::kRevision, "rev");
  descriptor.build_variant_id = Id(core::IdKind::kBuildVariant, "bv");
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = engine;
  descriptor.engine_toolchain_identity = "test-toolchain";
  return std::move(facts::MakeAnalysisRun(descriptor)).value();
}

// The M9 seam: a successful orchestration reduces to one valid, content-addressed
// batch that the fact bus validates and delivers. This pins the handoff the
// durable M9 store and explainFact will build on.
TEST(WpaFactBusHandoffTest, OrchestrationProducesValidFactBusBatch) {
  auto a = V2Summary("A");
  AddV2Call(&a, "A", "B");
  auto b = V2Summary("B");
  AddV2Call(&b, "B", "C");
  const std::vector<summary::SummaryArtifact> summaries = {a, b, V2Summary("C")};

  const auto db = std::filesystem::temp_directory_path() /
                  ("veritas_wpa_bus_" + std::to_string(getpid()) + "_" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  auto cpp = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppConformance, "test-toolchain");
  ASSERT_TRUE(cpp.ok());

  WpaOrchestrator orchestrator(*cpp, *repo);
  const std::array<WpaComponentKind, 1> components = {
      WpaComponentKind::kReachability};
  WpaRunRequest request;
  request.run = MakeRunManifest(facts::EngineIdentity::kCppConformance);
  request.summaries = summaries;
  request.components = components;
  auto result = orchestrator.Run(request);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ASSERT_FALSE(result->facts.empty());
  ASSERT_FALSE(result->witnesses.empty());

  facts::AnalysisFactBatch batch = facts::MakeAnalysisFactBatch(*result);
  EXPECT_FALSE(batch.facts.empty());
  EXPECT_FALSE(batch.witnesses.empty());
  EXPECT_EQ(batch.expected_components.size(), batch.completed_components.size());

  struct RecordingSink : facts::AnalysisFactSink {
    Status Publish(const facts::AnalysisFactBatch&) override {
      ++count;
      return Status::Ok();
    }
    int count = 0;
  } sink;

  facts::AnalysisFactBus bus(*repo);
  bus.AddSink("recording", sink);
  ASSERT_TRUE(bus.Publish(std::move(batch)).ok());
  EXPECT_EQ(sink.count, 1);

  std::filesystem::remove_all(db);
}

} // namespace
} // namespace veritas::wpa
