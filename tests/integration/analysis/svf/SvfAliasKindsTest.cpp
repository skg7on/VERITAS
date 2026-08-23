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

#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfSession.h"
#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"
#include "veritas/core/Status.h"

namespace veritas::analysis::svf {
namespace {

namespace semantic = veritas::analysis::semantic;

// A program with two disjoint stack allocations named "left" and "right",
// each stored and loaded, so SVF sees two distinct memory locations that do
// not alias.
std::unique_ptr<pipeline::ProgramIr> BuildDisjointAllocasProgramIr() {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();
  auto module =
      std::make_unique<::llvm::Module>("disjoint", program_ir->GetContext());
  auto* i32ty = ::llvm::Type::getInt32Ty(program_ir->GetContext());

  auto* func = ::llvm::Function::Create(
      ::llvm::FunctionType::get(i32ty, {}, false),
      ::llvm::Function::ExternalLinkage, "disjoint", module.get());
  auto* entry =
      ::llvm::BasicBlock::Create(program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);

  auto* left = builder.CreateAlloca(i32ty, nullptr, "left");
  auto* right = builder.CreateAlloca(i32ty, nullptr, "right");
  builder.CreateStore(::llvm::ConstantInt::get(i32ty, 1), left);
  builder.CreateStore(::llvm::ConstantInt::get(i32ty, 2), right);
  auto* a = builder.CreateLoad(i32ty, left, "a");
  auto* b = builder.CreateLoad(i32ty, right, "b");
  auto* sum = builder.CreateAdd(a, b, "sum");
  builder.CreateRet(sum);

  program_ir->SetModule(std::move(module));
  return program_ir;
}

veritas::StatusOr<SvfMappingResult> AnalyzeDisjointAllocas() {
  auto program_ir = BuildDisjointAllocasProgramIr();

  AnalyzerRunContext run_context{
      .analyzer_run_id = "alias_kinds_test",
      .llvm_toolchain_identity = "llvm-22.0.0",
      .program_module_hash = "test_hash",
  };

  SvfMappingResult result;
  auto status = RunWithSvfSession(
      *program_ir, SvfConfig::Default(), [&](const SvfSessionView& view) {
        return MapSvfFacts(*program_ir, view, run_context, SvfConfig::Default(),
                           &result);
      });
  if (!status.ok()) return status;
  return result;
}

std::optional<semantic::NormalizedAlias> FindAlias(
    const semantic::NormalizedAnalysisFacts& facts,
    const std::string& left_name, const std::string& right_name) {
  for (const auto& alias : facts.aliases) {
    const std::string& a = alias.left.object.diagnostic_name;
    const std::string& b = alias.right.object.diagnostic_name;
    if ((a == left_name && b == right_name) ||
        (a == right_name && b == left_name)) {
      return alias;
    }
  }
  return std::nullopt;
}

TEST(SvfAliasKindsTest, KeepsNoAliasAsSemanticNoAliasMust) {
  auto result = AnalyzeDisjointAllocas();
  ASSERT_TRUE(result.ok());
  auto observation = FindAlias(result->facts, "left", "right");
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->kind, semantic::AliasKind::kNoAlias);
  EXPECT_EQ(observation->epistemic, semantic::EpistemicState::kMust);
}

}  // namespace
}  // namespace veritas::analysis::svf
