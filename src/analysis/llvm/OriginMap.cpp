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

#include "analysis/llvm/OriginMap.h"

#include <llvm/IR/Function.h>

namespace veritas::analysis::llvm {

void OriginMap::RecordOrigin(::llvm::Function* func, std::string symbol_id) {
  func_to_symbol_[func] = symbol_id;
  symbol_to_func_[symbol_id] = func;
}

std::optional<std::string> OriginMap::GetSymbolId(
    const ::llvm::Function* func) const {
  auto it = func_to_symbol_.find(func);
  if (it == func_to_symbol_.end()) {
    return std::nullopt;
  }
  return it->second;
}

::llvm::Function* OriginMap::GetFunction(const std::string& symbol_id) const {
  auto it = symbol_to_func_.find(symbol_id);
  if (it == symbol_to_func_.end()) {
    return nullptr;
  }
  return it->second;
}

void OriginMap::RemoveFunction(const ::llvm::Function* func) {
  auto it = func_to_symbol_.find(func);
  if (it != func_to_symbol_.end()) {
    symbol_to_func_.erase(it->second);
    func_to_symbol_.erase(it);
  }
}

void OriginMap::Clear() {
  func_to_symbol_.clear();
  symbol_to_func_.clear();
}

}  // namespace veritas::analysis::llvm
