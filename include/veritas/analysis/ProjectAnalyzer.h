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

// AnalysisConfig provides budget and tuning parameters for project analysis
struct AnalysisConfig {
  std::chrono::seconds svf_soft_analysis_budget;
  std::size_t svf_max_graph_nodes;
  std::size_t svf_max_emitted_facts;

  static AnalysisConfig Default();
};

// ProjectAnalysisRequest specifies the input for a project analysis
struct ProjectAnalysisRequest {
  std::string project_root;
  std::string output_root;
};

// UnknownFact represents a scope where analysis was incomplete or uncertain
// (placeholder - in real implementation from include/veritas/summary/)
struct UnknownFact {
  std::string scope;
  std::string reason;
  std::string provenance;
};

// ProjectAnalysisResult contains the outcome of analyzing a project
struct ProjectAnalysisResult {
  AnalysisCompletion completion;
  std::string program_context_id;
  std::vector<std::string> published_summary_ids;
  std::vector<UnknownFact> unknowns;
};

// ProjectAnalyzer orchestrates the full M1→M4→M5 analysis pipeline.
//
// The standard workflow:
// 1. M1: Load compile_commands.json
// 2. M4: Run Clang and build LLVM IR, extract local facts
// 3. M5: Run required SVF analysis on linked IR
// 4. M5: Map SVF results and merge conservatively with M4 facts
// 5. M3: Publish atomically to SummaryRepository
//
// SVF is a required stage. If SVF construction fails, no summaries are published.
// If SVF completes with budget limits, partial validated facts are published with
// unknowns documenting the truncation.
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
  // Returns the completion status, context ID, published summary IDs, and
  // any unknowns. Fails if:
  // - project_root is invalid or missing compile_commands.json
  // - SVF construction fails (fatal, not a budget limit)
  // - publication fails
  //
  // Does NOT fail on budget limits or unmapped SVF nodes; those return
  // kCompleteWithUnknowns with explanatory unknowns.
  ProjectAnalysisResult AnalyzeProject(const ProjectAnalysisRequest& request,
                                        const AnalysisConfig& config);

 private:
  class Impl;
  explicit ProjectAnalyzer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class internal::ProjectAnalyzerTestFactory;
};

}  // namespace veritas::analysis

#endif  // VERITAS_ANALYSIS_PROJECTANALYZER_H_
