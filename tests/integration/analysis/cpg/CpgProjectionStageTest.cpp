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

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "ProjectFixture.h"
#include "analysis/cpg/CpgProjectionStage.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "cpg/CpgCanonicalizer.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"

namespace veritas::analysis::cpg {
namespace {

namespace cpg_t = ::veritas::cpg;

StatusOr<build::AnalysisManifest> LoadFixtureManifest(std::string_view name) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = testing::FixtureProject(name),
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok())
    return input.status();
  return build::LoadProjectManifest(*input);
}

core::StableId MakeId(core::IdKind kind, std::string_view seed) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(seed.data(), seed.size())));
}

::llvm::Function *FirstDefinition(pipeline::ProgramIr *program_ir) {
  for (auto &function : *program_ir->GetModule()) {
    if (!function.isDeclaration())
      return &function;
  }
  return nullptr;
}

TEST(CpgProjectionStageTest, BuildsFromLiveProgramIrAndSummaries) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  bool has_function = false;
  bool has_summary = false;
  bool has_flows_to = false;
  bool has_reads_writes = false;
  for (const auto &node : graph->nodes()) {
    has_function |= (node.kind == cpg_t::NodeKind::kFunction);
    has_summary |= (node.kind == cpg_t::NodeKind::kSummary);
  }
  for (const auto &edge : graph->edges()) {
    has_flows_to |= (edge.kind == cpg_t::EdgeKind::kFlowsTo);
    has_reads_writes |= (edge.kind == cpg_t::EdgeKind::kReads ||
                         edge.kind == cpg_t::EdgeKind::kWrites);
  }
  EXPECT_TRUE(has_function);
  EXPECT_TRUE(has_summary);
  EXPECT_TRUE(has_flows_to);
  EXPECT_TRUE(has_reads_writes);
}

TEST(CpgProjectionStageTest, IsDeterministicAcrossRebuilds) {
  auto first_manifest = LoadFixtureManifest("store_load");
  auto second_manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(first_manifest.ok());
  ASSERT_TRUE(second_manifest.ok());

  auto first = pipeline::RunLocalAnalysis(*first_manifest);
  auto second = pipeline::RunLocalAnalysis(*second_manifest);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  auto first_graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = first->program_ir,
      .completed_summaries = first->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });
  auto second_graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = second->program_ir,
      .completed_summaries = second->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });
  ASSERT_TRUE(first_graph.ok());
  ASSERT_TRUE(second_graph.ok());

  EXPECT_EQ(::veritas::cpg::CpgCanonicalizer::CanonicalBytes(*first_graph),
            ::veritas::cpg::CpgCanonicalizer::CanonicalBytes(*second_graph));
}

TEST(CpgProjectionStageTest, FunctionNodeIdEqualsOriginMapFunctionVariantId) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  std::size_t checked_functions = 0;
  for (const auto &function : *local->program_ir.GetModule()) {
    if (function.isDeclaration())
      continue;
    ++checked_functions;
    auto origin_id = local->program_ir.origin_map().GetSymbolId(&function);
    ASSERT_TRUE(origin_id.has_value());
    auto parsed = core::ParseStableId(*origin_id);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_TRUE(std::ranges::any_of(graph->nodes(), [&](const cpg_t::CpgNode
                                                            &node) {
      return node.kind == cpg_t::NodeKind::kFunction && node.node_id == *parsed;
    })) << *origin_id;
  }
  EXPECT_GT(checked_functions, 0u);
}

TEST(CpgProjectionStageTest, ResolvedCallTargetsFunctionVariantNode) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  bool checked_call = false;
  for (const auto &summary : local->summary_drafts) {
    auto caller = core::ParseStableId(summary.identity().function_variant_id());
    ASSERT_TRUE(caller.ok()) << caller.status().message();
    for (const auto &call : summary.calls()) {
      if (call.resolved_callee_function_variant_id().empty())
        continue;
      auto callee =
          core::ParseStableId(call.resolved_callee_function_variant_id());
      ASSERT_TRUE(callee.ok()) << callee.status().message();
      checked_call = true;
      EXPECT_TRUE(std::ranges::any_of(graph->nodes(), [&](const auto &node) {
        return node.kind == cpg_t::NodeKind::kFunction &&
               node.node_id == *callee;
      }));
      EXPECT_TRUE(std::ranges::any_of(graph->edges(), [&](const auto &edge) {
        return edge.source_node_id == *caller &&
               edge.target_node_id == *callee &&
               (edge.kind == cpg_t::EdgeKind::kCalls ||
                edge.kind == cpg_t::EdgeKind::kMayCall);
      }));
    }
  }
  EXPECT_TRUE(checked_call);
}

TEST(CpgProjectionStageTest, RejectsMalformedOriginMapFunctionIdentity) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  auto *function = FirstDefinition(&local->program_ir);
  ASSERT_NE(function, nullptr);
  local->program_ir.mutable_origin_map().RecordOrigin(function,
                                                      "not-a-stable-id");

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });

  ASSERT_FALSE(graph.ok());
  EXPECT_EQ(graph.status().code(), StatusCode::kInvalidArgument);
}

TEST(CpgProjectionStageTest, RejectsNonHexOriginMapFunctionDigest) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  auto *function = FirstDefinition(&local->program_ir);
  ASSERT_NE(function, nullptr);
  local->program_ir.mutable_origin_map().RecordOrigin(
      function, "funcvar:sha256:" + std::string(64, 'g'));

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });

  ASSERT_FALSE(graph.ok());
  EXPECT_EQ(graph.status().code(), StatusCode::kInvalidArgument);
}

TEST(CpgProjectionStageTest, RejectsWrongKindOriginMapFunctionIdentity) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  auto *function = FirstDefinition(&local->program_ir);
  ASSERT_NE(function, nullptr);
  local->program_ir.mutable_origin_map().RecordOrigin(
      function, core::ToString(MakeId(core::IdKind::kRevision, "wrong-kind")));

  auto graph = BuildThinCpg(CpgProjectionInput{
      .program_ir = local->program_ir,
      .completed_summaries = local->summary_drafts,
      .revision_id = core::StableId{core::IdKind::kRevision, "rev"},
      .build_variant_id = core::StableId{core::IdKind::kBuildVariant, "bv"},
  });

  ASSERT_FALSE(graph.ok());
  EXPECT_EQ(graph.status().code(), StatusCode::kInvalidArgument);
}

} // namespace
} // namespace veritas::analysis::cpg
