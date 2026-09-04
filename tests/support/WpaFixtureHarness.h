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

#ifndef VERITAS_TESTS_SUPPORT_WPAFIXTUREHARNESS_H_
#define VERITAS_TESTS_SUPPORT_WPAFIXTUREHARNESS_H_

#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "analysis/svf/SvfConfig.h"
#include "analysis/svf/SvfFactMapper.h"
#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/core/Status.h"
#include "veritas/summary/SummaryArtifact.h"

namespace veritas::testing {

class FixtureRootOwner;

struct SvfFixtureSnapshot {
  SvfFixtureSnapshot(const SvfFixtureSnapshot&) = delete;
  SvfFixtureSnapshot& operator=(const SvfFixtureSnapshot&) = delete;
  SvfFixtureSnapshot(SvfFixtureSnapshot&&) noexcept;
  SvfFixtureSnapshot& operator=(SvfFixtureSnapshot&&) noexcept;
  ~SvfFixtureSnapshot();

 private:
  SvfFixtureSnapshot(std::unique_ptr<FixtureRootOwner> fixture_root,
                     std::filesystem::path project_root,
                     analysis::svf::SvfMappingResult mapping,
                     std::map<std::string, std::string>
                         function_variant_ids_by_symbol);

  std::unique_ptr<FixtureRootOwner> fixture_root_;

  friend StatusOr<SvfFixtureSnapshot> MapFixtureWithSvf(
      std::string_view name, const analysis::svf::SvfConfig& config);

 public:
  std::filesystem::path project_root;
  analysis::svf::SvfMappingResult mapping;
  std::map<std::string, std::string> function_variant_ids_by_symbol;
};

struct LocalFixtureSnapshot {
  LocalFixtureSnapshot(const LocalFixtureSnapshot&) = delete;
  LocalFixtureSnapshot& operator=(const LocalFixtureSnapshot&) = delete;
  LocalFixtureSnapshot(LocalFixtureSnapshot&&) noexcept;
  LocalFixtureSnapshot& operator=(LocalFixtureSnapshot&&) noexcept;
  ~LocalFixtureSnapshot();

 private:
  LocalFixtureSnapshot(std::unique_ptr<FixtureRootOwner> fixture_root,
                       std::filesystem::path project_root,
                       std::vector<summary::v2::FunctionSummary> summaries,
                       std::map<std::string, std::string>
                           function_variant_ids_by_symbol);

  std::unique_ptr<FixtureRootOwner> fixture_root_;

  friend StatusOr<LocalFixtureSnapshot> AnalyzeLocalFixture(
      std::string_view name);

 public:
  std::filesystem::path project_root;
  std::vector<summary::v2::FunctionSummary> summaries;
  std::map<std::string, std::string> function_variant_ids_by_symbol;
};

struct AnalyzedFixtureSnapshot {
  AnalyzedFixtureSnapshot(const AnalyzedFixtureSnapshot&) = delete;
  AnalyzedFixtureSnapshot& operator=(const AnalyzedFixtureSnapshot&) = delete;
  AnalyzedFixtureSnapshot(AnalyzedFixtureSnapshot&&) noexcept;
  AnalyzedFixtureSnapshot& operator=(AnalyzedFixtureSnapshot&&) noexcept;
  ~AnalyzedFixtureSnapshot();

 private:
  AnalyzedFixtureSnapshot(std::unique_ptr<FixtureRootOwner> fixture_root,
                          std::filesystem::path project_root,
                          std::filesystem::path output_root,
                          analysis::ProjectAnalysisResult analysis,
                          std::vector<summary::SummaryArtifact> summaries,
                          std::map<std::string, std::string>
                              function_variant_ids_by_symbol);

  std::unique_ptr<FixtureRootOwner> fixture_root_;

  friend StatusOr<AnalyzedFixtureSnapshot> AnalyzeAndLoadFixture(
      std::string_view name, const analysis::AnalysisConfig& config);

 public:
  std::filesystem::path project_root;
  std::filesystem::path output_root;
  analysis::ProjectAnalysisResult analysis;
  std::vector<summary::SummaryArtifact> summaries;
  std::map<std::string, std::string> function_variant_ids_by_symbol;
};

StatusOr<SvfFixtureSnapshot> MapFixtureWithSvf(
    std::string_view name,
    const analysis::svf::SvfConfig& config =
        analysis::svf::SvfConfig::Default());

StatusOr<LocalFixtureSnapshot> AnalyzeLocalFixture(std::string_view name);

StatusOr<AnalyzedFixtureSnapshot> AnalyzeAndLoadFixture(
    std::string_view name, const analysis::AnalysisConfig& config);

bool AllArtifactsAreV2(std::span<const summary::SummaryArtifact> artifacts);
std::vector<const summary::v2::Call*> CallsWithDispatch(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::DispatchKind dispatch);
std::vector<const summary::v2::MemoryEffect*> MemoryEffectsWithObjectKind(
    std::span<const summary::SummaryArtifact> artifacts,
    summary::v2::AbstractObjectKind kind);

}  // namespace veritas::testing

#endif  // VERITAS_TESTS_SUPPORT_WPAFIXTUREHARNESS_H_
