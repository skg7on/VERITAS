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

#include "SvfAnalysisStage.h"

#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {

Status SvfAnalysisStage::Analyze(pipeline::ProgramIr& program_ir,
                                  const AnalyzerRunContext& run_context,
                                  const SvfConfig& config,
                                  SvfMappingResult* result) {
  if (!result) {
    return Status::Internal("result is null");
  }

  // Run SVF session and map facts inside the callback
  auto status = RunWithSvfSession(
      program_ir, config,
      [&](const SvfSessionView& view) {
        return MapSvfFacts(program_ir, view, run_context, config, result);
      });

  if (!status.ok()) {
    return status;
  }

  // Verify the callback populated the result
  // (MapSvfFacts should have set result, but verify)
  return Status::Ok();
}

}  // namespace veritas::analysis::svf
