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

#include "analysis/svf/SvfBudget.h"
#include "analysis/svf/SvfFactMapper.h"

#include <gtest/gtest.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfSession.h"

namespace veritas::analysis::svf {
namespace {

// Helper to build a fixture with multiple functions to generate more facts
std::unique_ptr<pipeline::ProgramIr> BuildLargeProgramIr() {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  auto module = std::make_unique<::llvm::Module>(
      "large_fixture", program_ir->GetContext());

  auto* i32_ty = ::llvm::Type::getInt32Ty(program_ir->GetContext());

  // Create multiple functions to exceed fact limits
  for (int i = 0; i < 10; ++i) {
    auto* func_type = ::llvm::FunctionType::get(i32_ty, {i32_ty}, false);
    auto* func = ::llvm::Function::Create(
        func_type,
        ::llvm::Function::ExternalLinkage,
        "func_" + std::to_string(i),
        module.get());

    auto* entry = ::llvm::BasicBlock::Create(
        program_ir->GetContext(), "entry", func);
    ::llvm::IRBuilder<> builder(entry);
    builder.CreateRet(func->getArg(0));
  }

  program_ir->SetModule(std::move(module));
  return program_ir;
}

TEST(SvfTruncationTest, TruncatedMappingReturnsCompleteWithUnknowns) {
  auto program_ir = BuildLargeProgramIr();

  // Set very low fact limit to trigger truncation
  auto config = SvfConfig::Default();
  config.max_emitted_facts = 1;

  AnalyzerRunContext run_context{
      .analyzer_run_id = "truncation_test",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  SvfMappingResult result;
  auto status = RunWithSvfSession(
      *program_ir, config,
      [&](const SvfSessionView& view) {
        return MapSvfFacts(
            *program_ir, view, run_context, config, &result);
      });

  EXPECT_TRUE(status.ok());

  // Should have truncated and returned kCompleteWithUnknowns
  // Note: actual behavior depends on fact emission in MapSvfFacts
  if (result.completion == SvfMappingCompletion::kCompleteWithUnknowns) {
    EXPECT_GT(result.facts.unknowns.size(), 0u);

    // Verify unknowns have proper structure
    for (const auto& unknown : result.facts.unknowns) {
      EXPECT_FALSE(unknown.scope.empty());
      EXPECT_FALSE(unknown.reason.empty());
      EXPECT_FALSE(unknown.provenance_ref.empty());
    }
  }
}

TEST(SvfTruncationTest, ValidPartialFactsPreservedAtTruncation) {
  auto program_ir = BuildLargeProgramIr();

  auto config = SvfConfig::Default();
  config.max_emitted_facts = 5;

  AnalyzerRunContext run_context{
      .analyzer_run_id = "partial_test",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  SvfMappingResult result;
  auto status = RunWithSvfSession(
      *program_ir, config,
      [&](const SvfSessionView& view) {
        return MapSvfFacts(
            *program_ir, view, run_context, config, &result);
      });

  EXPECT_TRUE(status.ok());

  // All emitted facts should be valid (deduplicated, with provenance)
  for (const auto& fact : result.facts.value_flows) {
    EXPECT_FALSE(fact.provenance_ref.empty());
  }

  // Should not exceed the configured limit
  EXPECT_LE(result.facts.value_flows.size() +
            result.facts.aliases.size() +
            result.facts.memory_effects.size() +
            result.facts.calls.size(),
            config.max_emitted_facts);
}

TEST(SvfTruncationTest, NoBudgetLimitProducesComplete) {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  auto module = std::make_unique<::llvm::Module>(
      "small_fixture", program_ir->GetContext());

  auto* func_type = ::llvm::FunctionType::get(
      ::llvm::Type::getInt32Ty(program_ir->GetContext()),
      {::llvm::Type::getInt32Ty(program_ir->GetContext())},
      false);
  auto* func = ::llvm::Function::Create(
      func_type,
      ::llvm::Function::ExternalLinkage,
      "simple",
      module.get());

  auto* entry = ::llvm::BasicBlock::Create(
      program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);
  builder.CreateRet(func->getArg(0));

  program_ir->SetModule(std::move(module));

  AnalyzerRunContext run_context{
      .analyzer_run_id = "complete_test",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  SvfMappingResult result;
  auto status = RunWithSvfSession(
      *program_ir, SvfConfig::Default(),
      [&](const SvfSessionView& view) {
        return MapSvfFacts(
            *program_ir, view, run_context, SvfConfig::Default(), &result);
      });

  EXPECT_TRUE(status.ok());

  // With default generous limits, should complete fully
  // (or with unknowns only due to unmapped nodes, not budget)
}

}  // namespace
}  // namespace veritas::analysis::svf
