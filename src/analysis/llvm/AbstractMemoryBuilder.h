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

#ifndef VERITAS_ANALYSIS_LLVM_ABSTRACTMEMORYBUILDER_H_
#define VERITAS_ANALYSIS_LLVM_ABSTRACTMEMORYBUILDER_H_

#include <cstdint>
#include <optional>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Status.h"

namespace llvm {
class DataLayout;
class Value;
}  // namespace llvm

namespace veritas::analysis::llvm {

class OriginMap;
class StableValueMapper;

// AbstractMemoryBuilder derives a semantic::MemoryLocation (an abstract
// object, a GEP access path, and a byte range) from an LLVM pointer value and
// an optional access size. The base object is found by stripping GEPs and
// no-op pointer casts; the access path is reconstructed from GEP source
// element types with the DataLayout, appending kUnknown (and an unknown byte
// range) whenever an index is not a compile-time constant. LLVM names never
// participate in object or memory-location identity.
class AbstractMemoryBuilder {
 public:
  AbstractMemoryBuilder(const ::llvm::DataLayout& layout,
                        const StableValueMapper& values,
                        const OriginMap& origins);
  ~AbstractMemoryBuilder() = default;

  StatusOr<semantic::MemoryLocation> LocationFor(
      const ::llvm::Value& pointer,
      std::optional<std::uint64_t> access_size) const;

 private:
  const ::llvm::DataLayout& layout_;
  const StableValueMapper& values_;
  const OriginMap& origins_;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_ABSTRACTMEMORYBUILDER_H_
