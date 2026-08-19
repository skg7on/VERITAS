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

#include "analysis/pipeline/LocalAnalysisStage.h"

namespace veritas::analysis::pipeline {

LocalAnalysisStage::LocalAnalysisStage()
    : program_ir_(std::make_unique<ProgramIr>()),
      call_extractor_(std::make_unique<llvm::CallGraphExtractor>()),
      memory_extractor_(std::make_unique<llvm::MemoryAccessExtractor>()),
      flow_extractor_(std::make_unique<llvm::ValueFlowExtractor>()) {}

LocalAnalysisStage::~LocalAnalysisStage() = default;

std::vector<LocalAnalysisStage::LocalFacts>
LocalAnalysisStage::AnalyzeProject(
    const std::string& compile_commands_path) {
  std::vector<LocalFacts> all_facts;

  // TODO(M4): Implement full pipeline:
  // 1. Parse compile_commands.json
  // 2. Build LLVM IR using ProjectIrBuilder with ProgramIr's context
  // 3. Populate OriginMap with function symbol IDs
  // 4. Extract local facts for each function
  //
  // Current stub returns empty to allow compilation.
  (void)compile_commands_path;  // Suppress unused parameter warning

  return all_facts;
}

LocalAnalysisStage::LocalFacts LocalAnalysisStage::ExtractFacts(
    const core::FunctionSymbolId& func_id,
    const ::llvm::Function* llvm_func) {
  LocalFacts facts;
  facts.function_id = func_id;

  // Extract call graph edges
  facts.direct_calls = call_extractor_->ExtractCalls(llvm_func);

  // Extract memory access patterns
  facts.memory_accesses = memory_extractor_->ExtractMemoryAccesses(llvm_func);

  // Extract value flow relationships
  facts.value_flows = flow_extractor_->ExtractValueFlows(llvm_func);

  return facts;
}

}  // namespace veritas::analysis::pipeline
