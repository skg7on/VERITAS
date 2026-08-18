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

#include "analysis/llvm/MemoryAccessExtractor.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace veritas::analysis::llvm {

std::vector<MemoryAccessExtractor::MemoryAccess>
MemoryAccessExtractor::ExtractMemoryAccesses(
    const ::llvm::Function* func) const {
  std::vector<MemoryAccess> accesses;
  if (!func || func->isDeclaration()) {
    return accesses;
  }

  for (const auto& bb : *func) {
    for (const auto& inst : bb) {
      if (const auto* load_inst = ::llvm::dyn_cast<::llvm::LoadInst>(&inst)) {
        accesses.push_back({AccessKind::kRead,
                            GetLocationName(load_inst->getPointerOperand()),
                            &inst});
      } else if (const auto* store_inst =
                     ::llvm::dyn_cast<::llvm::StoreInst>(&inst)) {
        accesses.push_back({AccessKind::kWrite,
                            GetLocationName(store_inst->getPointerOperand()),
                            &inst});
      } else if (const auto* rmw_inst =
                     ::llvm::dyn_cast<::llvm::AtomicRMWInst>(&inst)) {
        accesses.push_back({AccessKind::kReadWrite,
                            GetLocationName(rmw_inst->getPointerOperand()),
                            &inst});
      } else if (const auto* cmpxchg_inst =
                     ::llvm::dyn_cast<::llvm::AtomicCmpXchgInst>(&inst)) {
        accesses.push_back({AccessKind::kReadWrite,
                            GetLocationName(cmpxchg_inst->getPointerOperand()),
                            &inst});
      } else if (::llvm::isa<::llvm::CallInst>(&inst) ||
                 ::llvm::isa<::llvm::InvokeInst>(&inst)) {
        // Conservative: assume all calls may read and write memory
        accesses.push_back({AccessKind::kReadWrite, "<call-effect>", &inst});
      }
    }
  }

  return accesses;
}

std::string MemoryAccessExtractor::GetLocationName(
    const ::llvm::Value* ptr) const {
  if (!ptr) {
    return "<unknown>";
  }

  if (ptr->hasName()) {
    return ptr->getName().str();
  }

  // For unnamed values, provide a description based on the value type
  if (::llvm::isa<::llvm::GlobalVariable>(ptr)) {
    return "<global>";
  } else if (::llvm::isa<::llvm::AllocaInst>(ptr)) {
    return "<local>";
  } else if (::llvm::isa<::llvm::Argument>(ptr)) {
    return "<param>";
  }

  return "<unnamed>";
}

}  // namespace veritas::analysis::llvm
