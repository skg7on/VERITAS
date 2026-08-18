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

#ifndef VERITAS_ANALYSIS_LLVM_ORIGINMAP_H_
#define VERITAS_ANALYSIS_LLVM_ORIGINMAP_H_

#include <optional>
#include <string>
#include <unordered_map>

namespace llvm {
class Function;
}  // namespace llvm

namespace veritas::analysis::llvm {

// OriginMap tracks the bidirectional relationship between LLVM Function
// instances and their FunctionSymbolID. It survives LLVM IR transformations
// that preserve function identity (inlining destroys the mapping for the
// inlined callee, but not the caller).
class OriginMap {
 public:
  OriginMap() = default;
  ~OriginMap() = default;

  // Non-copyable, movable
  OriginMap(const OriginMap&) = delete;
  OriginMap& operator=(const OriginMap&) = delete;
  OriginMap(OriginMap&&) noexcept = default;
  OriginMap& operator=(OriginMap&&) noexcept = default;

  // Records the origin of an LLVM function. The function pointer must remain
  // valid for the lifetime of this OriginMap instance.
  void RecordOrigin(::llvm::Function* func, std::string symbol_id);

  // Looks up the symbol ID for a given LLVM function. Returns nullopt if the
  // function is not tracked.
  std::optional<std::string> GetSymbolId(const ::llvm::Function* func) const;

  // Looks up the LLVM function for a given symbol ID. Returns nullptr if the
  // symbol is not tracked.
  ::llvm::Function* GetFunction(const std::string& symbol_id) const;

  // Removes the mapping for a function (used when a function is deleted or
  // inlined away).
  void RemoveFunction(const ::llvm::Function* func);

  // Returns the number of tracked functions.
  size_t Size() const { return func_to_symbol_.size(); }

  // Clears all mappings.
  void Clear();

 private:
  std::unordered_map<const ::llvm::Function*, std::string> func_to_symbol_;
  std::unordered_map<std::string, ::llvm::Function*> symbol_to_func_;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_ORIGINMAP_H_
