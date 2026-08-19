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

#ifndef VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_
#define VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_

#include <memory>
#include <string>
#include <vector>

#include "analysis/llvm/CallGraphExtractor.h"
#include "analysis/llvm/MemoryAccessExtractor.h"
#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/ValueFlowExtractor.h"
#include "analysis/pipeline/ProgramIr.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::pipeline {

// LocalAnalysisStage orchestrates the M4 local analysis pipeline. It combines
// AST extraction, IR generation, and local fact extraction to produce function
// summaries without interprocedural analysis.
//
// Pipeline stages:
// 1. Clang AST extraction (via ProjectAstExtractor)
// 2. LLVM IR generation and linking (via ProjectIrBuilder)
// 3. Local fact extraction (CallGraph, Memory, ValueFlow)
// 4. Summary draft construction (placeholder for M5)
class LocalAnalysisStage {
 public:
  struct LocalFacts {
    core::FunctionSymbolId function_id;
    std::vector<std::string> direct_calls;
    std::vector<llvm::MemoryAccessExtractor::MemoryAccess> memory_accesses;
    std::vector<llvm::ValueFlowExtractor::ValueFlow> value_flows;
  };

  LocalAnalysisStage();
  ~LocalAnalysisStage();

  // Runs local analysis over a compilation database. Returns extracted facts
  // for each function analyzed.
  std::vector<LocalFacts> AnalyzeProject(
      const std::string& compile_commands_path);

 private:
  std::unique_ptr<ProgramIr> program_ir_;
  std::unique_ptr<llvm::CallGraphExtractor> call_extractor_;
  std::unique_ptr<llvm::MemoryAccessExtractor> memory_extractor_;
  std::unique_ptr<llvm::ValueFlowExtractor> flow_extractor_;

  LocalFacts ExtractFacts(const core::FunctionSymbolId& func_id,
                          const ::llvm::Function* llvm_func);
};

}  // namespace veritas::analysis::pipeline

#endif  // VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_
