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

#ifndef VERITAS_ANALYSIS_LLVM_VALUEFLOWEXTRACTOR_H_
#define VERITAS_ANALYSIS_LLVM_VALUEFLOWEXTRACTOR_H_

#include <string>
#include <vector>

namespace llvm {
class Function;
class Value;
}  // namespace llvm

namespace veritas::analysis::llvm {

// ValueFlowExtractor tracks data flow relationships within a function. It
// identifies def-use chains and value propagation patterns.
class ValueFlowExtractor {
 public:
  struct ValueFlow {
    std::string source;       // source value name or description
    std::string destination;  // destination value name or description
    std::string operation;    // operation that connects them
  };

  ValueFlowExtractor() = default;
  ~ValueFlowExtractor() = default;

  // Extracts data flow edges within a function. Returns a list of (source,
  // destination, operation) triples representing value propagation.
  std::vector<ValueFlow> ExtractValueFlows(const ::llvm::Function* func) const;

 private:
  std::string GetValueName(const ::llvm::Value* val) const;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_VALUEFLOWEXTRACTOR_H_
