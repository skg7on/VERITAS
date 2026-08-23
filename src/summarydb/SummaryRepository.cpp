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

#include "veritas/summarydb/SummaryRepository.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/ComponentHash.h"
#include "veritas/summary/FunctionSummary.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/summarydb/ObjectStore.h"

namespace veritas::summarydb {

namespace {

// PreparedArtifact bundles everything needed to stage one summary's metadata
// rows after its immutable bytes have been written to the object store. It is
// the single, version-neutral unit shared by the V1 and V2 publish paths.
struct PreparedArtifact {
  core::StableId id;
  std::string object_key;
  std::string schema_version;
  std::string function_variant_id;
  std::vector<summary::ComponentDigestInfo> digests;
};

// Compute the ID, object key, schema version, function identity, and component
// digests for one artifact — everything the metadata staging needs. Does not
// touch the object store; callers that need the immutable bytes written pair
// this with PrepareArtifact.
veritas::StatusOr<PreparedArtifact> PrepareArtifactMetadata(
    const summary::SummaryArtifact &artifact) {
  auto id_result = summary::ComputeFunctionSummaryId(artifact);
  if (!id_result.ok()) {
    return id_result.status();
  }

  return PreparedArtifact{
      *id_result,
      core::ToString(*id_result),
      std::string(summary::SchemaVersion(artifact)),
      std::string(summary::Identity(artifact).function_variant_id()),
      summary::ComputeComponentDigests(artifact),
  };
}

// Compute the ID, serialize, and write the immutable bytes for one artifact.
// Returns everything the metadata staging step needs. The object store write
// happens before any metadata transaction so the CAS layer never participates
// in a transaction.
veritas::StatusOr<PreparedArtifact> PrepareArtifact(
    ObjectStore &object_store, const summary::SummaryArtifact &artifact) {
  auto prepared = PrepareArtifactMetadata(artifact);
  if (!prepared.ok()) {
    return prepared.status();
  }

  auto bytes_result = summary::SerializeSummaryArtifact(artifact);
  if (!bytes_result.ok()) {
    return bytes_result.status();
  }

  auto put_status = object_store.PutIfAbsent(prepared->object_key, *bytes_result);
  if (!put_status.ok()) {
    return put_status;
  }

  return prepared;
}

// Stage the immutable-object metadata and component digests for one prepared
// artifact. Assumes the caller has already begun a transaction.
veritas::Status StageObjectMetadata(MetadataStore &metadata_store,
                                    const PreparedArtifact &entry) {
  const std::string id_str = core::ToString(entry.id);
  auto insert_object = metadata_store.Execute(
      "INSERT OR IGNORE INTO summary_objects (summary_id, object_key, "
      "schema_version, created_at) VALUES (?, ?, ?, strftime('%s', 'now'))",
      {id_str, entry.object_key, entry.schema_version});
  if (!insert_object.ok()) {
    return insert_object;
  }

  for (const auto &digest : entry.digests) {
    auto insert_component = metadata_store.Execute(
        "INSERT OR REPLACE INTO summary_components (summary_id, "
        "component_kind, semantic_hash, evidence_hash, item_count) "
        "VALUES (?, ?, ?, ?, ?)",
        {id_str, std::to_string(static_cast<int>(digest.kind)),
         core::DigestToHex(digest.semantic_hash),
         core::DigestToHex(digest.evidence_hash),
         std::to_string(digest.item_count)});
    if (!insert_component.ok()) {
      return insert_component;
    }
  }
  return veritas::Status::Ok();
}

// Stage the current binding for one prepared artifact. Assumes a transaction.
veritas::Status StageBinding(MetadataStore &metadata_store,
                             const PreparedArtifact &entry,
                             const std::string &function_variant_id,
                             const std::string &revision_id,
                             const std::string &build_variant_id) {
  return metadata_store.Execute(
      "INSERT OR REPLACE INTO summary_bindings (function_variant_id, "
      "revision_id, build_variant_id, summary_id, publication_epoch, "
      "is_current) VALUES (?, ?, ?, ?, strftime('%s', 'now'), 1)",
      {function_variant_id, revision_id, build_variant_id,
       core::ToString(entry.id)});
}

// Publish a single artifact and atomically bind it as current. The binding
// uses the caller-supplied function_variant_id (the V1 contract).
veritas::StatusOr<core::StableId> PublishSingleArtifact(
    ObjectStore &object_store, MetadataStore &metadata_store,
    const summary::SummaryArtifact &artifact,
    const PublicationContext &context) {
  auto prepared = PrepareArtifact(object_store, artifact);
  if (!prepared.ok()) {
    return prepared.status();
  }

  auto begin_result = metadata_store.BeginTransaction();
  if (!begin_result.ok()) {
    return begin_result;
  }
  bool committed = false;
  auto rollback_guard = [&metadata_store](bool *c) {
    if (!*c) {
      metadata_store.RollbackTransaction();
    }
  };
  std::unique_ptr<bool, decltype(rollback_guard)> guard(&committed,
                                                        rollback_guard);

  auto stage_object = StageObjectMetadata(metadata_store, *prepared);
  if (!stage_object.ok()) {
    return stage_object;
  }
  auto stage_binding = StageBinding(metadata_store, *prepared,
                                    context.function_variant_id,
                                    context.revision_id,
                                    context.build_variant_id);
  if (!stage_binding.ok()) {
    return stage_binding;
  }

  auto commit_result = metadata_store.CommitTransaction();
  if (!commit_result.ok()) {
    return commit_result;
  }
  committed = true;

  return prepared->id;
}

// Publish a batch of artifacts atomically: every current binding advances in
// one transaction, or none do.
veritas::StatusOr<std::vector<core::StableId>> PublishBatchArtifacts(
    ObjectStore &object_store, MetadataStore &metadata_store,
    const std::string &revision_id, const std::string &build_variant_id,
    const std::vector<summary::SummaryArtifact> &artifacts) {
  std::vector<PreparedArtifact> prepared;
  prepared.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    auto entry = PrepareArtifact(object_store, artifact);
    if (!entry.ok()) {
      return entry.status();
    }
    prepared.push_back(std::move(*entry));
  }

