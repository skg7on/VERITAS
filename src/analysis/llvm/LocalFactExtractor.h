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

#ifndef VERITAS_ANALYSIS_LLVM_LOCALFACTEXTRACTOR_H_
#define VERITAS_ANALYSIS_LLVM_LOCALFACTEXTRACTOR_H_

#include <vector>

#include "veritas/core/Status.h"
#include "veritas/summary/LocalSummaryBuilder.h"

namespace veritas::analysis::pipeline {
class ProgramIr;
}  // namespace veritas::analysis::pipeline

namespace veritas::analysis::llvm {

// LocalFactExtractor produces per-function local facts (direct calls, memory
// effects, value flows, and scoped unknowns) from a live linked ProgramIr. It
// does not expand callees; interprocedural facts arrive with M5. Facts resolve
// through the ProgramIr origin map and are never persisted with LLVM pointers.
class LocalFactExtractor {
 public:
  veritas::StatusOr<std::vector<summary::FunctionLocalFacts>> Extract(
      pipeline::ProgramIr& program_ir) const;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_LOCALFACTEXTRACTOR_H_
