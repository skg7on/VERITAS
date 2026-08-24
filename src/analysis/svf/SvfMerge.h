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

#ifndef VERITAS_ANALYSIS_SVF_SVFMERGE_H_
#define VERITAS_ANALYSIS_SVF_SVFMERGE_H_

#include <vector>

#include "veritas/analysis/semantic/ModelBundle.h"
#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::analysis::svf {

namespace semantic = veritas::analysis::semantic;

}  // namespace veritas::analysis::svf

namespace veritas::analysis::llvm {
class OriginMap;
}  // namespace veritas::analysis::llvm

namespace veritas::analysis::svf {

// Merge SVF-mapped facts into M4 summary drafts. M4 MUST facts are never
// erased; SVF calls (attributed to their caller via diagnostic_symbol) and
// scoped unknowns augment the matching draft. Unknowns are never dropped.
//
// Value-flow, alias, and memory-effect facts are whole-program (their stable
// IDs carry no recoverable per-function owner) and are therefore NOT merged
// into the per-function summary.v1 drafts here — publishing them would
// fabricate cross-function facts (e.g. a MUST-level NO_ALIAS inside an
// unrelated function). Task 9 re-adds them with precise per-function
// attribution against summary.v2.
std::vector<::veritas::summary::v1::FunctionSummary> MergeSvfFacts(
    std::vector<::veritas::summary::v1::FunctionSummary> drafts,
    const semantic::NormalizedAnalysisFacts& svf_facts,
    const ::veritas::analysis::llvm::OriginMap& origin_map);

// Merge SVF-mapped facts into summary.v2 drafts by stable owning function ID.
//
//   * Calls attribute to the caller's draft via the caller's stable function
//     value ref (recomputed from the draft's function-variant ID).
//   * Memory-effect and alias facts attribute via
//     location.object.owner_function (a function-variant ID).
//   * Model effects attach to the modeled function's draft (matched by exact
//     symbol against the draft identity; external-function models, whose
//     functions have no local summary, contribute no per-function facts here
//     and are surfaced to the WPA materializer via the bundle).
//   * Run-level unknowns attach to every draft (matching V1).
//
// Facts with no recoverable per-function owner — value flows and dependencies,
// whose kValueRef identities carry no owner — remain gated out. Component
// digests are recomputed after the merge so the resulting summaries are
// content-addressed over their merged semantic content.
StatusOr<std::vector<::veritas::summary::v2::FunctionSummary>> MergeSvfFactsV2(
    std::vector<::veritas::summary::v2::FunctionSummary> drafts,
    const semantic::NormalizedAnalysisFacts& svf_facts,
    const semantic::ModelBundle& model_bundle);

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFMERGE_H_