  auto begin_result = metadata_store.BeginTransaction();
  if (!begin_result.ok()) {
    return begin_result;
  }
  bool committed = false;
  auto rollback_guard = [&metadata_store](bool *c) {
    if (!*c) {
      metadata_store.RollbackTransaction();
    }
  };
  std::unique_ptr<bool, decltype(rollback_guard)> guard(&committed,
                                                        rollback_guard);

  std::vector<core::StableId> ids;
  ids.reserve(prepared.size());
  for (const auto &entry : prepared) {
    auto stage_object = StageObjectMetadata(metadata_store, entry);
    if (!stage_object.ok()) {
      return stage_object;
    }
    auto stage_binding = StageBinding(metadata_store, entry,
                                      entry.function_variant_id, revision_id,
                                      build_variant_id);
    if (!stage_binding.ok()) {
      return stage_binding;
    }
    ids.push_back(entry.id);
  }

  auto commit_result = metadata_store.CommitTransaction();
  if (!commit_result.ok()) {
    return commit_result;
  }
  committed = true;

  return ids;
}

// Convert a vector of concrete V1 or V2 summaries into artifacts.
template <typename T>
std::vector<summary::SummaryArtifact> ToArtifacts(const std::vector<T> &items) {
  std::vector<summary::SummaryArtifact> artifacts;
  artifacts.reserve(items.size());
  for (const auto &item : items) {
    artifacts.emplace_back(item);
  }
  return artifacts;
}

}  // namespace

SummaryRepository::SummaryRepository(
    std::unique_ptr<ObjectStore> object_store,
    std::unique_ptr<MetadataStore> metadata_store)
    : object_store_(std::move(object_store)),
      metadata_store_(std::move(metadata_store)) {}

veritas::StatusOr<std::unique_ptr<SummaryRepository>>
SummaryRepository::Open(const std::string &db_path) {
  std::error_code error;
  std::filesystem::create_directories(db_path, error);
  if (error) {
    return veritas::Status::Internal("cannot create repository directory " +
                                     db_path + ": " + error.message());
  }

  // Open object store (RocksDB)
  auto object_store_result = CreateObjectStore(db_path + "/objects");
  if (!object_store_result.ok()) {
    return object_store_result.status();
  }

  // Open metadata store (SQLite)
  auto metadata_store_result = MetadataStore::Open(db_path + "/metadata.db");
  if (!metadata_store_result.ok()) {
    return metadata_store_result.status();
  }

  auto metadata_store_ptr =
      std::make_unique<MetadataStore>(std::move(*metadata_store_result));

  // Apply schema (idempotent)
  auto schema_status = metadata_store_ptr->ApplySchema();
  if (!schema_status.ok()) {
    return schema_status;
  }

  return std::unique_ptr<SummaryRepository>(new SummaryRepository(
      std::move(*object_store_result), std::move(metadata_store_ptr)));
}

veritas::StatusOr<core::StableId>
SummaryRepository::PublishSummary(const summary::v1::FunctionSummary &summary,
                                  const PublicationContext &context) {
  return PublishSingleArtifact(*object_store_, *metadata_store_,
                               summary::SummaryArtifact{summary}, context);
}

