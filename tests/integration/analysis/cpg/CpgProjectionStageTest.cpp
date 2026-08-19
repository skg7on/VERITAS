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

#include <string_view>

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
  if (!input.ok()) return input.status();
  return build::LoadProjectManifest(*input);
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
  for (const auto& node : graph->nodes()) {
    has_function |= (node.kind == cpg_t::NodeKind::kFunction);
    has_summary |= (node.kind == cpg_t::NodeKind::kSummary);
  }
  for (const auto& edge : graph->edges()) {
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

}  // namespace
}  // namespace veritas::analysis::cpg
