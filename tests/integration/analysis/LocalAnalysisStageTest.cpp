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

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <set>
#include <string>
#include <string_view>

#include "ProjectFixture.h"
#include "analysis/llvm/ProjectIrBuilder.h"
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
  if (!input.ok())
    return input.status();
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

  for (const auto &draft : result->summary_drafts) {
    EXPECT_FALSE(draft.identity().function_variant_id().empty());
    EXPECT_FALSE(draft.identity().revision_id().empty());
  }
}

TEST(LocalAnalysisStageTest, ResolvesSystemHeadersWithoutExplicitSysroot) {
  auto manifest = LoadFixtureManifest("system_headers");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  ASSERT_EQ(result->summary_drafts.size(), 1u);
}

TEST(LocalAnalysisStageTest, ContextRetainsValueNames) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  // Clang's driver emits `-discard-value-names` when built against a
  // non-asserts LLVM, which would leave this context unable to parse the
  // textual IR (extapi.bc) that SVF loads into it. VERITAS must re-assert the
  // retain-names invariant after codegen, independent of the LLVM build.
  EXPECT_FALSE(result->program_ir.GetContext().shouldDiscardValueNames());
}

TEST(LocalAnalysisStageTest, ExtractsMemoryEffectsAndValueFlows) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ASSERT_EQ(result->summary_drafts.size(), 1u);

  const auto &draft = result->summary_drafts[0];
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

  const summary::v2::Call *add_call = nullptr;
  for (const auto &draft : result->summary_drafts) {
    for (const auto &call : draft.calls()) {
      if (call.callee_symbol().find("add") != std::string::npos) {
        add_call = &call;
      }
    }
  }
  ASSERT_NE(add_call, nullptr);
  auto parsed =
      core::ParseStableId(add_call->resolved_callee_function_variant_id());
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->kind, core::IdKind::kFunctionVariant);
  auto call_site = core::ParseStableId(add_call->call_site_id());
  ASSERT_TRUE(call_site.ok()) << call_site.status().message();
  EXPECT_EQ(call_site->kind, core::IdKind::kCallSite);
}

TEST(LocalAnalysisStageTest, FunctionVariantsIncludeBuildVariantIdentity) {
  auto first_manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(first_manifest.ok()) << first_manifest.status().message();
  auto second_manifest = *first_manifest;
  const auto alternate_build =
      core::MakeStableId(core::IdKind::kBuildVariant,
                         std::as_bytes(std::span("alternate-build", 15)));
  second_manifest.context.build_variant_id = core::ToString(alternate_build);
  for (auto &unit : second_manifest.translation_units) {
    unit.build_variant_id = second_manifest.context.build_variant_id;
  }

  auto first = RunLocalAnalysis(*first_manifest);
  auto second = RunLocalAnalysis(second_manifest);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  ASSERT_EQ(first->summary_drafts.size(), second->summary_drafts.size());

  for (std::size_t i = 0; i < first->summary_drafts.size(); ++i) {
    EXPECT_NE(first->summary_drafts[i].identity().function_variant_id(),
              second->summary_drafts[i].identity().function_variant_id());
  }
}

TEST(LocalAnalysisStageTest, InternalFunctionsIncludeTranslationUnitIdentity) {
  auto manifest = LoadFixtureManifest("internal_linkage");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  std::set<std::string> helper_ids;
  for (const auto &function : *result->program_ir.GetModule()) {
    if (!function.hasInternalLinkage() ||
        function.getName().find("helper") == std::string_view::npos) {
      continue;
    }
    auto id = result->program_ir.origin_map().GetSymbolId(&function);
    ASSERT_TRUE(id.has_value());
    helper_ids.insert(*id);
  }
  EXPECT_EQ(helper_ids.size(), 2u);
}

TEST(LocalAnalysisStageTest, PrivateFunctionsIncludeTranslationUnitIdentity) {
  ::llvm::LLVMContext context;
  ::llvm::Module first_module("first", context);
  ::llvm::Module second_module("second", context);
  auto *function_type =
      ::llvm::FunctionType::get(::llvm::Type::getVoidTy(context), false);
  auto *first = ::llvm::Function::Create(function_type,
                                         ::llvm::GlobalValue::PrivateLinkage,
                                         "helper", first_module);
  auto *second = ::llvm::Function::Create(function_type,
                                          ::llvm::GlobalValue::PrivateLinkage,
                                          "helper", second_module);
  const build::ProgramContext program_context{
      .repository_id = core::ToString(core::MakeStableId(
          core::IdKind::kRepository, std::as_bytes(std::span("repo", 4))))};
  const build::TranslationUnitCommand first_command{
      .translation_unit_id = "translation-unit:first"};
  const build::TranslationUnitCommand second_command{
      .translation_unit_id = "translation-unit:second"};

  const auto first_id = ::veritas::analysis::llvm::detail::FunctionSymbolId(
      *first, first_command, program_context);
  const auto second_id = ::veritas::analysis::llvm::detail::FunctionSymbolId(
      *second, second_command, program_context);
  EXPECT_NE(first_id, second_id);
}

} // namespace

// Pins the callee symbols real extraction produces for the three functions the
// model bundle describes. `malloc` and `free` survive under their own names,
// but Clang lowers `memcpy` to the intrinsic `llvm.memcpy.p0.p0.i64`, so a
// bundle keyed on `memcpy` cannot match it by exact symbol. That mismatch is
// invisible without a fixture that actually calls these functions -- the model
// simply contributes nothing and no error is raised.
TEST(LocalAnalysisStageTest, LowersMemcpyToAnIntrinsicCalleeSymbol) {
  auto manifest = LoadFixtureManifest("modeled_calls");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  std::set<std::string> callees;
  for (const auto &draft : result->summary_drafts) {
    for (const auto &call : draft.calls())
      callees.insert(call.callee_symbol());
  }

  EXPECT_TRUE(callees.contains("malloc"));
  EXPECT_TRUE(callees.contains("free"));
  EXPECT_FALSE(callees.contains("memcpy"))
      << "if Clang stops lowering memcpy, the model normalization fallback "
         "is no longer exercised by this fixture";
  EXPECT_TRUE(callees.contains("llvm.memcpy.p0.p0.i64"));
}

} // namespace veritas::analysis::pipeline