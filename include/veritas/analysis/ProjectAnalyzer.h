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

#ifndef VERITAS_ANALYSIS_PROJECTANALYZER_H_
#define VERITAS_ANALYSIS_PROJECTANALYZER_H_

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/core/Status.h"

namespace veritas::analysis {

// Forward declarations to avoid exposing implementation types
namespace internal {
class ProjectAnalyzerTestFactory;
}  // namespace internal

// AnalysisCompletion indicates whether analysis completed fully or with unknowns
enum class AnalysisCompletion {
  kComplete,
  kCompleteWithUnknowns,
};

// The recursive WPA engine. There is no automatic fallback: selecting the C++
// engine is explicit and separately identified.
enum class WpaEngineMode : std::uint8_t {
  kSouffle,
  kCppEmergency,
};

// AnalysisConfig provides budget and tuning parameters for project analysis
struct AnalysisConfig {
  std::chrono::seconds svf_soft_analysis_budget;
  std::size_t svf_max_graph_nodes;
  std::size_t svf_max_emitted_facts;
  std::size_t svf_max_alias_pairs;
  bool svf_field_sensitive;

  WpaEngineMode wpa_engine;
  std::chrono::milliseconds wpa_component_timeout;
  std::uint64_t wpa_component_memory_mb;
  std::uint32_t wpa_threads;
  std::string rule_bundle_version;
  std::string model_bundle_version;
  bool run_cpp_conformance_oracle;

  static AnalysisConfig Default();
};

// UnknownFact represents a scope where analysis was incomplete or uncertain
struct UnknownFact {
  std::string scope;
  std::string reason;
  std::string provenance;
};

// ProjectAnalysisResult contains the outcome of analyzing a project
struct ProjectAnalysisResult {
  AnalysisCompletion completion;
  std::string program_context_id;
  // The exact revision and build-variant context the summaries were published
  // under, so version-aware readers (e.g. ListCurrentSummaryArtifacts) cite the
  // same coordinates the publication coordinator staged.
  std::string revision_id;
  std::string build_variant_id;
  std::vector<std::string> published_summary_ids;
  std::vector<UnknownFact> unknowns;
  std::string projection_id;
  std::size_t cpg_node_count = 0;
  std::size_t cpg_edge_count = 0;
  // WPA outcome. Empty when the run did not complete; wpa_diagnostics carries
  // the failure reason or degraded-mode marker.
  std::string wpa_run_id;
  WpaEngineMode wpa_engine = WpaEngineMode::kSouffle;
  std::string wpa_diagnostics;
};

// ProjectAnalyzer orchestrates the full M1→M4→M5→M3 analysis pipeline.
//
// The standard workflow:
// 1. M1: Resolve the project and load its compile_commands.json manifest
// 2. M2: Persist the program context
// 3. M4: Build linked LLVM IR and extract local facts into summary drafts
// 4. M5: Run the required SVF analysis and merge results conservatively
// 5. M3: Publish the completed summaries atomically
//
// SVF is a required stage. If SVF construction fails, no summaries are published.
class ProjectAnalyzer {
 public:
  ProjectAnalyzer();
  ~ProjectAnalyzer();

  // Non-copyable, movable
  ProjectAnalyzer(const ProjectAnalyzer&) = delete;
  ProjectAnalyzer& operator=(const ProjectAnalyzer&) = delete;
  ProjectAnalyzer(ProjectAnalyzer&&) noexcept;
  ProjectAnalyzer& operator=(ProjectAnalyzer&&) noexcept;

  // AnalyzeProject runs the full pipeline on the specified project.
  //
  // Fails if:
  // - project_root is invalid or missing compile_commands.json
  // - SVF construction fails (fatal, not a budget limit)
  // - publication fails
  //
  // Does NOT fail on budget limits or unmapped SVF nodes; those return
  // kCompleteWithUnknowns with explanatory unknowns.
  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request, const AnalysisConfig& config);

 private:
  class Impl;
  explicit ProjectAnalyzer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class internal::ProjectAnalyzerTestFactory;
};

}  // namespace veritas::analysis

#endif  // VERITAS_ANALYSIS_PROJECTANALYZER_H_
