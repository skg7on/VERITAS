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
#include <span>
#include <string>
#include <system_error>

#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/summarydb/ObjectStore.h"
#include "veritas/summary/ComponentHash.h"
#include "veritas/summary/FunctionSummary.h"

namespace veritas::summarydb {

SummaryRepository::SummaryRepository(
    std::unique_ptr<ObjectStore> object_store,
    std::unique_ptr<MetadataStore> metadata_store)
    : object_store_(std::move(object_store)),
      metadata_store_(std::move(metadata_store)) {}

veritas::StatusOr<std::unique_ptr<SummaryRepository>> SummaryRepository::Open(
    const std::string& db_path) {
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

veritas::StatusOr<core::StableId> SummaryRepository::PublishSummary(
    const summary::v1::FunctionSummary& summary,
    const PublicationContext& context) {
  // Compute summary ID
  auto summary_id_result = summary::ComputeFunctionSummaryId(summary);
  if (!summary_id_result.ok()) {
    return summary_id_result.status();
  }
  auto summary_id = *summary_id_result;

  // Serialize summary for CAS
  std::string serialized;
  if (!summary.SerializeToString(&serialized)) {
    return veritas::Status::Internal("Failed to serialize summary");
  }

  auto bytes_span = std::as_bytes(std::span(serialized));
  std::vector<std::byte> bytes_vec(bytes_span.begin(), bytes_span.end());

  // Store in object store (idempotent)
  std::string object_key = core::ToString(summary_id);
  auto put_status = object_store_->PutIfAbsent(object_key, bytes_vec);
  if (!put_status.ok()) {
    return put_status;
  }

  // Compute component digests
  auto component_digests = summary::ComputeComponentDigests(summary);

  // Begin metadata transaction
  auto begin_result = metadata_store_->BeginTransaction();
  if (!begin_result.ok()) {
    return begin_result;
  }

  // RAII guard to ensure rollback on early return or exception
  auto rollback_guard = [this](bool* committed) {
    if (!*committed) {
      metadata_store_->RollbackTransaction();
    }
  };
  bool committed = false;
  std::unique_ptr<bool, decltype(rollback_guard)> guard(&committed,
                                                         rollback_guard);

  // Insert summary object metadata
  auto insert_object_result = metadata_store_->Execute(
      "INSERT OR IGNORE INTO summary_objects (summary_id, object_key, "
      "schema_version, created_at) VALUES (?, ?, ?, strftime('%s', 'now'))",
      {core::ToString(summary_id), object_key,
       summary.header().schema_version()});
  if (!insert_object_result.ok()) {
    return insert_object_result;
  }

  // Insert component digests
  for (const auto& digest : component_digests) {
    auto insert_component_result = metadata_store_->Execute(
        "INSERT OR REPLACE INTO summary_components (summary_id, "
        "component_kind, semantic_hash, evidence_hash, item_count) "
        "VALUES (?, ?, ?, ?, ?)",
        {core::ToString(summary_id), std::to_string(static_cast<int>(digest.kind)),
         core::DigestToHex(digest.semantic_hash),
         core::DigestToHex(digest.evidence_hash),
         std::to_string(digest.item_count)});
    if (!insert_component_result.ok()) {
      return insert_component_result;
    }
  }

  // Update current binding
  auto update_binding_result = metadata_store_->Execute(
      "INSERT OR REPLACE INTO summary_bindings (function_variant_id, "
      "revision_id, build_variant_id, summary_id, publication_epoch, "
      "is_current) VALUES (?, ?, ?, ?, strftime('%s', 'now'), 1)",
      {context.function_variant_id, context.revision_id,
       context.build_variant_id, core::ToString(summary_id)});
  if (!update_binding_result.ok()) {
    return update_binding_result;
  }

  // Commit transaction
  auto commit_result = metadata_store_->CommitTransaction();
  if (!commit_result.ok()) {
    // CommitTransaction already clears in_transaction_ flag on failure
    // (SQLite auto-rolls-back). No manual rollback needed - RAII guard
    // will handle cleanup if still needed.
    return commit_result;
  }

  // Mark as committed to prevent RAII rollback
  committed = true;

  return summary_id;
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PutImmutableSummaries(
    const std::vector<summary::v1::FunctionSummary>& summaries) {
  std::vector<core::StableId> ids;
  ids.reserve(summaries.size());
  for (const auto& summary : summaries) {
    auto id_result = summary::ComputeFunctionSummaryId(summary);
    if (!id_result.ok()) return id_result.status();
    std::string serialized;
    if (!summary.SerializeToString(&serialized)) {
      return veritas::Status::Internal("Failed to serialize summary");
    }
    auto bytes_span = std::as_bytes(std::span(serialized));
    std::vector<std::byte> bytes_vec(bytes_span.begin(), bytes_span.end());
    auto put_status =
        object_store_->PutIfAbsent(core::ToString(*id_result), bytes_vec);
    if (!put_status.ok()) return put_status;
    ids.push_back(*id_result);
  }
  return ids;
}

veritas::Status SummaryRepository::StageCurrentBindings(
    const std::string& revision_id,
    const std::string& build_variant_id,
    const std::vector<summary::v1::FunctionSummary>& summaries) {
  for (const auto& summary : summaries) {
    auto id_result = summary::ComputeFunctionSummaryId(summary);
    if (!id_result.ok()) return id_result.status();
    const std::string id_str = core::ToString(*id_result);

    auto insert_object = metadata_store_->Execute(
        "INSERT OR IGNORE INTO summary_objects (summary_id, object_key, "
        "schema_version, created_at) VALUES (?, ?, ?, strftime('%s', 'now'))",
        {id_str, id_str, summary.header().schema_version()});
    if (!insert_object.ok()) return insert_object;

    for (const auto& digest : summary::ComputeComponentDigests(summary)) {
      auto insert_component = metadata_store_->Execute(
          "INSERT OR REPLACE INTO summary_components (summary_id, "
          "component_kind, semantic_hash, evidence_hash, item_count) "
          "VALUES (?, ?, ?, ?, ?)",
          {id_str, std::to_string(static_cast<int>(digest.kind)),
           core::DigestToHex(digest.semantic_hash),
           core::DigestToHex(digest.evidence_hash),
           std::to_string(digest.item_count)});
      if (!insert_component.ok()) return insert_component;
    }

    auto update_binding = metadata_store_->Execute(
        "INSERT OR REPLACE INTO summary_bindings (function_variant_id, "
        "revision_id, build_variant_id, summary_id, publication_epoch, "
        "is_current) VALUES (?, ?, ?, ?, strftime('%s', 'now'), 1)",
        {summary.identity().function_variant_id(), revision_id, build_variant_id,
         id_str});
    if (!update_binding.ok()) return update_binding;
  }
  return veritas::Status::Ok();
}

veritas::StatusOr<std::vector<core::StableId>>
SummaryRepository::PublishProjectSummaries(
    const std::string& revision_id,
    const std::string& build_variant_id,
    const std::vector<summary::v1::FunctionSummary>& summaries) {
  struct Prepared {
    core::StableId id;
    std::string object_key;
    std::string schema_version;
    std::string function_variant_id;
    std::vector<summary::ComponentDigestInfo> digests;
  };

  // Write immutable objects and compute IDs/digests before opening the
  // transaction so the CAS layer never participates in a metadata transaction.
  std::vector<Prepared> prepared;
  prepared.reserve(summaries.size());
  for (const auto& summary : summaries) {
    auto id_result = summary::ComputeFunctionSummaryId(summary);
    if (!id_result.ok()) {
      return id_result.status();
    }

    std::string serialized;
    if (!summary.SerializeToString(&serialized)) {
      return veritas::Status::Internal("Failed to serialize summary");
    }
    auto bytes_span = std::as_bytes(std::span(serialized));
    std::vector<std::byte> bytes_vec(bytes_span.begin(), bytes_span.end());

    std::string object_key = core::ToString(*id_result);
    auto put_status = object_store_->PutIfAbsent(object_key, bytes_vec);
    if (!put_status.ok()) {
      return put_status;
    }

    prepared.push_back(Prepared{
        *id_result,
        std::move(object_key),
        summary.header().schema_version(),
        summary.identity().function_variant_id(),
        summary::ComputeComponentDigests(summary),
    });
  }

  auto begin_result = metadata_store_->BeginTransaction();
  if (!begin_result.ok()) {
    return begin_result;
  }
  bool committed = false;
  auto rollback_guard = [this](bool* c) {
    if (!*c) metadata_store_->RollbackTransaction();
  };
  std::unique_ptr<bool, decltype(rollback_guard)> guard(&committed,
                                                        rollback_guard);

  std::vector<core::StableId> ids;
  ids.reserve(prepared.size());
  for (const auto& entry : prepared) {
    const std::string id_str = core::ToString(entry.id);

    auto insert_object = metadata_store_->Execute(
        "INSERT OR IGNORE INTO summary_objects (summary_id, object_key, "
        "schema_version, created_at) VALUES (?, ?, ?, strftime('%s', 'now'))",
        {id_str, entry.object_key, entry.schema_version});
    if (!insert_object.ok()) {
      return insert_object;
    }

    for (const auto& digest : entry.digests) {
      auto insert_component = metadata_store_->Execute(
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

    auto update_binding = metadata_store_->Execute(
        "INSERT OR REPLACE INTO summary_bindings (function_variant_id, "
        "revision_id, build_variant_id, summary_id, publication_epoch, "
        "is_current) VALUES (?, ?, ?, ?, strftime('%s', 'now'), 1)",
        {entry.function_variant_id, revision_id, build_variant_id, id_str});
    if (!update_binding.ok()) {
      return update_binding;
    }

    ids.push_back(entry.id);
  }

  auto commit_result = metadata_store_->CommitTransaction();
  if (!commit_result.ok()) {
    return commit_result;
  }
  committed = true;

  return ids;
}

veritas::StatusOr<summary::v1::FunctionSummary>
SummaryRepository::GetCurrentSummary(
    const std::string& function_variant_id) const {
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
    return veritas::Status::NotFound(
        "No current summary for function variant");
  }

  std::string summary_id_str = rows[0][0];
  auto summary_id_result = core::ParseStableId(summary_id_str);
  if (!summary_id_result.ok()) {
    return summary_id_result.status();
  }

  return GetSummary(*summary_id_result);
}

veritas::StatusOr<std::vector<summary::v1::FunctionSummary>>
SummaryRepository::ListCurrentSummaries(
    std::string_view revision_id, std::string_view build_variant_id) const {
  auto query_result = metadata_store_->Query(
      "SELECT summary_id FROM summary_bindings "
      "WHERE revision_id = ? AND build_variant_id = ? AND is_current = 1 "
      "ORDER BY function_variant_id ASC",
      {std::string(revision_id), std::string(build_variant_id)});
  if (!query_result.ok()) return query_result.status();

  std::vector<summary::v1::FunctionSummary> summaries;
  summaries.reserve(query_result->size());
  for (const auto& row : *query_result) {
    if (row.size() != 1u) {
      return veritas::Status::Internal(
          "current summary query returned an unexpected column count");
    }
    auto summary_id = core::ParseStableId(row[0]);
    if (!summary_id.ok()) return summary_id.status();
    if (summary_id->kind != core::IdKind::kFunctionSummary) {
      return veritas::Status::FailedPrecondition(
          "current summary binding has a non-summary object ID");
    }

    auto summary = GetSummary(*summary_id);
    if (!summary.ok()) return summary.status();
    if (summary->identity().revision_id() != revision_id ||
        summary->identity().build_variant_id() != build_variant_id) {
      return veritas::Status::FailedPrecondition(
          "current summary binding does not match requested context");
    }
    summaries.push_back(std::move(*summary));
  }
  return summaries;
}

veritas::Status SummaryRepository::PersistManifestContext(
    const build::AnalysisManifest& manifest) {
  return metadata_store_->PutManifestContext(manifest);
}

veritas::StatusOr<summary::v1::FunctionSummary> SummaryRepository::GetSummary(
    const core::StableId& summary_id) const {
  std::string object_key = core::ToString(summary_id);

  // Retrieve from object store
  auto get_result = object_store_->Get(object_key);
  if (!get_result.ok()) {
    return get_result.status();
  }

  // Deserialize
  summary::v1::FunctionSummary summary;
  auto bytes = *get_result;
  if (!summary.ParseFromArray(bytes.data(), bytes.size())) {
    return veritas::Status::Internal("Failed to parse summary");
  }

  return summary;
}

}  // namespace veritas::summarydb
