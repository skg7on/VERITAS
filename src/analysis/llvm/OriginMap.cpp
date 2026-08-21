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

void OriginMap::RecordOrigin(::llvm::Function *func, std::string symbol_id) {
  const std::string llvm_name = func->getName().str();
  func_to_symbol_[func] = symbol_id;
  symbol_to_func_[symbol_id] = func;

  if (ambiguous_names_.contains(llvm_name))
    return;
  auto [name_it, inserted] = name_to_symbol_.emplace(llvm_name, symbol_id);
  if (!inserted && name_it->second != symbol_id) {
    name_to_symbol_.erase(name_it);
    ambiguous_names_.insert(llvm_name);
  }
}

std::optional<std::string>
OriginMap::GetSymbolId(const ::llvm::Function *func) const {
  auto it = func_to_symbol_.find(func);
  if (it == func_to_symbol_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string>
OriginMap::GetSymbolIdByLlvmName(std::string_view llvm_name) const {
  auto it = name_to_symbol_.find(std::string(llvm_name));
  if (it == name_to_symbol_.end())
    return std::nullopt;
  return it->second;
}

::llvm::Function *OriginMap::GetFunction(const std::string &symbol_id) const {
  auto it = symbol_to_func_.find(symbol_id);
  if (it == symbol_to_func_.end()) {
    return nullptr;
  }
  return it->second;
}

void OriginMap::RemoveFunction(const ::llvm::Function *func) {
  auto it = func_to_symbol_.find(func);
  if (it != func_to_symbol_.end()) {
    const std::string llvm_name = func->getName().str();
    auto name_it = name_to_symbol_.find(llvm_name);
    if (name_it != name_to_symbol_.end() && name_it->second == it->second) {
      name_to_symbol_.erase(name_it);
    }
    symbol_to_func_.erase(it->second);
    func_to_symbol_.erase(it);
  }
}

void OriginMap::Clear() {
  func_to_symbol_.clear();
  symbol_to_func_.clear();
  name_to_symbol_.clear();
  ambiguous_names_.clear();
}

} // namespace veritas::analysis::llvm
