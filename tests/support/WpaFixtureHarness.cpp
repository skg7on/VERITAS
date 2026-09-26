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

#include "WpaFixtureHarness.h"

#include <system_error>
#include <map>
#include <string>
#include <utility>

#include "ProjectFixture.h"
#include "analysis/pipeline/LocalAnalysisStage.h"
#include "analysis/pipeline/ProgramIr.h"
#include "analysis/svf/SvfAnalysisStage.h"
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/summarydb/SummaryRepository.h"

namespace veritas::testing {

class FixtureRootOwner {
 public:
  explicit FixtureRootOwner(std::filesystem::path project_root)
      : project_root_(std::move(project_root)) {}

  ~FixtureRootOwner() {
    std::error_code error;
    std::filesystem::remove_all(project_root_, error);
  }

  const std::filesystem::path& project_root() const { return project_root_; }

 private:
  std::filesystem::path project_root_;
};

namespace {

StatusOr<build::AnalysisManifest> LoadFixtureManifest(
    const std::filesystem::path& project_root) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = project_root,
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok()) {
    return input.status();
  }
  return build::LoadProjectManifest(*input);
}

bool HasPrefix(std::string_view value, std::string_view prefix) {
  return value.starts_with(prefix);
}

StatusOr<std::map<std::string, std::string>> FunctionVariantIdsBySymbol(
    const analysis::pipeline::ProgramIr& program_ir) {
  const ::llvm::Module* module = program_ir.GetModule();
  if (module == nullptr) {
    return Status::Internal("fixture local analysis produced no module");
  }
  std::map<std::string, std::string> function_variant_ids_by_symbol;
  for (const auto& function : *module) {
    if (function.isDeclaration()) {
      continue;
    }
    const auto id = program_ir.origin_map().GetSymbolId(&function);
    if (!id.has_value()) {
      return Status::Internal("fixture function has no origin identity");
    }
    function_variant_ids_by_symbol.emplace(function.getName().str(), *id);
  }
  return function_variant_ids_by_symbol;
}

}  // namespace

SvfFixtureSnapshot::SvfFixtureSnapshot(
    std::unique_ptr<FixtureRootOwner> fixture_root,
    std::filesystem::path project_root, analysis::svf::SvfMappingResult mapping,
    std::map<std::string, std::string> function_variant_ids_by_symbol)
    : fixture_root_(std::move(fixture_root)),
      project_root(std::move(project_root)),
      mapping(std::move(mapping)),
      function_variant_ids_by_symbol(
          std::move(function_variant_ids_by_symbol)) {}

SvfFixtureSnapshot::SvfFixtureSnapshot(SvfFixtureSnapshot&&) noexcept =
    default;

SvfFixtureSnapshot& SvfFixtureSnapshot::operator=(
    SvfFixtureSnapshot&&) noexcept = default;

SvfFixtureSnapshot::~SvfFixtureSnapshot() = default;

LocalFixtureSnapshot::LocalFixtureSnapshot(
    std::unique_ptr<FixtureRootOwner> fixture_root,
    std::filesystem::path project_root,
    std::vector<summary::v2::FunctionSummary> summaries,
    std::map<std::string, std::string> function_variant_ids_by_symbol)
    : fixture_root_(std::move(fixture_root)),
      project_root(std::move(project_root)),
      summaries(std::move(summaries)),
      function_variant_ids_by_symbol(
          std::move(function_variant_ids_by_symbol)) {}

LocalFixtureSnapshot::LocalFixtureSnapshot(LocalFixtureSnapshot&&) noexcept =
    default;

LocalFixtureSnapshot& LocalFixtureSnapshot::operator=(
    LocalFixtureSnapshot&&) noexcept = default;

LocalFixtureSnapshot::~LocalFixtureSnapshot() = default;

AnalyzedFixtureSnapshot::AnalyzedFixtureSnapshot(
    std::unique_ptr<FixtureRootOwner> fixture_root,
    std::filesystem::path project_root, std::filesystem::path output_root,
    analysis::ProjectAnalysisResult analysis,
    std::vector<summary::SummaryArtifact> summaries,
    std::map<std::string, std::string> function_variant_ids_by_symbol)
    : fixture_root_(std::move(fixture_root)),
      project_root(std::move(project_root)),
      output_root(std::move(output_root)),
      analysis(std::move(analysis)),
      summaries(std::move(summaries)),
      function_variant_ids_by_symbol(
          std::move(function_variant_ids_by_symbol)) {}

AnalyzedFixtureSnapshot::AnalyzedFixtureSnapshot(
    AnalyzedFixtureSnapshot&&) noexcept = default;

AnalyzedFixtureSnapshot& AnalyzedFixtureSnapshot::operator=(
    AnalyzedFixtureSnapshot&&) noexcept = default;

AnalyzedFixtureSnapshot::~AnalyzedFixtureSnapshot() = default;

