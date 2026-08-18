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

#ifndef VERITAS_ANALYSIS_LLVM_CALLGRAPHEXTRACTOR_H_
#define VERITAS_ANALYSIS_LLVM_CALLGRAPHEXTRACTOR_H_

#include <string>
#include <vector>

namespace llvm {
class Function;
class Module;
}  // namespace llvm

namespace veritas::analysis::llvm {

// CallGraphExtractor extracts direct function calls from LLVM IR. It produces
// a list of (caller, callee) pairs where both are FunctionSymbolIDs.
class CallGraphExtractor {
 public:
  struct CallEdge {
    std::string caller;
    std::string callee;
  };

  CallGraphExtractor() = default;
  ~CallGraphExtractor() = default;

  // Extracts all direct calls from a function. Returns a list of callee
  // symbol IDs. Indirect calls through function pointers are not included.
  std::vector<std::string> ExtractCalls(const ::llvm::Function* func) const;

  // Extracts the full call graph from a module. Returns all (caller, callee)
  // edges.
  std::vector<CallEdge> ExtractCallGraph(const ::llvm::Module* module) const;

 private:
  std::string GetFunctionSymbolId(const ::llvm::Function* func) const;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_CALLGRAPHEXTRACTOR_H_
