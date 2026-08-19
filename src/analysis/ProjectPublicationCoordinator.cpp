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

#include "analysis/ProjectPublicationCoordinator.h"

#include <algorithm>
#include <utility>

#include "veritas/cpg/CpgRepository.h"
#include "veritas/summary/FunctionSummary.h"
#include "veritas/summarydb/SummaryRepository.h"

namespace veritas::analysis {
namespace {

// Verify the graph's sorted summary-ID set exactly matches the completed
// summaries. A mismatch is a fatal precondition: no binding may advance.
Status ValidateSnapshotCorrespondence(const CompletedProjectAnalysis& completed) {
  std::vector<core::StableId> actual;
  actual.reserve(completed.summaries.size());
  for (const auto& summary : completed.summaries) {
    auto id = summary::ComputeFunctionSummaryId(summary);
    if (!id.ok()) return id.status();
    actual.push_back(*id);
  }
  std::sort(actual.begin(), actual.end());

  auto expected = completed.graph.metadata().summary_ids;
  std::sort(expected.begin(), expected.end());

  if (actual != expected) {
    return Status::FailedPrecondition(
        "graph summary IDs do not match completed summaries");
  }
  return Status::Ok();
}

}  // namespace

ProjectPublicationCoordinator::ProjectPublicationCoordinator(
    std::unique_ptr<summarydb::SummaryRepository> summaries)
    : summaries_(std::move(summaries)),
      cpg_(std::make_unique<::veritas::cpg::CpgRepository>(
          summaries_->metadata_store())) {}

ProjectPublicationCoordinator::~ProjectPublicationCoordinator() = default;

StatusOr<std::unique_ptr<ProjectPublicationCoordinator>>
ProjectPublicationCoordinator::Open(const std::string& db_path) {
  auto summaries = summarydb::SummaryRepository::Open(db_path);
  if (!summaries.ok()) return summaries.status();
  return std::unique_ptr<ProjectPublicationCoordinator>(
      new ProjectPublicationCoordinator(std::move(*summaries)));
}

Status ProjectPublicationCoordinator::PersistManifestContext(
    const build::AnalysisManifest& manifest) {
  return summaries_->PersistManifestContext(manifest);
}

StatusOr<std::vector<core::StableId>> ProjectPublicationCoordinator::Publish(
    CompletedProjectAnalysis completed) {
  auto validate = completed.graph.Validate();
  if (!validate.ok()) return validate;

  auto correspondence = ValidateSnapshotCorrespondence(completed);
  if (!correspondence.ok()) return correspondence;

  // Immutable objects first (outside the transaction).
  auto put = summaries_->PutImmutableSummaries(completed.summaries);
  if (!put.ok()) return put.status();

  auto& metadata = summaries_->metadata_store();
  auto begin = metadata.BeginTransaction();
  if (!begin.ok()) return begin;
  bool committed = false;
  auto rollback_guard = [&metadata](bool* c) {
    if (!*c) metadata.RollbackTransaction();
  };
  std::unique_ptr<bool, decltype(rollback_guard)> guard(&committed,
                                                        rollback_guard);

  const auto& meta = completed.graph.metadata();
  auto stage_summaries = summaries_->StageCurrentBindings(
      core::ToString(meta.revision_id), core::ToString(meta.build_variant_id),
      completed.summaries);
  if (!stage_summaries.ok()) return stage_summaries;

  auto stage_cpg = cpg_->StageProjection(completed.graph);
  if (!stage_cpg.ok()) return stage_cpg;

  auto commit = metadata.CommitTransaction();
  if (!commit.ok()) return commit;
  committed = true;
  return *put;
}

}  // namespace veritas::analysis
