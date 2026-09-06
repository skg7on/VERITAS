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

#include "veritas/wpa/WpaOrchestrator.h"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SccStateRepository.h"

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

core::StableId MemoryId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kMemoryRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor d;
  d.revision_id = core::MakeStableId(core::IdKind::kRevision,
                                     std::as_bytes(std::span("rev", 3)));
  d.build_variant_id = core::MakeStableId(core::IdKind::kBuildVariant,
                                          std::as_bytes(std::span("bv", 2)));
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'b');
  d.engine = engine;
  d.engine_toolchain_identity = "test-toolchain";
  return std::move(facts::MakeAnalysisRun(d)).value();
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

void AddWrite(v2::FunctionSummary* summary, std::string_view memory) {
  auto* effect = summary->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
  auto* location = effect->mutable_location();
  location->set_memory_location_id(core::ToString(MemoryId(memory)));
}

// a -> b -> c: three SCCs in a chain, so the reverse-topological order has c
// (the leaf) first.
std::vector<summary::SummaryArtifact> ChainProgram() {
  auto a = V2Summary("a");
  AddCall(&a, "a", "b");
  auto b = V2Summary("b");
  AddCall(&b, "b", "c");
  return {a, b, V2Summary("c")};
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// Records the SCC order in which Execute is called; returns an empty raw
// evaluation (the canonicalizer produces no facts, which is a valid result).
class RecordingExecutor : public WpaExecutor {
 public:
  explicit RecordingExecutor(std::vector<core::StableId>& order)
      : order_(order) {}

  facts::EngineIdentity identity() const override {
    return facts::EngineIdentity::kSouffle;
  }
  std::string_view toolchain_identity() const override {
    return "test-toolchain";
  }
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& envelope, const WpaExecutionLimits&) const override {
    order_.push_back(envelope.logical.scc_id);
    return facts::RawWpaEvaluation{};
  }

 private:
  std::vector<core::StableId>& order_;
};

class FailingExecutor : public WpaExecutor {
 public:
  facts::EngineIdentity identity() const override {
    return facts::EngineIdentity::kSouffle;
  }
  std::string_view toolchain_identity() const override {
    return "test-toolchain";
  }
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope&, const WpaExecutionLimits&) const override {
    return Status::Internal("injected failure");
  }
};

// Emits one derived fact per component, grounded in the materializer's own
// local roots, and records the successor support it observes. Reachability
// derives Reachable(caller, callee) from a DirectCall root; MemoryEffects
// derives MayWrite(function, memory) from a DirectWrite root.
class FactEmittingExecutor : public WpaExecutor {
 public:
  struct Observation {
    core::StableId scc_id;
    WpaComponentKind component;
    std::vector<facts::AnalysisFact> successor_roots;
  };

  facts::EngineIdentity identity() const override {
    return facts::EngineIdentity::kSouffle;
  }
  std::string_view toolchain_identity() const override {
    return "test-toolchain";
  }

  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& envelope, const WpaExecutionLimits&) const override {
    const auto& logical = envelope.logical;
    observations_.push_back(
        Observation{logical.scc_id, logical.component, {}});
    for (const auto& root : logical.successor_roots) {
      observations_.back().successor_roots.push_back(root.fact);
    }

    facts::RawWpaEvaluation raw;
    const bool reach = logical.component == WpaComponentKind::kReachability;
    const facts::RelationId input_relation =
        reach ? facts::RelationId::kDirectCall
              : facts::RelationId::kDirectWrite;
    for (const auto& root : logical.local_roots) {
      if (root.fact.row.relation != input_relation) {
        continue;
      }
      const auto& in = root.fact.row;
      facts::SemanticRow result;
      std::string rule;
      if (reach) {
        // DirectCall {site, caller, callee, dispatch, epistemic} ->
        // ReachableCall {caller, callee, epistemic}.
        result = facts::SemanticRow{facts::RelationId::kReachableCall,
                                    {in.cells[1], in.cells[2], in.cells[4]}};
        rule = "wpa.reachability.direct.v2";
      } else {
        // DirectWrite {fn, memory, range, offset, size, epistemic} ->
        // MayWrite {fn, memory, epistemic}.
        result = facts::SemanticRow{facts::RelationId::kMayWrite,
                                    {in.cells[0], in.cells[1], in.cells[5]}};
        rule = "wpa.memory.may_write.direct.v2";
      }
      raw.results.push_back(result);
      raw.witnesses.push_back(
          facts::WitnessEdge{.result = facts::SemanticKey{result},
                             .rule_id = rule,
                             .input = facts::SemanticKey{in},
                             .input_ordinal = 0});
      break;  // one derived fact per component
    }
    return raw;
  }

  const std::vector<Observation>& observations() const { return observations_; }

 private:
  mutable std::vector<Observation> observations_;
};

TEST(WpaOrchestratorTest, RunsSccsInReverseTopologicalOrder) {
  const auto program = ChainProgram();
  auto graph = CallGraph::FromSummaries(program);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());

  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  std::vector<core::StableId> order;
  RecordingExecutor executor(order);
  WpaOrchestrator orchestrator(executor, *repo);

  const std::array<WpaComponentKind, 1> components = {
      WpaComponentKind::kReachability};
  WpaRunRequest request;
  request.run = MakeManifest(facts::EngineIdentity::kSouffle);
  request.summaries = program;
  request.components = components;

  auto result = orchestrator.Run(request);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(order.size(), scc->ReverseTopologicalOrder().size());
  EXPECT_EQ(order,
            std::vector<core::StableId>(scc->ReverseTopologicalOrder().begin(),
                                        scc->ReverseTopologicalOrder().end()));

  std::filesystem::remove_all(db);
}

