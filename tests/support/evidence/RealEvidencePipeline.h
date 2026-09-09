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

// RealEvidencePipeline.h — runs the real M1→M6→M9→M10A pipeline on a fixture
// and binds the durable read surface (CPG projection + FactStore) so tests can
// exercise EvidenceQueryService against genuinely produced facts.
//
// Header-only so integration tests share it without a new library target.

#ifndef VERITAS_TESTING_REAL_EVIDENCE_PIPELINE_H_
#define VERITAS_TESTING_REAL_EVIDENCE_PIPELINE_H_

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/evidence/EvidenceReadBackend.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/facts/FactStore.h"

namespace veritas::testing {

// Everything a real-fixture evidence test needs: the durable FactStore (which
// owns the shared metadata store), the loaded CPG projection, the pinned
// snapshot descriptor, and the analysis-run id.
struct RealEvidenceSnapshot {
  std::filesystem::path project_root;
  std::filesystem::path output_root;
  analysis::ProjectAnalysisResult analysis;
  cpg::ThinCpg cpg;
  facts::FactStore fact_store;
  evidence::SnapshotDescriptor descriptor;
  core::StableId run_id;
};

namespace {

constexpr std::string_view kAnalysisConfig = "veritas-analysis-config.v1";

// The canonical repository id is not carried on the CPG ProjectionMetadata, so
// it is read back from the manifest context. It is content-derived (a hash of
// the source tree), never the checkout path.
StatusOr<std::string> RepositoryId(const std::filesystem::path& project_root) {
  const analysis::ProjectAnalysisRequest request{
      .project_root = project_root,
      .output_root = {},
  };
  auto input = build::ResolveProjectInput(request);
  if (!input.ok()) {
    return input.status();
  }
  auto manifest = build::LoadProjectManifest(*input);
  if (!manifest.ok()) {
    return manifest.status();
  }
  return manifest->context.repository_id;
}

}  // namespace

// Materializes the named fixture, runs the full analysis, opens the FactStore
// at the output root, loads the CPG projection, and derives a snapshot
// descriptor whose repository/revision/build-variant strings agree with the
// CPG ProjectionMetadata (revision/build-variant) and the manifest context
// (repository). fact_snapshot_fingerprint is the canonical digest over the
// run's current fact IDs.
inline StatusOr<RealEvidenceSnapshot> AnalyzeRealFixture(std::string_view name) {
  const auto project_root = FixtureProject(name);
  const auto output_root = project_root / ".veritas";

  const auto repository = RepositoryId(project_root);
  if (!repository.ok()) {
    return repository.status();
  }

  analysis::ProjectAnalyzer analyzer;
  auto analysis = analyzer.AnalyzeProject(
      analysis::ProjectAnalysisRequest{.project_root = project_root,
                                       .output_root = output_root},
      analysis::AnalysisConfig::Default());
  if (!analysis.ok()) {
    return analysis.status();
  }

  auto fact_store = facts::FactStore::Open(output_root);
  if (!fact_store.ok()) {
    return fact_store.status();
  }

  auto projection = core::ParseStableId(analysis->projection_id);
  if (!projection.ok()) {
    return projection.status();
  }
  cpg::CpgRepository repository_impl(fact_store->metadata_store());
  auto cpg = repository_impl.LoadProjection(*projection);
  if (!cpg.ok()) {
    return cpg.status();
  }

  auto run_id = core::ParseStableId(analysis->wpa_run_id);
  if (!run_id.ok()) {
    return run_id.status();
  }

  auto current_facts = fact_store->GetCurrentFacts(*run_id);
  if (!current_facts.ok()) {
    return current_facts.status();
  }

  evidence::SnapshotDescriptor descriptor;
  descriptor.repository = *repository;
  descriptor.revision = core::ToString(cpg->metadata().revision_id);
  descriptor.build_variant = core::ToString(cpg->metadata().build_variant_id);
  descriptor.analysis_config = std::string(kAnalysisConfig);
  descriptor.analysis_run_id = *run_id;
  descriptor.fact_snapshot_fingerprint =
      evidence::ReturnedMemberDigest(*current_facts);

  RealEvidenceSnapshot snapshot{
      .project_root = project_root,
      .output_root = output_root,
      .analysis = std::move(*analysis),
      .cpg = std::move(*cpg),
      .fact_store = std::move(*fact_store),
      .descriptor = std::move(descriptor),
      .run_id = *run_id,
  };
  return snapshot;
}

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_REAL_EVIDENCE_PIPELINE_H_
