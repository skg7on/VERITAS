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

#include "analysis/llvm/ProjectIrBuilder.h"

#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>

#include "analysis/llvm/OriginMap.h"

namespace veritas::analysis::llvm {

ProjectIrBuilder::ProjectIrBuilder(::llvm::LLVMContext& context)
    : context_(context) {}

ProjectIrBuilder::~ProjectIrBuilder() = default;

bool ProjectIrBuilder::AddTranslationUnit(
    const std::string& source_file,
    const std::vector<std::string>& compiler_args) {
  // Create a CodeGenAction to emit LLVM IR
  auto action = std::make_unique<::clang::EmitLLVMOnlyAction>(&context_);

  // Run the action using Clang tooling
  if (!::clang::tooling::runToolOnCodeWithArgs(std::move(action),
                                                source_file,
                                                compiler_args)) {
    last_error_ = "Failed to generate LLVM IR for translation unit";
    return false;
  }

  // The module is now owned by the action and will be retrieved during linking
  return true;
}

std::unique_ptr<::llvm::Module> ProjectIrBuilder::LinkAndBuild(
    OriginMap& origin_map) {
  if (modules_.empty()) {
    last_error_ = "No translation units to link";
    return nullptr;
  }

  // Start with the first module as the base
  auto linked_module = std::move(modules_[0]);
  ::llvm::Linker linker(*linked_module);

  // Link in all remaining modules
  for (size_t i = 1; i < modules_.size(); ++i) {
    if (linker.linkInModule(std::move(modules_[i]))) {
      last_error_ = "Failed to link module " + std::to_string(i);
      return nullptr;
    }
  }

  // Populate the origin map by iterating over all functions
  for (auto& func : *linked_module) {
    if (!func.isDeclaration()) {
      // Generate the symbol ID from the function name and linkage
      std::string symbol_id = func.getName().str();
      if (func.hasInternalLinkage()) {
        // For static functions, qualify with module identifier
        symbol_id = linked_module->getModuleIdentifier() + "::" + symbol_id;
      }
      origin_map.RecordOrigin(&func, symbol_id);
    }
  }

  modules_.clear();
  return linked_module;
}

}  // namespace veritas::analysis::llvm
