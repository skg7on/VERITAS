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
#include <cstdlib>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "ProjectFixture.h"
#include "analysis/cpg/CpgProjectionStage.h"
#include "analysis/llvm/StableValueMapper.h"
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

// Catches ownership inferred from a summary's arbitrary cross-function flow
// endpoints instead of the live LLVM argument/instruction owner.
TEST(CpgProjectionStageTest, OwnsLocalValuesButNotForeignFlowEndpoints) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok());
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok()) << local.status().message();
  auto *function = FirstDefinition(&local->program_ir);
  ASSERT_NE(function, nullptr);
  ASSERT_FALSE(function->empty());
  llvm::StableValueMapper values(*local->program_ir.GetModule(),
                                 local->program_ir.origin_map());
  auto local_value = values.IdFor(function->front().front());
  auto owner = core::ParseStableId(
      *local->program_ir.origin_map().GetSymbolId(function));
  ASSERT_TRUE(local_value.ok());
  ASSERT_TRUE(owner.ok());
  const auto foreign_value = MakeId(core::IdKind::kValueRef, "foreign-value");
  auto *global = new ::llvm::GlobalVariable(
      *local->program_ir.GetModule(),
      ::llvm::Type::getInt32Ty(local->program_ir.GetContext()), false,
      ::llvm::GlobalValue::ExternalLinkage, nullptr, "shared_global");
  const auto global_value = *values.IdFor(*global);
  ASSERT_FALSE(local->summary_drafts.empty());
  auto *flow = local->summary_drafts.front().add_value_flows();
  flow->set_source_value_id(core::ToString(foreign_value));
  flow->set_destination_value_id(core::ToString(global_value));
  flow->set_epistemic(summary::v1::EPISTEMIC_STATE_MAY);

  auto graph = BuildThinCpg(
      CpgProjectionInput{local->program_ir, local->summary_drafts,
                         MakeId(core::IdKind::kRevision, "rev"),
                         MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  std::size_t local_owners = 0;
  for (const auto &edge : graph->edges()) {
    if (edge.kind != cpg_t::EdgeKind::kContains)
      continue;
    EXPECT_NE(edge.target_node_id, foreign_value);
    EXPECT_NE(edge.target_node_id, global_value);
    if (edge.target_node_id == *local_value) {
      ++local_owners;
      EXPECT_EQ(edge.source_node_id, *owner);
    }
  }
  EXPECT_EQ(local_owners, 1u);
}

TEST(CpgProjectionStageTest, OwnsOnlyProvablyLocalStackMemory) {
  auto manifest = LoadFixtureManifest("store_load");
  ASSERT_TRUE(manifest.ok());
  auto local = pipeline::RunLocalAnalysis(*manifest);
  ASSERT_TRUE(local.ok());
  auto graph = BuildThinCpg({local->program_ir, local->summary_drafts,
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok());
  std::size_t checked = 0;
  for (const auto &summary : local->summary_drafts) {
    for (const auto &effect : summary.memory_effects()) {
      if (effect.location().object().kind() !=
          summary::v2::ABSTRACT_OBJECT_KIND_STACK)
        continue;
      auto memory = core::ParseStableId(effect.location().memory_location_id());
      auto owner =
          core::ParseStableId(summary.identity().function_variant_id());
      ASSERT_TRUE(memory.ok());
      ASSERT_TRUE(owner.ok());
      EXPECT_TRUE(std::ranges::any_of(graph->edges(), [&](const auto &edge) {
        return edge.kind == cpg_t::EdgeKind::kContains &&
               edge.source_node_id == *owner && edge.target_node_id == *memory;
      }));
      ++checked;
    }
  }
  EXPECT_GT(checked, 0u);
}

pipeline::ProgramIr CopyProgram(bool insert_unrelated) {
  pipeline::ProgramIr ir;
  auto module = std::make_unique<::llvm::Module>("copy-calls", ir.GetContext());
  ::llvm::IRBuilder<> builder(ir.GetContext());
  auto *ptr = builder.getPtrTy();
  auto *size = builder.getInt64Ty();
  auto *copy_type = ::llvm::FunctionType::get(ptr, {ptr, ptr, size}, false);
  auto *copy = ::llvm::Function::Create(
      copy_type, ::llvm::GlobalValue::ExternalLinkage, "memcpy", *module);
  auto *other = ::llvm::Function::Create(
      ::llvm::FunctionType::get(builder.getVoidTy(), {ptr, size}, false),
      ::llvm::GlobalValue::ExternalLinkage, "other_exit", *module);
  auto *type =
      ::llvm::FunctionType::get(builder.getVoidTy(), {ptr, size}, false);
  const auto add_caller = [&](std::string_view name, bool copies) {
    auto *fn = ::llvm::Function::Create(
        type, ::llvm::GlobalValue::ExternalLinkage, name, *module);
    ir.mutable_origin_map().RecordOrigin(
        fn, core::ToString(name == "unrelated"
                               ? core::StableId{core::IdKind::kFunctionVariant,
                                                std::string(64, '0')}
                               : MakeId(core::IdKind::kFunctionVariant, name)));
    builder.SetInsertPoint(
        ::llvm::BasicBlock::Create(ir.GetContext(), "entry", fn));
    if (copies) {
      auto *dst =
          builder.CreateAlloca(builder.getInt8Ty(), builder.getInt64(32));
      builder.CreateCall(copy, {dst, fn->getArg(0), fn->getArg(1)});
    }
    builder.CreateCall(other, {fn->getArg(0), fn->getArg(1)});
    builder.CreateRetVoid();
  };
  if (insert_unrelated)
    add_caller("unrelated", false);
  add_caller("first", true);
  add_caller("second", true);
  ir.SetModule(std::move(module));
  return ir;
}

// Shared callee nodes are not call instructions. Each call must retain its
// actual arguments and owner, regardless of another caller's sort position.
TEST(CpgProjectionStageTest, TwoCallersKeepDistinctMemcpyArguments) {
  auto ir = CopyProgram(false);
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  std::size_t callsites = 0;
  for (const auto &node : graph->nodes()) {
    if (node.kind == cpg_t::NodeKind::kCallSite)
      ++callsites;
  }
  EXPECT_EQ(callsites, 4u); // two memcpy and two independent other_exit calls
  EXPECT_EQ(graph->metadata().schema_version, "thin-cpg.v2");
  ASSERT_EQ(graph->callsites().size(), 4u);
  llvm::StableValueMapper values(*ir.GetModule(), ir.origin_map());
  std::set<core::StableId> copies;
  for (const auto &binding : graph->callsites()) {
    auto *function = ir.origin_map().GetFunction(
        core::ToString(binding.function_variant_id));
    ASSERT_NE(function, nullptr);
    const ::llvm::CallBase *matched = nullptr;
    for (const auto &block : *function) {
      for (const auto &inst : block) {
        const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
        if (call && *values.CallSiteIdFor(*call) == binding.callsite_id)
          matched = call;
      }
    }
    ASSERT_NE(matched, nullptr);
    ASSERT_EQ(binding.arguments.size(), matched->arg_size());
    for (std::size_t i = 0; i < binding.arguments.size(); ++i) {
      EXPECT_EQ(binding.arguments[i].index, i);
      EXPECT_EQ(binding.arguments[i].value_ref,
                *values.IdFor(*matched->getArgOperand(i)));
    }
    if (matched->getCalledFunction()->getName() == "memcpy") {
      EXPECT_EQ(binding.normalized_operation, "memcpy.v1");
      ASSERT_TRUE(binding.arguments[0].memory_ref.has_value());
      copies.insert(binding.callsite_id);
    } else {
      EXPECT_TRUE(binding.normalized_operation.empty());
    }
  }
  EXPECT_EQ(copies.size(), 2u);

  auto *last = &ir.GetModule()->getFunctionList().back();
  last->removeFromParent();
  ir.GetModule()->getFunctionList().push_front(last);
  auto reordered = BuildThinCpg({ir,
                                 {},
                                 MakeId(core::IdKind::kRevision, "rev"),
                                 MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(reordered.ok());
  EXPECT_EQ(cpg_t::CpgCanonicalizer::CanonicalBytes(*reordered),
            cpg_t::CpgCanonicalizer::CanonicalBytes(*graph));

  auto changed_ir = CopyProgram(true);
  auto changed = BuildThinCpg({changed_ir,
                               {},
                               MakeId(core::IdKind::kRevision, "rev"),
                               MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(changed.ok()) << changed.status().message();
  for (const auto &binding : graph->callsites()) {
    auto found = std::ranges::find(changed->callsites(), binding.callsite_id,
                                   &cpg_t::CallSiteBinding::callsite_id);
    ASSERT_NE(found, changed->callsites().end());
    EXPECT_EQ(*found, binding);
  }
}

TEST(CpgProjectionStageTest, RejectsBindingAnArgumentFromAnotherCaller) {
  auto ir = CopyProgram(false);
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok());
  std::vector<cpg_t::CallSiteBinding> copies;
  for (const auto &binding : graph->callsites()) {
    if (binding.normalized_operation == "memcpy.v1")
      copies.push_back(binding);
  }
  ASSERT_EQ(copies.size(), 2u);
  auto changed_binding = copies[0];
  changed_binding.arguments[1] = copies[1].arguments[1];
  cpg_t::ThinCpg changed;
  changed.SetMetadata(graph->metadata());
  for (const auto &node : graph->nodes())
    ASSERT_TRUE(changed.AddNode(node).ok());
  for (const auto &edge : graph->edges())
    ASSERT_TRUE(changed.AddEdge(edge).ok());
  for (const auto &binding : graph->callsites()) {
    ASSERT_TRUE(changed
                    .AddCallSiteBinding(binding.callsite_id ==
                                                changed_binding.callsite_id
                                            ? changed_binding
                                            : binding)
                    .ok());
  }
  EXPECT_FALSE(changed.Validate().ok());
}

TEST(CpgProjectionStageTest, RejectsConflictingCompilerFunctionOwners) {
  auto ir = CopyProgram(false);
  ir.mutable_origin_map().RecordOrigin(
      ir.GetFunction("second"),
      *ir.origin_map().GetSymbolId(ir.GetFunction("first")));
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_FALSE(graph.ok());
  EXPECT_EQ(graph.status().code(), StatusCode::kFailedPrecondition);
}

TEST(CpgProjectionStageTest, RejectsIncompleteCompilerCallSiteCoverage) {
  auto ir = CopyProgram(false);
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok());
  cpg_t::ThinCpg incomplete;
  incomplete.SetMetadata(graph->metadata());
  for (const auto &node : graph->nodes())
    ASSERT_TRUE(incomplete.AddNode(node).ok());
  for (const auto &edge : graph->edges())
    ASSERT_TRUE(incomplete.AddEdge(edge).ok());
  ASSERT_GT(graph->callsites().size(), 1u);
  for (std::size_t i = 1; i < graph->callsites().size(); ++i) {
    ASSERT_TRUE(incomplete.AddCallSiteBinding(graph->callsites()[i]).ok());
  }
  EXPECT_FALSE(incomplete.Validate().ok());
}

TEST(CpgProjectionStageTest, FrozenBindingsSurviveCompilerNormalization) {
  auto ir = CopyProgram(false);
  auto compiler = CaptureCompilerProjection(ir);
  ASSERT_TRUE(compiler.ok());
  const auto revision = MakeId(core::IdKind::kRevision, "rev");
  const auto build = MakeId(core::IdKind::kBuildVariant, "build");
  auto before = BuildThinCpg({ir, {}, revision, build, &*compiler});
  ASSERT_TRUE(before.ok());
  for (auto &function : *ir.GetModule()) {
    if (function.isDeclaration())
      continue;
    ::llvm::IRBuilder<> builder(&function.front().front());
    builder.CreateAlloca(builder.getInt8Ty());
  }
  auto after = BuildThinCpg({ir, {}, revision, build, &*compiler});
  ASSERT_TRUE(after.ok()) << after.status().message();
  EXPECT_EQ(cpg_t::CpgCanonicalizer::CanonicalBytes(*before),
            cpg_t::CpgCanonicalizer::CanonicalBytes(*after));
  auto recomputed = BuildThinCpg({ir, {}, revision, build});
  ASSERT_TRUE(recomputed.ok());
  EXPECT_NE(before->callsites()[0].callsite_id,
            recomputed->callsites()[0].callsite_id);
  ir.SetModuleHash("another-module");
  EXPECT_FALSE(BuildThinCpg({ir, {}, revision, build, &*compiler}).ok());
}

void ScalableProjectionExit(bool passed_to_call) {
  pipeline::ProgramIr ir;
  auto module = std::make_unique<::llvm::Module>("scalable", ir.GetContext());
  ::llvm::IRBuilder<> builder(ir.GetContext());
  auto *function = ::llvm::Function::Create(
      ::llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getPtrTy(), builder.getInt64Ty()},
                                false),
      ::llvm::GlobalValue::ExternalLinkage, "scalable", *module);
  ir.mutable_origin_map().RecordOrigin(
      function,
      core::ToString(MakeId(core::IdKind::kFunctionVariant, "scalable")));
  builder.SetInsertPoint(
      ::llvm::BasicBlock::Create(ir.GetContext(), "entry", function));
  auto *storage = builder.CreateAlloca(
      ::llvm::ScalableVectorType::get(builder.getInt32Ty(), 4));
  if (passed_to_call) {
    auto *copy = ::llvm::Function::Create(
        ::llvm::FunctionType::get(
            builder.getPtrTy(),
            {builder.getPtrTy(), builder.getPtrTy(), builder.getInt64Ty()},
            false),
        ::llvm::GlobalValue::ExternalLinkage, "memcpy", *module);
    builder.CreateCall(copy,
                       {storage, function->getArg(0), function->getArg(1)});
  }
  builder.CreateRetVoid();
  if (::llvm::verifyModule(*module, &::llvm::errs()))
    std::_Exit(2);
  ir.SetModule(std::move(module));
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  if (!graph.ok())
    std::_Exit(3);
  for (const auto &edge : graph->edges()) {
    if (edge.kind == cpg_t::EdgeKind::kContains &&
        edge.target_node_id.kind == core::IdKind::kMemoryRef)
      std::_Exit(4);
  }
  if (passed_to_call && (graph->callsites().size() != 1 ||
                         !graph->callsites()[0].normalized_operation.empty() ||
                         graph->callsites()[0].arguments[0].memory_ref))
    std::_Exit(5);
  std::_Exit(0);
}

TEST(CpgProjectionStageTest, ScalableUnusedStorageStaysUnownedWithoutCrashing) {
  ASSERT_EXIT(ScalableProjectionExit(false), ::testing::ExitedWithCode(0),
              ".*");
}

TEST(CpgProjectionStageTest,
     ScalablePointerArgumentDoesNotAcquireCopyCapability) {
  ASSERT_EXIT(ScalableProjectionExit(true), ::testing::ExitedWithCode(0), ".*");
}

TEST(CpgProjectionStageTest, ByvalDestinationIsNotAnOrdinaryMemcpyAbi) {
  auto ir = CopyProgram(false);
  auto byval = ::llvm::Attribute::getWithByValType(
      ir.GetContext(), ::llvm::Type::getInt8Ty(ir.GetContext()));
  ir.GetFunction("memcpy")->addParamAttr(0, byval);
  for (auto &function : *ir.GetModule())
    for (auto &block : function) {
      for (auto &inst : block) {
        auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
        if (call && call->getCalledFunction()->getName() == "memcpy")
          call->addParamAttr(0, byval);
      }
    }
  ASSERT_FALSE(::llvm::verifyModule(*ir.GetModule(), &::llvm::errs()));
  auto graph = BuildThinCpg({ir,
                             {},
                             MakeId(core::IdKind::kRevision, "rev"),
                             MakeId(core::IdKind::kBuildVariant, "build")});
  ASSERT_TRUE(graph.ok());
  EXPECT_EQ(graph->callsites().size(), 4u);
  for (const auto &binding : graph->callsites())
    EXPECT_TRUE(binding.normalized_operation.empty());
}

TEST(CpgProjectionStageTest, DoesNotNormalizeMemcpyNameSubstringsOrForeignAbi) {
  for (bool rename : {false, true}) {
    auto ir = CopyProgram(false);
    if (rename)
      ir.GetFunction("memcpy")->setName("my_memcpy_wrapper");
    else {
      for (auto &function : *ir.GetModule()) {
        for (auto &block : function)
          for (auto &inst : block) {
            auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
            if (call && call->getCalledFunction()->getName() == "memcpy") {
              call->setCallingConv(::llvm::CallingConv::Fast);
            }
          }
      }
    }
    auto graph = BuildThinCpg({ir,
                               {},
                               MakeId(core::IdKind::kRevision, "rev"),
                               MakeId(core::IdKind::kBuildVariant, "build")});
    ASSERT_TRUE(graph.ok()) << graph.status().message();
    EXPECT_EQ(graph->callsites().size(), 4u);
    for (const auto &binding : graph->callsites())
      EXPECT_TRUE(binding.normalized_operation.empty());
  }
}

} // namespace
} // namespace veritas::analysis::cpg
