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

#ifndef VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_
#define VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace veritas::analysis::llvm {

class OriginMap;

// ProjectIrBuilder generates LLVM IR for each translation unit and links them
// into a single module. It populates the OriginMap with FunctionSymbolID ->
// Function* mappings.
class ProjectIrBuilder {
 public:
  explicit ProjectIrBuilder(::llvm::LLVMContext& context);
  ~ProjectIrBuilder();

  // Non-copyable, non-movable (holds references)
  ProjectIrBuilder(const ProjectIrBuilder&) = delete;
  ProjectIrBuilder& operator=(const ProjectIrBuilder&) = delete;
  ProjectIrBuilder(ProjectIrBuilder&&) = delete;
  ProjectIrBuilder& operator=(ProjectIrBuilder&&) = delete;

  // Generates LLVM IR for a single translation unit and adds it to the
  // accumulated modules. Returns false on error.
  bool AddTranslationUnit(const std::string& source_file,
                          const std::vector<std::string>& compiler_args);

  // Links all accumulated translation units into a single module and populates
  // the origin map. Returns nullptr on error. The caller takes ownership of
  // the returned module.
  std::unique_ptr<::llvm::Module> LinkAndBuild(OriginMap& origin_map);

  // Returns the last error message, if any.
  const std::string& GetLastError() const { return last_error_; }

 private:
  ::llvm::LLVMContext& context_;
  std::vector<std::unique_ptr<::llvm::Module>> modules_;
  std::string last_error_;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_
