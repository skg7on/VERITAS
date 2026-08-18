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

#ifndef VERITAS_ANALYSIS_LLVM_MEMORYACCESSEXTRACTOR_H_
#define VERITAS_ANALYSIS_LLVM_MEMORYACCESSEXTRACTOR_H_

#include <string>
#include <vector>

namespace llvm {
class Function;
class Instruction;
class Value;
}  // namespace llvm

namespace veritas::analysis::llvm {

// MemoryAccessExtractor identifies memory read and write operations in LLVM
// IR. It produces a conservative over-approximation of memory effects.
class MemoryAccessExtractor {
 public:
  enum class AccessKind {
    kRead,
    kWrite,
    kReadWrite,
  };

  struct MemoryAccess {
    AccessKind kind;
    std::string location;  // symbolic location (variable name or description)
    const ::llvm::Instruction* instruction;
  };

  MemoryAccessExtractor() = default;
  ~MemoryAccessExtractor() = default;

  // Extracts all memory accesses from a function. Returns loads, stores, and
  // calls that may access memory.
  std::vector<MemoryAccess> ExtractMemoryAccesses(
      const ::llvm::Function* func) const;

 private:
  std::string GetLocationName(const ::llvm::Value* ptr) const;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_MEMORYACCESSEXTRACTOR_H_
