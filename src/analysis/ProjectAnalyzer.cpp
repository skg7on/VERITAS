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

#include "veritas/analysis/ProjectAnalyzer.h"

#include "analysis/svf/SvfAnalysisStage.h"
#include "analysis/svf/SvfConfig.h"

namespace veritas::analysis {

AnalysisConfig AnalysisConfig::Default() {
  auto svf_config = svf::SvfConfig::Default();
  return AnalysisConfig{
      .svf_soft_analysis_budget = svf_config.soft_analysis_budget,
      .svf_max_graph_nodes = svf_config.max_graph_nodes,
      .svf_max_emitted_facts = svf_config.max_emitted_facts,
  };
}

// ProjectAnalyzer::Impl holds the implementation details
class ProjectAnalyzer::Impl {
 public:
  Impl() : svf_stage_(std::make_unique<svf::SvfAnalysisStage>()) {}

  ProjectAnalysisResult AnalyzeProject(const ProjectAnalysisRequest& request,
                                        const AnalysisConfig& config) {
    // Placeholder implementation showing the pipeline structure
    // In real implementation would:
    // 1. Call M1 to load compile_commands.json from request.project_root
    // 2. Call M4 to run Clang and build ProgramIr + local summaries
    // 3. Create AnalyzerRunContext with IDs
    // 4. Call SVF stage (below)
    // 5. Merge SVF facts conservatively with M4 summaries
    // 6. Publish atomically to SummaryRepository

    // For now, demonstrate the SVF stage structure
    svf::SvfConfig svf_config{
        .pointer_analysis = svf::PointerAnalysisKind::kAndersenWaveDiff,
        .soft_analysis_budget = config.svf_soft_analysis_budget,
        .max_graph_nodes = config.svf_max_graph_nodes,
        .max_emitted_facts = config.svf_max_emitted_facts,
        .field_sensitive = true,
    };

    // Placeholder return
    ProjectAnalysisResult result;
    result.completion = AnalysisCompletion::kComplete;
    result.program_context_id = "placeholder_context";
    return result;
  }

  // Test seam for dependency injection
  void SetSvfStage(std::unique_ptr<svf::SvfAnalysisStage> stage) {
    svf_stage_ = std::move(stage);
  }

 private:
  std::unique_ptr<svf::SvfAnalysisStage> svf_stage_;
};

ProjectAnalyzer::ProjectAnalyzer()
    : impl_(std::make_unique<Impl>()) {}

ProjectAnalyzer::~ProjectAnalyzer() = default;

ProjectAnalyzer::ProjectAnalyzer(ProjectAnalyzer&&) noexcept = default;

ProjectAnalyzer& ProjectAnalyzer::operator=(ProjectAnalyzer&&) noexcept = default;

ProjectAnalysisResult ProjectAnalyzer::AnalyzeProject(
    const ProjectAnalysisRequest& request,
    const AnalysisConfig& config) {
  return impl_->AnalyzeProject(request, config);
}

// Private constructor for test factory
ProjectAnalyzer::ProjectAnalyzer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

}  // namespace veritas::analysis
