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

#include "veritas/analysis/ProjectAnalyzer.h"

#include <gtest/gtest.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfSession.h"

namespace veritas::analysis {
namespace {

// Helper to run a minimal analysis twice
svf::SvfMappingResult RunMinimalAnalysis() {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  auto module = std::make_unique<::llvm::Module>(
      "repeated_test", program_ir->GetContext());

  auto* func_type = ::llvm::FunctionType::get(
      ::llvm::Type::getInt32Ty(program_ir->GetContext()),
      {::llvm::Type::getInt32Ty(program_ir->GetContext())},
      false);
  auto* func = ::llvm::Function::Create(
      func_type,
      ::llvm::Function::ExternalLinkage,
      "test_func",
      module.get());

  auto* entry = ::llvm::BasicBlock::Create(
      program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);
  builder.CreateRet(func->getArg(0));

  program_ir->SetModule(std::move(module));

  svf::AnalyzerRunContext run_context{
      .analyzer_run_id = "determinism_test",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  svf::SvfMappingResult result;
  auto status = svf::RunWithSvfSession(
      *program_ir, svf::SvfConfig::Default(),
      [&](const svf::SvfSessionView& view) {
        return svf::MapSvfFacts(
            *program_ir, view, run_context, svf::SvfConfig::Default(), &result);
      });

  if (!status.ok()) {
    ADD_FAILURE() << "Analysis failed";
  }

  return result;
}

TEST(RepeatedProjectAnalysisTest, TwoRunsProduceDeterministicResults) {
  auto result1 = RunMinimalAnalysis();
  auto result2 = RunMinimalAnalysis();

  // Completion status should match
  EXPECT_EQ(result1.completion, result2.completion);

  // Fact counts should match (deterministic analysis)
  EXPECT_EQ(result1.facts.value_flows.size(),
            result2.facts.value_flows.size());
  EXPECT_EQ(result1.facts.aliases.size(),
            result2.facts.aliases.size());
  EXPECT_EQ(result1.facts.unknowns.size(),
            result2.facts.unknowns.size());

  // Facts should be identical after sorting (already sorted by mapper)
  EXPECT_EQ(result1.facts.value_flows, result2.facts.value_flows);
  EXPECT_EQ(result1.facts.aliases, result2.facts.aliases);
  EXPECT_EQ(result1.facts.unknowns, result2.facts.unknowns);
}

TEST(RepeatedProjectAnalysisTest, SvfStateCleanBetweenRuns) {
  // Run analysis twice to verify SVF singleton cleanup works
  for (int i = 0; i < 3; ++i) {
    auto result = RunMinimalAnalysis();

    // Should succeed on every iteration
    EXPECT_TRUE(result.completion == svf::SvfMappingCompletion::kComplete ||
                result.completion == svf::SvfMappingCompletion::kCompleteWithUnknowns);
  }

  // If SVF state wasn't cleaned up properly, later runs would fail
}

}  // namespace
}  // namespace veritas::analysis
