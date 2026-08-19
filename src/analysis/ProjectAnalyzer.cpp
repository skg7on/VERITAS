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

#include <string>

#include "analysis/cpg/CpgProjectionStage.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "analysis/svf/SvfAnalysisStage.h"
#include "analysis/svf/SvfConfig.h"
#include "analysis/svf/SvfMerge.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Ids.h"

#include "ProjectAnalyzerInternal.h"
#include "ProjectPublicationCoordinator.h"

namespace veritas::analysis {

AnalysisConfig AnalysisConfig::Default() {
  auto svf_config = svf::SvfConfig::Default();
  return AnalysisConfig{
      .svf_soft_analysis_budget = svf_config.soft_analysis_budget,
      .svf_max_graph_nodes = svf_config.max_graph_nodes,
      .svf_max_emitted_facts = svf_config.max_emitted_facts,
  };
}

namespace {

svf::SvfConfig ToSvfConfig(const AnalysisConfig& config) {
  return svf::SvfConfig{
      .pointer_analysis = svf::PointerAnalysisKind::kAndersenWaveDiff,
      .soft_analysis_budget = config.svf_soft_analysis_budget,
      .max_graph_nodes = config.svf_max_graph_nodes,
      .max_emitted_facts = config.svf_max_emitted_facts,
      .field_sensitive = true,
  };
}

}  // namespace

// ProjectAnalyzer::Impl holds the implementation details
class ProjectAnalyzer::Impl {
 public:
  Impl() : svf_stage_(std::make_unique<svf::SvfAnalysisStage>()) {}

  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request, const AnalysisConfig& config) {
    // M1: resolve and load the project manifest.
    auto input = build::ResolveProjectInput(request);
    if (!input.ok()) return input.status();
    auto manifest = build::LoadProjectManifest(*input);
    if (!manifest.ok()) return manifest.status();

    // M4: build linked IR and extract local summary drafts.
    auto local = pipeline::RunLocalAnalysis(*manifest);
    if (!local.ok()) return local.status();

    // M5: run the required SVF stage over the live ProgramIr.
    svf::AnalyzerRunContext run_context{
        .analyzer_run_id = manifest->context.revision_id,
        .llvm_toolchain_identity = "llvm",
        .program_module_hash = std::string(local->program_ir.module_hash()),
    };
    auto svf_result = svf_stage_->Analyze(local->program_ir, run_context,
                                          ToSvfConfig(config));
    if (!svf_result.ok()) return svf_result.status();

    // Merge SVF facts conservatively into the M4 drafts.
    auto merged = svf::MergeSvfFacts(std::move(local->summary_drafts),
                                     svf_result->facts);

    // M6: project the CPG from the live ProgramIr and completed summaries.
    auto revision_id = core::ParseStableId(manifest->context.revision_id);
    auto build_variant_id = core::ParseStableId(manifest->context.build_variant_id);
    if (!revision_id.ok()) return revision_id.status();
    if (!build_variant_id.ok()) return build_variant_id.status();
    auto graph = cpg::BuildThinCpg(cpg::CpgProjectionInput{
        .program_ir = local->program_ir,
        .completed_summaries = merged,
        .revision_id = *revision_id,
        .build_variant_id = *build_variant_id,
    });
    if (!graph.ok()) return graph.status();

    // M2 + M3 + M6: persist the context and publish summaries + CPG atomically.
    auto coordinator =
        ProjectPublicationCoordinator::Open(input->output_root.string());
    if (!coordinator.ok()) return coordinator.status();
    auto persist = (*coordinator)->PersistManifestContext(*manifest);
    if (!persist.ok()) return persist;
    auto published = (*coordinator)->Publish(
        CompletedProjectAnalysis{std::move(merged), std::move(*graph)});
    if (!published.ok()) return published.status();

    ProjectAnalysisResult result;
    result.completion =
        svf_result->completion == svf::SvfMappingCompletion::kComplete
            ? AnalysisCompletion::kComplete
            : AnalysisCompletion::kCompleteWithUnknowns;
    result.program_context_id = manifest->context.revision_id;
    result.published_summary_ids.reserve(published->size());
    for (const auto& id : *published) {
      result.published_summary_ids.push_back(core::ToString(id));
    }
    result.unknowns.reserve(svf_result->facts.unknowns.size());
    for (const auto& unknown : svf_result->facts.unknowns) {
      result.unknowns.push_back(
          UnknownFact{unknown.scope, unknown.reason, unknown.provenance});
    }
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

ProjectAnalyzer& ProjectAnalyzer::operator=(ProjectAnalyzer&&) noexcept =
    default;

StatusOr<ProjectAnalysisResult> ProjectAnalyzer::AnalyzeProject(
    const ProjectAnalysisRequest& request, const AnalysisConfig& config) {
  return impl_->AnalyzeProject(request, config);
}

// Private constructor for test factory
ProjectAnalyzer::ProjectAnalyzer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ProjectAnalyzer internal::ProjectAnalyzerTestFactory::Create(
    std::unique_ptr<svf::SvfAnalysisStage> stage) {
  auto impl = std::make_unique<ProjectAnalyzer::Impl>();
  impl->SetSvfStage(std::move(stage));
  return ProjectAnalyzer(std::move(impl));
}

}  // namespace veritas::analysis
