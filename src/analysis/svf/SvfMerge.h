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

#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::analysis::svf {

namespace semantic = veritas::analysis::semantic;

}  // namespace veritas::analysis::svf

namespace veritas::analysis::llvm {
class OriginMap;
}  // namespace veritas::analysis::llvm

namespace veritas::analysis::svf {

// Merge SVF-mapped facts into M4 summary drafts. M4 MUST facts are never
// erased; SVF value-flow, alias, memory-effect, call, and unknown facts augment
// the matching draft. Unknowns are never dropped.
//
// Normalized SVF facts carry stable IDs without per-function ownership; calls
// are attributed to their caller via diagnostic_symbol, and the remaining
// whole-program facts are attached conservatively (Task 9 performs proper
// per-function attribution against summary.v2).
std::vector<::veritas::summary::v1::FunctionSummary> MergeSvfFacts(
    std::vector<::veritas::summary::v1::FunctionSummary> drafts,
    const semantic::NormalizedAnalysisFacts& svf_facts,
    const ::veritas::analysis::llvm::OriginMap& origin_map);

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFMERGE_H_
