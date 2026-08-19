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

#ifndef VERITAS_ANALYSIS_CPG_CPGPROJECTIONSTAGE_H_
#define VERITAS_ANALYSIS_CPG_CPGPROJECTIONSTAGE_H_

#include <span>

#include "analysis/pipeline/ProgramIr.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::analysis::cpg {

// CpgProjectionInput is the private, engine-neutral input boundary for the M6
// projection stage. It borrows the live ProgramIr and completed summaries; no
// native pointer or third-party ID may escape in a node, edge, or diagnostic.
struct CpgProjectionInput {
  const pipeline::ProgramIr& program_ir;
  std::span<const summary::v1::FunctionSummary> completed_summaries;
  core::StableId revision_id;
  core::StableId build_variant_id;
};

// BuildThinCpg projects the live ProgramIr and completed summaries into a
// validated ThinCpg. It consumes borrowed objects only during the call.
StatusOr<::veritas::cpg::ThinCpg> BuildThinCpg(const CpgProjectionInput& input);

}  // namespace veritas::analysis::cpg

#endif  // VERITAS_ANALYSIS_CPG_CPGPROJECTIONSTAGE_H_
