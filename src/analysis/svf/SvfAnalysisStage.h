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

#ifndef VERITAS_ANALYSIS_SVF_SVFANALYSISSTAGE_H_
#define VERITAS_ANALYSIS_SVF_SVFANALYSISSTAGE_H_

#include "analysis/svf/SvfConfig.h"
#include "analysis/svf/SvfFactMapper.h"
#include "analysis/svf/SvfSession.h"

namespace veritas::analysis::pipeline {
class ProgramIr;
}  // namespace veritas::analysis::pipeline

namespace veritas::analysis::svf {

// SvfAnalysisStage is the required SVF pointer analysis stage in the M5 pipeline.
//
// It runs SVF directly on the live ProgramIr, maps results to VERITAS Summary IR,
// and returns either kComplete or kCompleteWithUnknowns based on mapping success
// and budget limits.
//
// This is the only implementation of the required SVF stage. There is no
// disabled mode or optional path.
class SvfAnalysisStage {
 public:
  virtual ~SvfAnalysisStage() = default;

  // Analyze runs the required SVF stage on program_ir.
  //
  // Steps:
  // 1. Run RunWithSvfSession to build SVF from live LLVM module
  // 2. Inside the session callback, call MapSvfFacts
  // 3. Return the mapped facts and completion status
  //
  // Returns error if SVF construction fails (fatal).
  // Returns success with kCompleteWithUnknowns if budget limits were reached
  // or SVF nodes could not be mapped.
  virtual StatusOr<SvfMappingResult> Analyze(
      pipeline::ProgramIr& program_ir,
      const AnalyzerRunContext& run_context,
      const SvfConfig& config);
};

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFANALYSISSTAGE_H_
