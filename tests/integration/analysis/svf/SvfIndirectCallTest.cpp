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

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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

// A program with a function-pointer call: `invoke-callback` loads a function
// pointer from a global initialized to `target` and calls it indirectly.
std::unique_ptr<pipeline::ProgramIr> BuildFunctionPointerProgramIr() {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();
  auto module = std::make_unique<::llvm::Module>("function_pointer",
                                                 program_ir->GetContext());
  auto* i32ty = ::llvm::Type::getInt32Ty(program_ir->GetContext());
  auto* fnty = ::llvm::FunctionType::get(i32ty, {i32ty}, false);
  auto* ptrty = ::llvm::PointerType::get(program_ir->GetContext(), 0);

  auto* target = ::llvm::Function::Create(
      fnty, ::llvm::Function::ExternalLinkage, "target", module.get());
  {
    auto* entry = ::llvm::BasicBlock::Create(program_ir->GetContext(), "entry",
                                             target);
    ::llvm::IRBuilder<> builder(entry);
    builder.CreateRet(target->getArg(0));
  }

  // Address-taken global function pointer so Andersen resolves the call.
  new ::llvm::GlobalVariable(*module, ptrty, /*isConstant=*/false,
                             ::llvm::GlobalValue::ExternalLinkage, target,
                             "fp");

  auto* caller = ::llvm::Function::Create(
      ::llvm::FunctionType::get(i32ty, {}, false), ::llvm::Function::ExternalLinkage,
      "invoke-callback", module.get());
  {
    auto* entry = ::llvm::BasicBlock::Create(program_ir->GetContext(), "entry",
                                             caller);
    ::llvm::IRBuilder<> builder(entry);
    auto* fp = module->getGlobalVariable("fp");
    auto* cb = builder.CreateLoad(ptrty, fp, "cb");
    auto* arg = ::llvm::ConstantInt::get(i32ty, 42);
    auto* result = builder.CreateCall(fnty, cb, {arg}, "call");
    builder.CreateRet(result);
  }

  program_ir->SetModule(std::move(module));
  return program_ir;
}

veritas::StatusOr<SvfMappingResult> AnalyzeFixture(const std::string&) {
  auto program_ir = BuildFunctionPointerProgramIr();

  AnalyzerRunContext run_context{
      .analyzer_run_id = "indirect_call_test",
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

std::vector<semantic::NormalizedCallTarget> CallsAt(
    const semantic::NormalizedAnalysisFacts& facts,
    const std::string& diagnostic_symbol) {
  std::vector<semantic::NormalizedCallTarget> out;
  for (const auto& call : facts.calls) {
    if (call.diagnostic_symbol == diagnostic_symbol) out.push_back(call);
  }
  return out;
}

TEST(SvfIndirectCallTest, EmitsStableMayTargetForFunctionPointer) {
  auto result = AnalyzeFixture("function_pointer");
  ASSERT_TRUE(result.ok());
  auto targets = CallsAt(result->facts, "invoke-callback");
  ASSERT_FALSE(targets.empty());
  EXPECT_TRUE(std::ranges::all_of(targets, [](const auto& target) {
    return target.callee.has_value() &&
           target.dispatch == semantic::DispatchKind::kIndirect &&
           target.epistemic == semantic::EpistemicState::kMay;
  }));
}

}  // namespace
}  // namespace veritas::analysis::svf
