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

#include "analysis/llvm/CallGraphExtractor.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace veritas::analysis::llvm {

std::vector<std::string> CallGraphExtractor::ExtractCalls(
    const ::llvm::Function* func) const {
  std::vector<std::string> callees;
  if (!func || func->isDeclaration()) {
    return callees;
  }

  for (const auto& bb : *func) {
    for (const auto& inst : bb) {
      if (const auto* call_inst = ::llvm::dyn_cast<::llvm::CallInst>(&inst)) {
        if (auto* callee = call_inst->getCalledFunction()) {
          callees.push_back(GetFunctionSymbolId(callee));
        }
      } else if (const auto* invoke_inst =
                     ::llvm::dyn_cast<::llvm::InvokeInst>(&inst)) {
        if (auto* callee = invoke_inst->getCalledFunction()) {
          callees.push_back(GetFunctionSymbolId(callee));
        }
      }
    }
  }

  return callees;
}

std::vector<CallGraphExtractor::CallEdge>
CallGraphExtractor::ExtractCallGraph(const ::llvm::Module* module) const {
  std::vector<CallEdge> edges;
  if (!module) {
    return edges;
  }

  for (const auto& func : *module) {
    if (func.isDeclaration()) {
      continue;
    }

    std::string caller_id = GetFunctionSymbolId(&func);
    auto callees = ExtractCalls(&func);

    for (const auto& callee_id : callees) {
      edges.push_back({caller_id, callee_id});
    }
  }

  return edges;
}

std::string CallGraphExtractor::GetFunctionSymbolId(
    const ::llvm::Function* func) const {
  if (!func) {
    return "";
  }

  std::string symbol_id = func->getName().str();
  if (func->hasInternalLinkage() && func->getParent()) {
    // Qualify static functions with module identifier
    symbol_id = func->getParent()->getModuleIdentifier() + "::" + symbol_id;
  }

  return symbol_id;
}

}  // namespace veritas::analysis::llvm
