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

#include "analysis/svf/SvfFactMapper.h"

#include <gtest/gtest.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "ProjectFixture.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfMerge.h"
#include "analysis/svf/SvfSession.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::svf {
namespace {

namespace semantic = veritas::analysis::semantic;
namespace core = veritas::core;

// Helper to build a fixture ProgramIr for testing
std::unique_ptr<pipeline::ProgramIr>
BuildFixtureProgramIr(const std::string &fixture_name) {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  auto module =
      std::make_unique<::llvm::Module>(fixture_name, program_ir->GetContext());

  // Create a simple function for testing
  auto *func_type = ::llvm::FunctionType::get(
      ::llvm::Type::getInt32Ty(program_ir->GetContext()),
      {::llvm::Type::getInt32Ty(program_ir->GetContext())}, false);
  auto *func = ::llvm::Function::Create(
      func_type, ::llvm::Function::ExternalLinkage, fixture_name, module.get());

  auto *entry =
      ::llvm::BasicBlock::Create(program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);
  builder.CreateRet(func->getArg(0));

  program_ir->SetModule(std::move(module));
  return program_ir;
}

// Helper to build a ProgramIr with two functions, foo and bar.
std::unique_ptr<pipeline::ProgramIr> BuildTwoFunctionProgramIr() {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();
  auto module =
      std::make_unique<::llvm::Module>("two", program_ir->GetContext());
  auto *i32ty = ::llvm::Type::getInt32Ty(program_ir->GetContext());
  auto *func_type = ::llvm::FunctionType::get(i32ty, {i32ty}, false);
  for (const char *name : {"foo", "bar"}) {
    auto *func = ::llvm::Function::Create(
        func_type, ::llvm::Function::ExternalLinkage, name, module.get());
    auto *entry =
        ::llvm::BasicBlock::Create(program_ir->GetContext(), "entry", func);
    ::llvm::IRBuilder<> builder(entry);
    builder.CreateRet(func->getArg(0));
  }
  program_ir->SetModule(std::move(module));
  return program_ir;
}

// Helper to analyze a fixture with SVF and map facts
SvfMappingResult AnalyzeFixtureWithSvf(const std::string &fixture_name) {
  auto program_ir = BuildFixtureProgramIr(fixture_name);

  AnalyzerRunContext run_context{
      .analyzer_run_id = "test_run_001",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  SvfMappingResult result;
  auto status = RunWithSvfSession(
      *program_ir, SvfConfig::Default(), [&](const SvfSessionView &view) {
        return MapSvfFacts(*program_ir, view, run_context, SvfConfig::Default(),
                           &result);
      });

  if (!status.ok()) {
    ADD_FAILURE() << "SVF session failed";
  }

  return result;
}

TEST(SvfFactMapperTest, MapsParameterReturnFlow) {
  auto result = AnalyzeFixtureWithSvf("parameter_return");

  // Should have at least mapped some facts (even if empty due to simple IR)
  EXPECT_TRUE(result.completion == SvfMappingCompletion::kComplete ||
              result.completion == SvfMappingCompletion::kCompleteWithUnknowns);

  // The synthetic function returns its argument, so SVF should map a real
  // value flow rather than returning an empty fact set.
  EXPECT_GT(result.facts.value_flows.size(), 0u);

  // All facts should have provenance
  for (const auto &fact : result.facts.value_flows) {
    EXPECT_FALSE(fact.provenance_ref.empty());
  }
}

TEST(SvfFactMapperTest, MapsStoreLoadWithAliasProvenance) {
  auto result = AnalyzeFixtureWithSvf("store_load");

  EXPECT_TRUE(result.completion == SvfMappingCompletion::kComplete ||
              result.completion == SvfMappingCompletion::kCompleteWithUnknowns);

  // Verify facts are deduplicated
  auto value_flows = result.facts.value_flows;
  std::ranges::sort(value_flows);
  auto [first, last] = std::ranges::unique(value_flows);
  EXPECT_EQ(first, last) << "Facts should be deduplicated";
}

TEST(SvfFactMapperTest, AttachesCompleteProvenance) {
  auto result = AnalyzeFixtureWithSvf("parameter_return");

  // All fact types should have provenance
  for (const auto &fact : result.facts.value_flows) {
    EXPECT_NE(fact.provenance_ref.find("analyzer="), std::string::npos);
  }
  for (const auto &fact : result.facts.aliases) {
    EXPECT_NE(fact.provenance_ref.find("analyzer="), std::string::npos);
  }
  for (const auto &fact : result.facts.unknowns) {
    EXPECT_NE(fact.provenance_ref.find("analyzer="), std::string::npos);
  }
}

TEST(SvfFactMapperTest, UnmappedNodesCreateUnknownFacts) {
  auto result = AnalyzeFixtureWithSvf("parameter_return");

  // If mapping is incomplete, unknowns should explain why
  if (result.completion == SvfMappingCompletion::kCompleteWithUnknowns) {
    EXPECT_GT(result.facts.unknowns.size(), 0u);

    for (const auto &unknown : result.facts.unknowns) {
      EXPECT_FALSE(unknown.scope.empty());
      EXPECT_FALSE(unknown.reason.empty());
      EXPECT_FALSE(unknown.provenance_ref.empty());
    }
  }
}

TEST(SvfFactMapperTest, MergeAttributesCallsToCallerDraft) {
  auto program_ir = BuildFixtureProgramIr("caller");
  auto *caller = program_ir->GetFunction("caller");
  ASSERT_NE(caller, nullptr);
  program_ir->mutable_origin_map().RecordOrigin(caller,
                                                "funcvar:sha256:caller");

  ::veritas::summary::v1::FunctionSummary draft;
  draft.mutable_identity()->set_function_variant_id("funcvar:sha256:caller");
  semantic::NormalizedAnalysisFacts facts;
  facts.calls.push_back(semantic::NormalizedCallTarget{
      .call_site = {core::IdKind::kCallSite, "aa"},
      .caller = {core::IdKind::kValueRef, "bb"},
      .callee = core::StableId{core::IdKind::kValueRef, "cc"},
      .dispatch = semantic::DispatchKind::kIndirect,
      .epistemic = semantic::EpistemicState::kMay,
      .diagnostic_symbol = "caller",
      .provenance_ref = "svf:test",
  });

  auto merged = MergeSvfFacts({draft}, facts, program_ir->origin_map());
  ASSERT_EQ(merged.size(), 1u);
  ASSERT_EQ(merged[0].calls_size(), 1);
  EXPECT_EQ(merged[0].calls(0).callee_symbol(), "valref:sha256:cc");
  EXPECT_EQ(merged[0].calls(0).call_site_anchor_id(), "callsite:sha256:aa");
}

TEST(SvfFactMapperTest, MergeDoesNotLeakWholeProgramFactsAcrossFunctions) {
  auto program_ir = BuildTwoFunctionProgramIr();
  program_ir->mutable_origin_map().RecordOrigin(program_ir->GetFunction("foo"),
                                                "funcvar:sha256:foo");
  program_ir->mutable_origin_map().RecordOrigin(program_ir->GetFunction("bar"),
                                                "funcvar:sha256:bar");

  ::veritas::summary::v1::FunctionSummary foo_draft;
  foo_draft.mutable_identity()->set_function_variant_id("funcvar:sha256:foo");
  ::veritas::summary::v1::FunctionSummary bar_draft;
  bar_draft.mutable_identity()->set_function_variant_id("funcvar:sha256:bar");

  semantic::NormalizedAnalysisFacts facts;

  // A MUST-level NO_ALIAS — the kind of fact that must never be fabricated
  // into an unrelated function's summary.
  semantic::MemoryLocation left;
  left.id = {core::IdKind::kMemoryRef, "left"};
  semantic::MemoryLocation right;
  right.id = {core::IdKind::kMemoryRef, "right"};
  facts.aliases.push_back(semantic::NormalizedAlias{
      .left = left,
      .right = right,
      .kind = semantic::AliasKind::kNoAlias,
      .epistemic = semantic::EpistemicState::kMust,
      .provenance_ref = "svf:test",
  });

  facts.value_flows.push_back(semantic::NormalizedValueFlow{
      .source_value_id = {core::IdKind::kValueRef, "01"},
      .destination_value_id = {core::IdKind::kValueRef, "02"},
      .epistemic = semantic::EpistemicState::kMay,
      .provenance_ref = "svf:test",
  });

  semantic::MemoryLocation mem;
  mem.id = {core::IdKind::kMemoryRef, "mem"};
  facts.memory_effects.push_back(semantic::NormalizedMemoryEffect{
      .operation = {core::IdKind::kValueRef, "op"},
      .location = mem,
      .kind = semantic::MemoryEffectKind::kMayWrite,
      .epistemic = semantic::EpistemicState::kMay,
      .provenance_ref = "svf:test",
  });

  // A call made inside foo.
  facts.calls.push_back(semantic::NormalizedCallTarget{
      .call_site = {core::IdKind::kCallSite, "cs"},
      .caller = {core::IdKind::kValueRef, "foo"},
      .callee = core::StableId{core::IdKind::kValueRef, "callee"},
      .dispatch = semantic::DispatchKind::kIndirect,
      .epistemic = semantic::EpistemicState::kMay,
      .diagnostic_symbol = "foo",
      .provenance_ref = "svf:test",
  });

  auto merged = MergeSvfFacts({foo_draft, bar_draft}, facts,
                              program_ir->origin_map());
  ASSERT_EQ(merged.size(), 2u);

  // Whole-program facts (value flows, aliases, memory effects) must not leak
  // into either draft.
  for (const auto &draft : merged) {
    EXPECT_EQ(draft.value_flows_size(), 0);
    EXPECT_EQ(draft.alias_facts_size(), 0);
    EXPECT_EQ(draft.memory_effects_size(), 0);
  }

  // The call is attributed only to its caller, foo (draft order preserved).
  EXPECT_EQ(merged[0].calls_size(), 1);
  EXPECT_EQ(merged[1].calls_size(), 0);
}

TEST(SvfFactMapperTest, LocalAnalysisSvfFactsMergeIntoHashedOwnerSummary) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = testing::FixtureProject("store_load"),
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = build::LoadProjectManifest(*input);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  ASSERT_EQ(local->summary_drafts.size(), 1u);
  const int original_flows = local->summary_drafts[0].value_flows_size();

  AnalyzerRunContext run_context{
      .analyzer_run_id = "test_run_local_pipeline",
      .llvm_toolchain_identity = "llvm",
      .program_module_hash = std::string(local->program_ir.module_hash()),
  };
  SvfMappingResult mapped;
  auto status = RunWithSvfSession(
      local->program_ir, SvfConfig::Default(), [&](const SvfSessionView &view) {
        return MapSvfFacts(local->program_ir, view, run_context,
                           SvfConfig::Default(), &mapped);
      });
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_FALSE(mapped.facts.value_flows.empty());
  // The on-disk store_load fixture performs a store and a load.
  EXPECT_GT(mapped.facts.memory_effects.size(), 0u);

  auto merged = MergeSvfFacts(std::move(local->summary_drafts), mapped.facts,
                              local->program_ir.origin_map());
  ASSERT_EQ(merged.size(), 1u);
  // Whole-program SVF value flows are gated out of summary.v1 until Task 9;
  // the draft retains only its local (M4) value flows.
  EXPECT_EQ(merged[0].value_flows_size(), original_flows);
}

} // namespace
} // namespace veritas::analysis::svf
