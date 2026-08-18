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

#ifndef VERITAS_ANALYSIS_PIPELINE_PROGRAMIR_H_
#define VERITAS_ANALYSIS_PIPELINE_PROGRAMIR_H_

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
class Function;
}  // namespace llvm

namespace veritas::analysis::pipeline {

// ProgramIr owns the linked LLVM IR module and provides query methods for
// analysis stages. It manages the LLVMContext lifetime and ensures the module
// outlives all Function* references.
class ProgramIr {
 public:
  ProgramIr();
  ~ProgramIr();

  // Non-copyable, movable
  ProgramIr(const ProgramIr&) = delete;
  ProgramIr& operator=(const ProgramIr&) = delete;
  ProgramIr(ProgramIr&&) noexcept;
  ProgramIr& operator=(ProgramIr&&) noexcept;

  // Takes ownership of the module. The module must have been created in the
  // context owned by this ProgramIr instance.
  void SetModule(std::unique_ptr<::llvm::Module> module);

  // Returns the owned module. May be nullptr if SetModule has not been called.
  ::llvm::Module* GetModule() const { return module_.get(); }

  // Returns the LLVMContext owned by this instance.
  ::llvm::LLVMContext& GetContext() const { return *context_; }

  // Returns the function with the given name, or nullptr if not found.
  ::llvm::Function* GetFunction(const std::string& name) const;

  // Returns all functions in the module.
  std::vector<::llvm::Function*> GetAllFunctions() const;

  // Returns true if the module contains a function with the given name.
  bool HasFunction(const std::string& name) const;

  // Returns the number of functions in the module.
  size_t GetFunctionCount() const;

 private:
  std::unique_ptr<::llvm::LLVMContext> context_;
  std::unique_ptr<::llvm::Module> module_;
};

}  // namespace veritas::analysis::pipeline

#endif  // VERITAS_ANALYSIS_PIPELINE_PROGRAMIR_H_
