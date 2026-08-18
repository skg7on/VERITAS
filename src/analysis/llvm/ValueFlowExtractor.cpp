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

#include "analysis/llvm/ValueFlowExtractor.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace veritas::analysis::llvm {

std::vector<ValueFlowExtractor::ValueFlow>
ValueFlowExtractor::ExtractValueFlows(const ::llvm::Function* func) const {
  std::vector<ValueFlow> flows;
  if (!func || func->isDeclaration()) {
    return flows;
  }

  for (const auto& bb : *func) {
    for (const auto& inst : bb) {
      // For each instruction with operands, record the data flow
      for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        auto* operand = inst.getOperand(i);
        if (::llvm::isa<::llvm::Instruction>(operand) ||
            ::llvm::isa<::llvm::Argument>(operand)) {
          flows.push_back({GetValueName(operand),
                           GetValueName(&inst),
                           inst.getOpcodeName()});
        }
      }

      // Special handling for PHI nodes (merge multiple flows)
      if (const auto* phi = ::llvm::dyn_cast<::llvm::PHINode>(&inst)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          auto* incoming = phi->getIncomingValue(i);
          flows.push_back({GetValueName(incoming),
                           GetValueName(phi),
                           "phi"});
        }
      }

      // Special handling for return instructions
      if (const auto* ret = ::llvm::dyn_cast<::llvm::ReturnInst>(&inst)) {
        if (auto* ret_val = ret->getReturnValue()) {
          flows.push_back({GetValueName(ret_val),
                           "<return>",
                           "ret"});
        }
      }
    }
  }

  return flows;
}

std::string ValueFlowExtractor::GetValueName(const ::llvm::Value* val) const {
  if (!val) {
    return "<null>";
  }

  if (val->hasName()) {
    return val->getName().str();
  }

  if (::llvm::isa<::llvm::Argument>(val)) {
    return "<arg" + std::to_string(
               ::llvm::cast<::llvm::Argument>(val)->getArgNo()) + ">";
  }

  if (::llvm::isa<::llvm::Constant>(val)) {
    return "<const>";
  }

  return "<unnamed>";
}

}  // namespace veritas::analysis::llvm
