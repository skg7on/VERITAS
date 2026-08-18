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

#define VERITAS_SVF_SESSION_TEST_HOOKS
#include "analysis/svf/SvfSession.h"

#include <gtest/gtest.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>

#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {
namespace {

// Helper to build a minimal fixture ProgramIr
std::unique_ptr<pipeline::ProgramIr> BuildFixtureProgramIr(
    const std::string& fixture_name) {
  auto program_ir = std::make_unique<pipeline::ProgramIr>();

  // Create a simple module with a function
  auto module = std::make_unique<::llvm::Module>(
      fixture_name, program_ir->GetContext());

  // Create a simple function: int parameter_return(int arg0) { return arg0; }
  auto* func_type = ::llvm::FunctionType::get(
      ::llvm::Type::getInt32Ty(program_ir->GetContext()),
      {::llvm::Type::getInt32Ty(program_ir->GetContext())},
      false);
  auto* func = ::llvm::Function::Create(
      func_type,
      ::llvm::Function::ExternalLinkage,
      "parameter_return",
      module.get());

  auto* entry = ::llvm::BasicBlock::Create(
      program_ir->GetContext(), "entry", func);
  ::llvm::IRBuilder<> builder(entry);
  builder.CreateRet(func->getArg(0));

  program_ir->SetModule(std::move(module));
  return program_ir;
}

TEST(SvfSessionTest, BuildsDirectlyFromLiveLlvmModule) {
  auto program_ir = BuildFixtureProgramIr("parameter_return");
  int callback_count = 0;

  auto status = RunWithSvfSession(
      *program_ir, SvfConfig::Default(),
      [&](const SvfSessionView& view) {
        ++callback_count;
        EXPECT_NE(view.svf_ir, nullptr);
        EXPECT_NE(view.andersen, nullptr);
        EXPECT_NE(view.svfg, nullptr);
        return Status::Ok();
      });

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(callback_count, 1);
}

TEST(SvfSessionTest, ReleasesSingletonStateBetweenRuns) {
  for (int run = 0; run < 2; ++run) {
    auto program_ir = BuildFixtureProgramIr("parameter_return");

    auto status = RunWithSvfSession(
        *program_ir, SvfConfig::Default(),
        [](const SvfSessionView& view) {
          return Status::Ok();
        });

    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(SvfGlobalStateIsCleanForTest());
  }
}

TEST(SvfSessionTest, CallbackErrorPreservesCleanup) {
  auto program_ir = BuildFixtureProgramIr("parameter_return");

  auto status = RunWithSvfSession(
      *program_ir, SvfConfig::Default(),
      [](const SvfSessionView& view) {
        return Status::Internal("deliberate callback failure");
      });

  EXPECT_FALSE(status.ok());

  // Verify cleanup still happened by running another session
  auto program_ir2 = BuildFixtureProgramIr("parameter_return");
  auto status2 = RunWithSvfSession(
      *program_ir2, SvfConfig::Default(),
      [](const SvfSessionView& view) {
        return Status::Ok();
      });

  EXPECT_TRUE(status2.ok());
}

}  // namespace
}  // namespace veritas::analysis::svf