TEST(WpaOrchestratorTest, FailedComponentPublishesNoResult) {
  const auto program = ChainProgram();
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  FailingExecutor executor;
  WpaOrchestrator orchestrator(executor, *repo);

  const std::array<WpaComponentKind, 1> components = {
      WpaComponentKind::kReachability};
  WpaRunRequest request;
  request.run = MakeManifest(facts::EngineIdentity::kSouffle);
  request.summaries = program;
  request.components = components;

  auto result = orchestrator.Run(request);
  EXPECT_FALSE(result.ok());

  auto status = repo->RunStatus(request.run.run_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, WpaRunStatus::kIncomplete);

  std::filesystem::remove_all(db);
}

// A first run publishes every component's externally visible hash, so the
// chain's callers are scheduled. Re-running the same input changes nothing, so
// no predecessor is scheduled again.
TEST(WpaOrchestratorTest, RepeatedRunSchedulesNoPredecessors) {
  const auto program = ChainProgram();
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());
  SccStateRepository scc_state(repo->metadata_store());

  std::vector<core::StableId> order;
  RecordingExecutor executor(order);
  WpaOrchestrator orchestrator(executor, *repo, &scc_state);

  const std::array<WpaComponentKind, 1> components = {
      WpaComponentKind::kReachability};
  WpaRunRequest request;
  request.run = MakeManifest(facts::EngineIdentity::kSouffle);
  request.summaries = program;
  request.components = components;

  // The V1 scheduler's component-state table references repositories,
  // revisions, and build variants, so seed them for the run's context.
  ASSERT_TRUE(repo->metadata_store()
                  .Execute("INSERT INTO repositories(repository_id, vcs_kind, "
                           "vcs_revision, source_tree_hash) VALUES(?, ?, ?, ?)",
                           {"repo:test", "git", "r", "tree"})
                  .ok());
  ASSERT_TRUE(repo->metadata_store()
                  .Execute("INSERT INTO revisions(revision_id, repository_id, "
                           "vcs_revision) VALUES(?, ?, ?)",
                           {core::ToString(request.run.revision_id), "repo:test",
                            "r"})
                  .ok());
  ASSERT_TRUE(repo->metadata_store()
                  .Execute("INSERT INTO build_variants(build_variant_id, "
                           "target_triple, compiler_id, compiler_version, "
                           "compile_options_hash, macro_set_hash, "
                           "include_closure_hash, type_layout_hash) VALUES("
                           "?, ?, ?, ?, ?, ?, ?, ?)",
                           {core::ToString(request.run.build_variant_id), "arm64",
                            "clang", "24", "a", "b", "c", "d"})
                  .ok());

  auto first = orchestrator.Run(request);
  ASSERT_TRUE(first.ok()) << first.status().message();
  EXPECT_FALSE(first->scheduled_predecessors.empty());

  auto second = orchestrator.Run(request);
  ASSERT_TRUE(second.ok());
  EXPECT_TRUE(second->scheduled_predecessors.empty());

  std::filesystem::remove_all(db);
}

// Reachability and MemoryEffects run together over a chain a -> b -> c; each
// function writes a distinct memory so MemoryEffects has a local root too.
// After both components complete for b, a's Reachability successor support must
// still contain b's ReachableCall(b, c) rather than being overwritten by b's
// later MemoryEffects completion.
TEST(WpaOrchestratorTest, TwoComponentsPreserveReachabilitySupport) {
  auto a = V2Summary("a");
  AddCall(&a, "a", "b");
  AddWrite(&a, "ma");
  auto b = V2Summary("b");
  AddCall(&b, "b", "c");
  AddWrite(&b, "mb");
  auto c = V2Summary("c");
  AddWrite(&c, "mc");
  const std::vector<summary::SummaryArtifact> program = {a, b, c};

  auto graph = CallGraph::FromSummaries(program);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());

  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  FactEmittingExecutor executor;
  WpaOrchestrator orchestrator(executor, *repo);

  const std::array<WpaComponentKind, 2> components = {
      WpaComponentKind::kReachability, WpaComponentKind::kMemoryEffects};
  WpaRunRequest request;
  request.run = MakeManifest(facts::EngineIdentity::kSouffle);
  request.summaries = program;
  request.components = components;

  auto result = orchestrator.Run(request);
  ASSERT_TRUE(result.ok()) << result.status().message();

  auto scc_a = scc->SccForFunction(FunctionId("a"));
  ASSERT_TRUE(scc_a.ok());

  const FactEmittingExecutor::Observation* a_reach = nullptr;
  for (const auto& obs : executor.observations()) {
    if (obs.scc_id == *scc_a &&
        obs.component == WpaComponentKind::kReachability) {
      a_reach = &obs;
    }
  }
  ASSERT_NE(a_reach, nullptr);

  // Successor support is re-projected into the support relation by the
  // materializer (kSupportReachableCall mirrors kReachableCall's columns).
  bool saw_reachable_bc = false;
  for (const auto& root : a_reach->successor_roots) {
    const auto& row = root.row;
    if (row.relation == facts::RelationId::kSupportReachableCall &&
        std::get<core::StableId>(row.cells[0]) == FunctionId("b") &&
        std::get<core::StableId>(row.cells[1]) == FunctionId("c")) {
      saw_reachable_bc = true;
    }
  }
  EXPECT_TRUE(saw_reachable_bc)
      << "a's reachability successor support lost b's reachable call";

  std::filesystem::remove_all(db);
}

}  // namespace
}  // namespace veritas::wpa
