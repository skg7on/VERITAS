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

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/StableValueMapper.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::llvm {
namespace {

// Owns the LLVMContext alongside the parsed module so the module always has a
// live context. Exposes Module-like dereference for the brief's test idiom.
struct ParsedModule {
  std::unique_ptr<::llvm::LLVMContext> context;
  std::unique_ptr<::llvm::Module> module;

  ::llvm::Module& operator*() { return *module; }
  ::llvm::Module* operator->() { return module.get(); }
};

ParsedModule ParseIr(const char* ir) {
  auto context = std::make_unique<::llvm::LLVMContext>();
  ::llvm::SMDiagnostic error;
  auto module = ::llvm::parseAssemblyString(ir, error, *context);
  if (!module) {
    error.print("StableValueMapperTest", ::llvm::errs());
    std::abort();
  }
  ParsedModule parsed;
  parsed.context = std::move(context);
  parsed.module = std::move(module);
  return parsed;
}

OriginMap OriginMapFor(const ::llvm::Module& module) {
  OriginMap origin_map;
  for (const auto& function : module) {
    if (function.isDeclaration())
      continue;
    const std::string name = function.getName().str();
    const auto id = core::MakeStableId(
        core::IdKind::kFunctionVariant,
        std::as_bytes(std::span(name.data(), name.size())));
    origin_map.RecordOrigin(const_cast<::llvm::Function*>(&function),
                            core::ToString(id));
  }
  return origin_map;
}

std::vector<const ::llvm::Value*> InstructionsNamedByOrdinal(
    const ::llvm::Function& function) {
  std::vector<const ::llvm::Value*> values;
  for (const auto& block : function) {
    for (const auto& inst : block) {
      values.push_back(&inst);
    }
  }
  return values;
}

TEST(StableValueMapperTest, DistinguishesUnnamedInstructions) {
  auto module = ParseIr(R"(
    define void @f() {
      %1 = alloca i32
      %2 = alloca i32
      ret void
    })");
  StableValueMapper mapper(*module, OriginMapFor(*module));
  auto values = InstructionsNamedByOrdinal(*module->getFunction("f"));
  EXPECT_NE(*mapper.IdFor(*values[0]), *mapper.IdFor(*values[1]));
}

}  // namespace
}  // namespace veritas::analysis::llvm
