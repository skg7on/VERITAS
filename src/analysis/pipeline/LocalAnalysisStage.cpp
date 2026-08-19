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

#include "analysis/pipeline/LocalAnalysisStage.h"

#include <vector>

#include "analysis/llvm/LocalFactExtractor.h"
#include "analysis/llvm/ProjectIrBuilder.h"
#include "veritas/summary/LocalSummaryBuilder.h"

namespace veritas::analysis::pipeline {

StatusOr<LocalAnalysisResult> RunLocalAnalysis(
    const build::AnalysisManifest& manifest) {
  llvm::ProjectIrBuilder ir_builder;
  auto program_ir = ir_builder.BuildProjectIr(manifest);
  if (!program_ir.ok()) {
    return program_ir.status();
  }

  llvm::LocalFactExtractor fact_extractor;
  auto local_facts = fact_extractor.Extract(*program_ir);
  if (!local_facts.ok()) {
    return local_facts.status();
  }

  std::vector<summary::v1::FunctionSummary> drafts;
  drafts.reserve(local_facts->size());
  for (const auto& facts : *local_facts) {
    auto summary = summary::BuildLocalSummary(facts, manifest.context);
    if (!summary.ok()) {
      return summary.status();
    }
    drafts.push_back(std::move(*summary));
  }

  return LocalAnalysisResult{std::move(*program_ir), std::move(drafts)};
}

}  // namespace veritas::analysis::pipeline
