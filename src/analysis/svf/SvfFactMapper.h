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

#ifndef VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_
#define VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_

#include <string>
#include <vector>

#include "analysis/svf/SvfConfig.h"
#include "analysis/svf/SvfSession.h"
#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"

namespace veritas::analysis::pipeline {
class ProgramIr;
}  // namespace veritas::analysis::pipeline

namespace veritas::analysis::svf {

// AnalyzerRunContext provides identity and provenance for this analysis run.
struct AnalyzerRunContext {
  std::string analyzer_run_id;
  std::string llvm_toolchain_identity;
  std::string program_module_hash;
};

// SvfFacts contains all VERITAS-normalized facts mapped from SVF results. The
// facts carry stable VERITAS identities (core::StableId) and typed semantic
// enums; SVF-native nodes never leave this boundary.
using SvfFacts = veritas::analysis::semantic::NormalizedAnalysisFacts;

// SvfMappingCompletion indicates whether mapping completed fully or with
// unknowns.
enum class SvfMappingCompletion {
  kComplete,
  kCompleteWithUnknowns,
};

// SvfMappingResult packages the completion status with mapped facts.
struct SvfMappingResult {
  SvfMappingCompletion completion;
  SvfFacts facts;
};

// MapSvfFacts translates SVF analysis results into normalized VERITAS facts.
//
// Resolves SVF values through LLVM values, the Task 7 StableValueMapper /
// AbstractMemoryBuilder, and the ProgramIr origin map. Maps value-flow edges,
// alias relationships (all four AliasKind values with independent epistemic
// state), memory effects (loads/stores), and indirect-call targets. Every fact
// carries stable identities and complete analyzer provenance.
//
// Returns kComplete when all SVF results mapped successfully, or
// kCompleteWithUnknowns when some results could not be resolved (unmapped SVF
// nodes and empty/unknown indirect-call sets become scoped NormalizedUnknowns).
Status MapSvfFacts(const pipeline::ProgramIr& program_ir,
                   const SvfSessionView& view,
                   const AnalyzerRunContext& run_context,
                   const SvfConfig& config,
                   SvfMappingResult* result);

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_
