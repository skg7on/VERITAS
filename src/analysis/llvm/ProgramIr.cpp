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

#include "analysis/pipeline/ProgramIr.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace veritas::analysis::pipeline {

ProgramIr::ProgramIr() : context_(std::make_unique<::llvm::LLVMContext>()) {}

ProgramIr::~ProgramIr() = default;

ProgramIr::ProgramIr(ProgramIr&&) noexcept = default;

ProgramIr& ProgramIr::operator=(ProgramIr&&) noexcept = default;

void ProgramIr::SetModule(std::unique_ptr<::llvm::Module> module) {
  module_ = std::move(module);
}

::llvm::Function* ProgramIr::GetFunction(const std::string& name) const {
  if (!module_) {
    return nullptr;
  }
  return module_->getFunction(name);
}

std::vector<::llvm::Function*> ProgramIr::GetAllFunctions() const {
  std::vector<::llvm::Function*> functions;
  if (!module_) {
    return functions;
  }
  for (auto& func : *module_) {
    functions.push_back(&func);
  }
  return functions;
}

bool ProgramIr::HasFunction(const std::string& name) const {
  return GetFunction(name) != nullptr;
}

size_t ProgramIr::GetFunctionCount() const {
  if (!module_) {
    return 0;
  }
  return module_->size();
}

}  // namespace veritas::analysis::pipeline
