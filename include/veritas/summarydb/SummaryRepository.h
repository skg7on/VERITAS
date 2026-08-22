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

#ifndef VERITAS_SUMMARYDB_SUMMARY_REPOSITORY_H_
#define VERITAS_SUMMARYDB_SUMMARY_REPOSITORY_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/summarydb/ObjectStore.h"

namespace veritas::summarydb {

// PublicationContext provides the metadata for publishing a summary.
struct PublicationContext {
  std::string revision_id;
  std::string build_variant_id;
  std::string function_variant_id;
};

// SummaryRepository manages the publication and retrieval of function
// summaries. Summaries are stored immutably in the ObjectStore, with metadata
// bindings managed transactionally in the MetadataStore.
class SummaryRepository {
public:
  static veritas::StatusOr<std::unique_ptr<SummaryRepository>>
  Open(const std::string &db_path);

  // Publish a summary and atomically bind it as current.
  // Returns the FunctionSummaryID on success.
  veritas::StatusOr<core::StableId>
  PublishSummary(const summary::v1::FunctionSummary &summary,
                 const PublicationContext &context);

  // V2 overload: publish a summary.v2 object through the same version-aware
  // storage path, binding it as current.
  veritas::StatusOr<core::StableId>
  PublishSummary(const summary::v2::FunctionSummary &summary,
                 const PublicationContext &context);

  // Publish a batch of summaries atomically: every current binding advances in
  // one SQLite transaction, or none do. Immutable objects are written first
  // (outside the transaction); the metadata inserts and binding updates are
  // staged together. Returns the FunctionSummaryIDs in input order.
  veritas::StatusOr<std::vector<core::StableId>> PublishProjectSummaries(
      const std::string &revision_id, const std::string &build_variant_id,
      const std::vector<summary::v1::FunctionSummary> &summaries);

  // V2 overload of PublishProjectSummaries.
  veritas::StatusOr<std::vector<core::StableId>> PublishProjectSummaries(
      const std::string &revision_id, const std::string &build_variant_id,
      const std::vector<summary::v2::FunctionSummary> &summaries);

  // Retrieve the current summary for a function variant.
  veritas::StatusOr<summary::v1::FunctionSummary>
  GetCurrentSummary(const std::string &function_variant_id) const;

  // Retrieve every current summary in one revision/build context, ordered by
  // stable function-variant ID.
  veritas::StatusOr<std::vector<summary::v1::FunctionSummary>>
  ListCurrentSummaries(std::string_view revision_id,
                       std::string_view build_variant_id) const;

  // Retrieve a specific summary by ID.
  veritas::StatusOr<summary::v1::FunctionSummary>
  GetSummary(const core::StableId &summary_id) const;

  // Version-neutral retrieval. GetSummaryArtifact reads the stored
  // schema_version and parses the CAS bytes with the matching protobuf, so a
  // summary.v2 object round-trips without being reinterpreted as V1.
  veritas::StatusOr<summary::SummaryArtifact>
  GetSummaryArtifact(const core::StableId &summary_id) const;

  // Version-neutral current-binding retrieval for a function variant.
  veritas::StatusOr<summary::SummaryArtifact>
  GetCurrentSummaryArtifact(const std::string &function_variant_id) const;

  // Version-neutral listing of current summaries in one revision/build
  // context, ordered by stable function-variant ID.
  veritas::StatusOr<std::vector<summary::SummaryArtifact>>
  ListCurrentSummaryArtifacts(std::string_view revision_id,
                              std::string_view build_variant_id) const;

  // Persist the M1 program context (repository, revision, build variant, and
  // translation units). Required before PublishProjectSummaries so the summary
  // bindings' revision/build-variant foreign keys resolve. Idempotent.
  veritas::Status
  PersistManifestContext(const build::AnalysisManifest &manifest);

  // Expose the backing MetadataStore so the publication coordinator can stage
  // summaries and the CPG projection in one transaction.
  MetadataStore &metadata_store() { return *metadata_store_; }

  // Write immutable summary objects (no transaction) and return their
  // FunctionSummaryIDs in input order.
  veritas::StatusOr<std::vector<core::StableId>> PutImmutableSummaries(
      const std::vector<summary::v1::FunctionSummary> &summaries);

  // Version-neutral PutImmutableSummaries: accepts a mix of V1 and V2 objects.
  veritas::StatusOr<std::vector<core::StableId>> PutImmutableSummaryArtifacts(
      const std::vector<summary::SummaryArtifact> &artifacts);

  // Stage summary metadata and current bindings within the current transaction.
  // Assumes BeginTransaction has already been called on metadata_store().
  veritas::Status StageCurrentBindings(
      const std::string &revision_id, const std::string &build_variant_id,
      const std::vector<summary::v1::FunctionSummary> &summaries);

  // Version-neutral StageCurrentBindings: stages a mix of V1 and V2 objects.
  veritas::Status StageCurrentArtifactBindings(
      const std::string &revision_id, const std::string &build_variant_id,
      const std::vector<summary::SummaryArtifact> &artifacts);

private:
  SummaryRepository(std::unique_ptr<ObjectStore> object_store,
                    std::unique_ptr<MetadataStore> metadata_store);

  std::unique_ptr<ObjectStore> object_store_;
  std::unique_ptr<MetadataStore> metadata_store_;
};

} // namespace veritas::summarydb

#endif // VERITAS_SUMMARYDB_SUMMARY_REPOSITORY_H_
