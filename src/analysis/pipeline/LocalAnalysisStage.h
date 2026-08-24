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

#ifndef VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_
#define VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_

#include <vector>

#include "analysis/pipeline/ProgramIr.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::analysis::pipeline {

// LocalAnalysisResult is the M4-to-M5 handoff: a live linked ProgramIr plus the
// unpublished local summary.v2 drafts that M5 merges with SVF facts. The
// ProgramIr stays alive so M5 and M6 can borrow it before publication.
struct LocalAnalysisResult {
  ProgramIr program_ir;
  std::vector<summary::v2::FunctionSummary> summary_drafts;
};

// RunLocalAnalysis builds the linked whole-program IR from an M1 manifest,
// extracts local facts, and produces unpublished summary drafts. It does not
// publish; M5 owns merge and publication.
StatusOr<LocalAnalysisResult> RunLocalAnalysis(
    const build::AnalysisManifest& manifest);

}  // namespace veritas::analysis::pipeline

#endif  // VERITAS_ANALYSIS_PIPELINE_LOCALANALYSISSTAGE_H_
