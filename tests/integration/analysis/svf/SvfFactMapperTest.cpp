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

#include <filesystem>

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
#include "veritas/analysis/semantic/ModelBundle.h"
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

  const auto model_dir =
      testing::TestSourceRoot().parent_path() / "logic" / "models";
  auto bundle = semantic::ModelBundle::Load(model_dir / "models.v1.tsv",
                                            model_dir / "models.v1.manifest");
  ASSERT_TRUE(bundle.ok()) << bundle.status().message();

  auto merged = MergeSvfFactsV2(std::move(local->summary_drafts),
                                mapped.facts, *bundle);
  ASSERT_TRUE(merged.ok()) << merged.status().message();
  ASSERT_EQ(merged->size(), 1u);
  // Whole-program SVF value flows carry no recoverable per-function owner, so
  // they stay gated out of the merged summary.v2; the draft retains only its
  // local (M4) value flows.
  EXPECT_EQ((*merged)[0].value_flows_size(), original_flows);
}

// Local extraction and SVF both observe the same store. Since memory identity
// no longer includes the byte range, they now agree on memory_location_id --
// and that exposed a disagreement the differing ids had been hiding: local
// states the effect MUST (LocalFactExtractor), SVF states it MAY
// (SvfFactMapper). Publishing both derives MayWrite(f, o, MUST) and
// MayWrite(f, o, MAY) for one object, where the first strictly implies the
// second, leaving "does f must-write o" with two answers.
//
// The weaker duplicate is dropped. An SVF effect for a location the local
// pass never reported is still kept: this suppresses redundancy, not
// discovery.
TEST(SvfMergeTest, DropsSvfEffectAlreadyStatedMoreStronglyByLocalPass) {
  const auto function_id = core::MakeStableId(
      core::IdKind::kFunctionVariant, std::as_bytes(std::span("f", 1)));
  const auto shared = core::MakeStableId(core::IdKind::kMemoryRef,
                                         std::as_bytes(std::span("shared", 6)));
  const auto svf_only = core::MakeStableId(
      core::IdKind::kMemoryRef, std::as_bytes(std::span("svfonly", 7)));
  const auto object_id = core::MakeStableId(core::IdKind::kAbstractObject,
                                            std::as_bytes(std::span("obj", 3)));
  const auto operation = core::MakeStableId(core::IdKind::kValueRef,
                                            std::as_bytes(std::span("op", 2)));

  summary::v2::FunctionSummary draft;
  draft.mutable_header()->set_schema_version("summary.v2");
  draft.mutable_identity()->set_function_variant_id(core::ToString(function_id));
  auto* local = draft.add_memory_effects();
  local->set_kind(summary::v1::EFFECT_KIND_WRITE);
  local->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);
  local->mutable_location()->set_memory_location_id(core::ToString(shared));

  auto MakeEffect = [&](const core::StableId& location_id) {
    semantic::NormalizedMemoryEffect effect;
    effect.operation = operation;
    effect.location.id = location_id;
    effect.location.object.id = object_id;
    effect.location.object.kind = semantic::AbstractObjectKind::kHeap;
    effect.location.object.owner_function = function_id;
    effect.location.byte_range = semantic::ByteRange::Unknown();
    effect.kind = semantic::MemoryEffectKind::kMayWrite;
    effect.epistemic = semantic::EpistemicState::kMay;
    effect.provenance_ref = "svf:store";
    return effect;
  };

  semantic::NormalizedAnalysisFacts facts;
  facts.memory_effects.push_back(MakeEffect(shared));
  facts.memory_effects.push_back(MakeEffect(svf_only));

  const auto model_dir =
      testing::TestSourceRoot().parent_path() / "logic" / "models";
  auto bundle = semantic::ModelBundle::Load(model_dir / "models.v1.tsv",
                                            model_dir / "models.v1.manifest");
  ASSERT_TRUE(bundle.ok()) << bundle.status().message();

  std::vector<summary::v2::FunctionSummary> drafts;
  drafts.push_back(std::move(draft));
  auto merged = MergeSvfFactsV2(std::move(drafts), facts, *bundle);
  ASSERT_TRUE(merged.ok()) << merged.status().message();
  ASSERT_EQ(merged->size(), 1u);

  int shared_count = 0;
  int svf_only_count = 0;
  for (const auto& effect : (*merged)[0].memory_effects()) {
    if (effect.location().memory_location_id() == core::ToString(shared))
      ++shared_count;
    if (effect.location().memory_location_id() == core::ToString(svf_only))
      ++svf_only_count;
  }
  EXPECT_EQ(shared_count, 1) << "weaker SVF duplicate was not dropped";
  EXPECT_EQ((*merged)[0].memory_effects(0).epistemic(),
            summary::v1::EPISTEMIC_STATE_MUST);
  EXPECT_EQ(svf_only_count, 1) << "SVF-only discovery must be preserved";
}

}  // namespace
} // namespace veritas::analysis::svf
