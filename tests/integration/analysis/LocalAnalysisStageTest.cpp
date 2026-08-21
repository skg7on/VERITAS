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

#include <string>
#include <string_view>

#include "ProjectFixture.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::pipeline {
namespace {

StatusOr<build::AnalysisManifest> LoadFixtureManifest(std::string_view name) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = testing::FixtureProject(name),
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok()) return input.status();
  return build::LoadProjectManifest(*input);
}

TEST(LocalAnalysisStageTest, BuildsLinkedProgramIrAndSummaryDrafts) {
  auto manifest = LoadFixtureManifest("multiple_tus");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  EXPECT_EQ(result->program_ir.translation_unit_count(), 2u);
  EXPECT_FALSE(result->program_ir.module_hash().empty());
  ASSERT_EQ(result->summary_drafts.size(), 2u);

  for (const auto& draft : result->summary_drafts) {
    EXPECT_FALSE(draft.identity().function_variant_id().empty());
    EXPECT_FALSE(draft.identity().revision_id().empty());
  }
}

TEST(LocalAnalysisStageTest, ExtractsMemoryEffectsAndValueFlows) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ASSERT_EQ(result->summary_drafts.size(), 1u);

  const auto& draft = result->summary_drafts[0];
  EXPECT_GT(draft.value_flows_size(), 0);
  EXPECT_GT(draft.memory_effects_size(), 0);
}

TEST(LocalAnalysisStageTest, IdenticalInputsProduceDeterministicDrafts) {
  auto first_manifest = LoadFixtureManifest("multiple_tus");
  auto second_manifest = LoadFixtureManifest("multiple_tus");
  ASSERT_TRUE(first_manifest.ok());
  ASSERT_TRUE(second_manifest.ok());

  auto first = RunLocalAnalysis(*first_manifest);
  auto second = RunLocalAnalysis(*second_manifest);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  EXPECT_EQ(first->program_ir.module_hash(), second->program_ir.module_hash());
  ASSERT_EQ(first->summary_drafts.size(), second->summary_drafts.size());
  for (std::size_t i = 0; i < first->summary_drafts.size(); ++i) {
    EXPECT_EQ(first->summary_drafts[i].SerializeAsString(),
              second->summary_drafts[i].SerializeAsString());
  }
}

TEST(LocalAnalysisStageTest, DirectCallCarriesResolvedFunctionVariantId) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  const summary::v1::Call* add_call = nullptr;
  for (const auto& draft : result->summary_drafts) {
    for (const auto& call : draft.calls()) {
      if (call.callee_symbol().find("add") != std::string::npos) {
        add_call = &call;
      }
    }
  }
  ASSERT_NE(add_call, nullptr);
  auto parsed = core::ParseStableId(
      add_call->resolved_callee_function_variant_id());
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->kind, core::IdKind::kFunctionVariant);
  auto call_site = core::ParseStableId(add_call->call_site_anchor_id());
  ASSERT_TRUE(call_site.ok()) << call_site.status().message();
  EXPECT_EQ(call_site->kind, core::IdKind::kCallSite);
}

}  // namespace
}  // namespace veritas::analysis::pipeline
