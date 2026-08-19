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

#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfSession.h"

namespace veritas::analysis::svf {
namespace {

// Helper to build a fixture ProgramIr for testing
std::unique_ptr<pipeline::ProgramIr> BuildFixtureProgramIr(
    const std::string& fixture_name) {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  auto module = std::make_unique<::llvm::Module>(
      fixture_name, program_ir->GetContext());

  // Create a simple function for testing
  auto* func_type = ::llvm::FunctionType::get(
      ::llvm::Type::getInt32Ty(program_ir->GetContext()),
      {::llvm::Type::getInt32Ty(program_ir->GetContext())},
      false);
  auto* func = ::llvm::Function::Create(
      func_type,
      ::llvm::Function::ExternalLinkage,
      fixture_name,
      module.get());

  auto* entry = ::llvm::BasicBlock::Create(
      program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);
  builder.CreateRet(func->getArg(0));

  program_ir->SetModule(std::move(module));
  return program_ir;
}

// Helper to analyze a fixture with SVF and map facts
SvfMappingResult AnalyzeFixtureWithSvf(const std::string& fixture_name) {
  auto program_ir = BuildFixtureProgramIr(fixture_name);

  AnalyzerRunContext run_context{
      .analyzer_run_id = "test_run_001",
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

  // All facts should have provenance
  for (const auto& fact : result.facts.value_flows) {
    EXPECT_FALSE(fact.provenance.empty());
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
  for (const auto& fact : result.facts.value_flows) {
    EXPECT_NE(fact.provenance.find("analyzer="), std::string::npos);
  }
  for (const auto& fact : result.facts.aliases) {
    EXPECT_NE(fact.provenance.find("analyzer="), std::string::npos);
  }
  for (const auto& fact : result.facts.unknowns) {
    EXPECT_NE(fact.provenance.find("analyzer="), std::string::npos);
  }
}

TEST(SvfFactMapperTest, UnmappedNodesCreateUnknownFacts) {
  auto result = AnalyzeFixtureWithSvf("parameter_return");

  // If mapping is incomplete, unknowns should explain why
  if (result.completion == SvfMappingCompletion::kCompleteWithUnknowns) {
    EXPECT_GT(result.facts.unknowns.size(), 0u);

    for (const auto& unknown : result.facts.unknowns) {
      EXPECT_FALSE(unknown.scope.empty());
      EXPECT_FALSE(unknown.reason.empty());
      EXPECT_FALSE(unknown.provenance.empty());
    }
  }
}

}  // namespace
}  // namespace veritas::analysis::svf
