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

#ifndef VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_
#define VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_

#include "analysis/pipeline/ProgramIr.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace llvm {
class Function;
}

namespace veritas::analysis::llvm {

namespace detail {

core::StableId FunctionSymbolId(const ::llvm::Function &function,
                                const build::TranslationUnitCommand &command,
                                const build::ProgramContext &context);

} // namespace detail

// ProjectIrBuilder generates LLVM IR for every translation unit in an M1
// manifest, links the modules into a single whole-program module, computes a
// deterministic module hash, and populates the LLVM-to-VERITAS origin map.
//
// The result is a move-only ProgramIr that owns the linked module and its
// LLVMContext. No native pointer escapes into persisted identity.
class ProjectIrBuilder {
public:
  ProjectIrBuilder() = default;
  ~ProjectIrBuilder() = default;

  veritas::StatusOr<pipeline::ProgramIr>
  BuildProjectIr(const build::AnalysisManifest &manifest);
};

} // namespace veritas::analysis::llvm

#endif // VERITAS_ANALYSIS_LLVM_PROJECTIRBUILDER_H_