StatusOr<SvfFixtureSnapshot> MapFixtureWithSvf(
    std::string_view name, const analysis::svf::SvfConfig& config) {
  auto fixture_root = std::make_unique<FixtureRootOwner>(FixtureProject(name));
  const auto project_root = fixture_root->project_root();
  auto manifest = LoadFixtureManifest(project_root);
  if (!manifest.ok()) {
    return manifest.status();
  }

  auto local = analysis::pipeline::RunLocalAnalysis(*manifest);
  if (!local.ok()) {
    return local.status();
  }
  auto function_variant_ids_by_symbol =
      FunctionVariantIdsBySymbol(local->program_ir);
  if (!function_variant_ids_by_symbol.ok()) {
    return function_variant_ids_by_symbol.status();
  }

  const analysis::svf::AnalyzerRunContext run_context{
      .analyzer_run_id = manifest->context.revision_id,
      .llvm_toolchain_identity = "llvm",
      .program_module_hash = std::string(local->program_ir.module_hash()),
  };
  analysis::svf::SvfAnalysisStage stage;
  auto mapping = stage.Analyze(local->program_ir, run_context, config);
  if (!mapping.ok()) {
    return mapping.status();
  }

  return SvfFixtureSnapshot(std::move(fixture_root), project_root,
                            std::move(*mapping),
                            std::move(*function_variant_ids_by_symbol));
}

StatusOr<LocalFixtureSnapshot> AnalyzeLocalFixture(std::string_view name) {
  auto fixture_root = std::make_unique<FixtureRootOwner>(FixtureProject(name));
  const auto project_root = fixture_root->project_root();
  auto manifest = LoadFixtureManifest(project_root);
  if (!manifest.ok()) {
    return manifest.status();
  }
  auto local = analysis::pipeline::RunLocalAnalysis(*manifest);
  if (!local.ok()) {
    return local.status();
  }
  auto function_variant_ids_by_symbol =
      FunctionVariantIdsBySymbol(local->program_ir);
  if (!function_variant_ids_by_symbol.ok()) {
    return function_variant_ids_by_symbol.status();
  }
  return LocalFixtureSnapshot(std::move(fixture_root), project_root,
                              std::move(local->summary_drafts),
                              std::move(*function_variant_ids_by_symbol));
}

StatusOr<AnalyzedFixtureSnapshot> AnalyzeAndLoadFixture(
    std::string_view name, const analysis::AnalysisConfig& config) {
  auto fixture_root = std::make_unique<FixtureRootOwner>(FixtureProject(name));
  const auto project_root = fixture_root->project_root();
  const auto output_root = project_root / ".veritas";
  auto manifest = LoadFixtureManifest(project_root);
  if (!manifest.ok()) {
    return manifest.status();
  }
  auto local = analysis::pipeline::RunLocalAnalysis(*manifest);
  if (!local.ok()) {
    return local.status();
  }
  auto function_variant_ids_by_symbol =
      FunctionVariantIdsBySymbol(local->program_ir);
  if (!function_variant_ids_by_symbol.ok()) {
    return function_variant_ids_by_symbol.status();
  }
  const analysis::ProjectAnalysisRequest request{
      .project_root = project_root,
      .output_root = output_root,
  };
  analysis::ProjectAnalyzer analyzer;
  auto result = analyzer.AnalyzeProject(request, config);
  if (!result.ok()) {
    return result.status();
  }

  auto repository = summarydb::SummaryRepository::Open(output_root.string());
  if (!repository.ok()) {
    return repository.status();
  }
  auto summaries = (*repository)->ListCurrentSummaryArtifacts(
      result->revision_id, result->build_variant_id);
  if (!summaries.ok()) {
    return summaries.status();
  }

  return AnalyzedFixtureSnapshot(std::move(fixture_root), project_root,
                                 output_root, std::move(*result),
                                 std::move(*summaries),
                                 std::move(*function_variant_ids_by_symbol));
}

bool AllArtifactsAreV2(
    std::span<const summary::SummaryArtifact> artifacts) {
  for (const auto& artifact : artifacts) {
    const auto* function =
        std::get_if<summary::v2::FunctionSummary>(&artifact);
    if (function == nullptr ||
        !HasPrefix(function->header().schema_version(), "summary.v2")) {
      return false;
    }
  }
  return true;
}

std::vector<const summary::v2::Call*> CallsWithDispatch(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::DispatchKind dispatch) {
  std::vector<const summary::v2::Call*> matches;
  for (const auto& artifact : artifacts) {
    const auto* function =
        std::get_if<summary::v2::FunctionSummary>(&artifact);
    if (function == nullptr) {
      continue;
    }
    for (const auto& call : function->calls()) {
      if (call.dispatch() == dispatch &&
          HasPrefix(call.call_site_id(), "callsite:sha256:")) {
        matches.push_back(&call);
      }
    }
  }
  return matches;
}

std::vector<const summary::v2::MemoryEffect*> MemoryEffectsWithObjectKind(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::AbstractObjectKind kind) {
  std::vector<const summary::v2::MemoryEffect*> matches;
  for (const auto& artifact : artifacts) {
    const auto* function =
        std::get_if<summary::v2::FunctionSummary>(&artifact);
    if (function == nullptr) {
      continue;
    }
    for (const auto& effect : function->memory_effects()) {
      if (effect.location().object().kind() == kind &&
          HasPrefix(effect.location().memory_location_id(),
                    "memref:sha256:")) {
        matches.push_back(&effect);
      }
    }
  }
  return matches;
}

}  // namespace veritas::testing
