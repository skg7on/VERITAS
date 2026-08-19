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

#ifndef VERITAS_SUMMARY_LOCAL_SUMMARY_BUILDER_H_
#define VERITAS_SUMMARY_LOCAL_SUMMARY_BUILDER_H_

#include <string>
#include <vector>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::summary {

// FunctionLocalFacts is the M4 handoff: per-function local facts extracted from
// LLVM IR before interprocedural analysis. It is converted into an immutable
// FunctionSummary draft by BuildLocalSummary. Facts are represented with the
// same proto messages that FunctionSummary stores, so no parallel fact model is
// introduced.
struct FunctionLocalFacts {
  std::string function_symbol_id;
  std::string function_variant_id;
  std::vector<v1::Call> calls;
  std::vector<v1::MemoryEffect> memory_effects;
  std::vector<v1::ValueFlow> value_flows;
  std::vector<v1::BasicBlockSummaryRef> basic_block_summaries;
  std::vector<v1::DominatorSummaryFact> dominator_summaries;
  std::vector<v1::RangeFact> range_facts;
  std::vector<v1::Unknown> unknowns;
};

// Build an immutable FunctionSummary draft from local facts. The draft carries
// full identity (repository/revision/build/function-variant) and is not
// published until M5 merges required SVF results. Repeated construction from
// equivalent input produces byte-identical summaries (creation_epoch_ms is
// pinned to zero and repeated facts are sorted), keeping FunctionSummaryID
// deterministic.
StatusOr<v1::FunctionSummary> BuildLocalSummary(
    const FunctionLocalFacts& facts,
    const build::ProgramContext& context);

}  // namespace veritas::summary

#endif  // VERITAS_SUMMARY_LOCAL_SUMMARY_BUILDER_H_