veritas::StatusOr<core::StableId>
SummaryRepository::PublishSummary(const summary::v2::FunctionSummary &summary,
                                  const PublicationContext &context) {
  return PublishSingleArtifact(*object_store_, *metadata_store_,
                               summary::SummaryArtifact{summary}, context);
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PutImmutableSummaries(
    const std::vector<summary::v1::FunctionSummary> &summaries) {
  return PutImmutableSummaryArtifacts(ToArtifacts(summaries));
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PutImmutableSummaryArtifacts(
    const std::vector<summary::SummaryArtifact> &artifacts) {
  std::vector<core::StableId> ids;
  ids.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    auto id_result = summary::ComputeFunctionSummaryId(artifact);
    if (!id_result.ok()) {
      return id_result.status();
    }
    auto bytes_result = summary::SerializeSummaryArtifact(artifact);
    if (!bytes_result.ok()) {
      return bytes_result.status();
    }
    auto put_status =
        object_store_->PutIfAbsent(core::ToString(*id_result), *bytes_result);
    if (!put_status.ok()) {
      return put_status;
    }
    ids.push_back(*id_result);
  }
  return ids;
}

veritas::Status SummaryRepository::StageCurrentBindings(
    const std::string &revision_id, const std::string &build_variant_id,
    const std::vector<summary::v1::FunctionSummary> &summaries) {
  return StageCurrentArtifactBindings(revision_id, build_variant_id,
                                      ToArtifacts(summaries));
}

veritas::Status SummaryRepository::StageCurrentArtifactBindings(
    const std::string &revision_id, const std::string &build_variant_id,
    const std::vector<summary::SummaryArtifact> &artifacts) {
  for (const auto &artifact : artifacts) {
    // The immutable bytes were already written by PutImmutableSummaryArtifacts,
    // so only the metadata staging is needed here.
    auto prepared = PrepareArtifactMetadata(artifact);
    if (!prepared.ok()) {
      return prepared.status();
    }

    auto stage_object = StageObjectMetadata(*metadata_store_, *prepared);
    if (!stage_object.ok()) {
      return stage_object;
    }

    auto stage_binding = StageBinding(*metadata_store_, *prepared,
                                      prepared->function_variant_id,
                                      revision_id, build_variant_id);
    if (!stage_binding.ok()) {
      return stage_binding;
    }
  }
  return veritas::Status::Ok();
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PublishProjectSummaries(
    const std::string &revision_id, const std::string &build_variant_id,
    const std::vector<summary::v1::FunctionSummary> &summaries) {
  return PublishBatchArtifacts(*object_store_, *metadata_store_, revision_id,
                               build_variant_id, ToArtifacts(summaries));
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PublishProjectSummaries(
    const std::string &revision_id, const std::string &build_variant_id,
    const std::vector<summary::v2::FunctionSummary> &summaries) {
  return PublishBatchArtifacts(*object_store_, *metadata_store_, revision_id,
                               build_variant_id, ToArtifacts(summaries));
}

veritas::StatusOr<summary::SummaryArtifact>
SummaryRepository::GetCurrentSummaryArtifact(
    const std::string &function_variant_id) const {
  // Query current binding
  auto query_result = metadata_store_->Query(
      "SELECT summary_id FROM summary_bindings WHERE function_variant_id = ? "
      "AND is_current = 1 LIMIT 1",
      {function_variant_id});
  if (!query_result.ok()) {
    return query_result.status();
  }

  auto rows = *query_result;
  if (rows.empty()) {
    return veritas::Status::NotFound("No current summary for function variant");
  }

  std::string summary_id_str = rows[0][0];
  auto summary_id_result = core::ParseStableId(summary_id_str);
  if (!summary_id_result.ok()) {
    return summary_id_result.status();
  }

  return GetSummaryArtifact(*summary_id_result);
}

veritas::StatusOr<summary::v1::FunctionSummary>
SummaryRepository::GetCurrentSummary(
    const std::string &function_variant_id) const {
  auto artifact = GetCurrentSummaryArtifact(function_variant_id);
  if (!artifact.ok()) {
    return artifact.status();
  }
  auto *v1 = std::get_if<summary::v1::FunctionSummary>(&*artifact);
  if (v1 == nullptr) {
    return veritas::Status::FailedPrecondition(
        "current summary for function variant is not a summary.v1 artifact");
  }
  return std::move(*v1);
}

veritas::StatusOr<std::vector<summary::SummaryArtifact>>
SummaryRepository::ListCurrentSummaryArtifacts(
    std::string_view revision_id, std::string_view build_variant_id) const {
  auto query_result = metadata_store_->Query(
      "SELECT summary_id, function_variant_id FROM summary_bindings "
      "WHERE revision_id = ? AND build_variant_id = ? AND is_current = 1 "
      "ORDER BY function_variant_id ASC",
      {std::string(revision_id), std::string(build_variant_id)});
  if (!query_result.ok()) {
    return query_result.status();
  }

  std::vector<summary::SummaryArtifact> artifacts;
  artifacts.reserve(query_result->size());
  for (const auto &row : *query_result) {
    if (row.size() != 2u) {
      return veritas::Status::Internal(
          "current summary query returned an unexpected column count");
    }
    auto summary_id = core::ParseStableId(row[0]);
    if (!summary_id.ok()) {
      return summary_id.status();
    }
    if (summary_id->kind != core::IdKind::kFunctionSummary) {
      return veritas::Status::FailedPrecondition(
          "current summary binding has a non-summary object ID");
    }

    auto artifact = GetSummaryArtifact(*summary_id);
    if (!artifact.ok()) {
      return artifact.status();
    }
    const auto &identity = summary::Identity(*artifact);
    if (identity.revision_id() != revision_id ||
        identity.build_variant_id() != build_variant_id) {
      return veritas::Status::FailedPrecondition(
          "current summary binding does not match requested context");
    }
    if (identity.function_variant_id() != row[1]) {
      return veritas::Status::FailedPrecondition(
          "current summary binding does not match stored function identity");
    }
    artifacts.push_back(std::move(*artifact));
  }
  return artifacts;
}

veritas::StatusOr<std::vector<summary::v1::FunctionSummary>>
SummaryRepository::ListCurrentSummaries(std::string_view revision_id,
                                        std::string_view build_variant_id) const {
  auto artifacts = ListCurrentSummaryArtifacts(revision_id, build_variant_id);
  if (!artifacts.ok()) {
    return artifacts.status();
  }

  std::vector<summary::v1::FunctionSummary> summaries;
  summaries.reserve(artifacts->size());
  for (auto &artifact : *artifacts) {
    auto *v1 = std::get_if<summary::v1::FunctionSummary>(&artifact);
    if (v1 == nullptr) {
      return veritas::Status::FailedPrecondition(
          "current summary binding is not a summary.v1 artifact");
    }
    summaries.push_back(std::move(*v1));
  }
  return summaries;
}

veritas::Status SummaryRepository::PersistManifestContext(
    const build::AnalysisManifest &manifest) {
  return metadata_store_->PutManifestContext(manifest);
}

veritas::StatusOr<summary::SummaryArtifact>
SummaryRepository::GetSummaryArtifact(const core::StableId &summary_id) const {
  const std::string id_str = core::ToString(summary_id);

  // Read the CAS bytes first: the schema version is either recorded in the
  // metadata row or, for objects written by PutImmutableSummaryArtifacts before
  // staging, carried in the serialized header itself.
  auto get_result = object_store_->Get(id_str);
  if (!get_result.ok()) {
    return get_result.status();
  }

  auto version_query = metadata_store_->Query(
      "SELECT schema_version FROM summary_objects WHERE summary_id = ?",
      {id_str});
  if (!version_query.ok()) {
    return version_query.status();
  }

  std::string schema_version;
  if (version_query->empty()) {
    // No metadata row: probe the serialized header. Both v1 and v2 share the
    // field-1 SummaryHeader with a field-1 schema_version, so parsing the bytes
    // as v1 recovers the true version for either schema. Do NOT assume V1.
    summary::v1::FunctionSummary probe;
    if (!probe.ParseFromArray(get_result->data(),
                              static_cast<int>(get_result->size()))) {
      return veritas::Status::FailedPrecondition(
          "cannot determine schema version for " + id_str +
          ": bytes do not parse as a summary header");
    }
    schema_version = probe.header().schema_version();
  } else {
    schema_version = (*version_query)[0][0];
  }

  return summary::ParseSummaryArtifact(schema_version, *get_result);
}

veritas::StatusOr<summary::v1::FunctionSummary>
SummaryRepository::GetSummary(const core::StableId &summary_id) const {
  auto artifact = GetSummaryArtifact(summary_id);
  if (!artifact.ok()) {
    return artifact.status();
  }
  auto *v1 = std::get_if<summary::v1::FunctionSummary>(&*artifact);
  if (v1 == nullptr) {
    return veritas::Status::FailedPrecondition(
        "summary object is not a summary.v1 artifact");
  }
  return std::move(*v1);
}

} // namespace veritas::summarydb
