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

// WpaRunRepository.h — versioned WPA run state and the content-addressed cache.
//
// Owns the metadata connection and a dedicated immutable object store at
// <db_path>/wpa-component-results. It persists run manifests/status, per-SCC
// component input/fixpoint/external hashes, diagnostics, and stale linkage; it
// does not persist M9 facts. Each canonical component result is stored as an
// opaque immutable cache object so a reused successor still supplies its facts
// and witnesses.

#ifndef VERITAS_WPA_WPA_RUN_REPOSITORY_H_
#define VERITAS_WPA_WPA_RUN_REPOSITORY_H_

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/summarydb/ObjectStore.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {

enum class WpaRunStatus : std::uint8_t {
  kInProgress,
  kComplete,
  kIncomplete,
};

enum class WpaComponentStatus : std::uint8_t {
  kSucceeded,
  kFailed,
};

// One component of one SCC. The component is repeated inside the key (rather
// than carried only in the result) so state and cache rows are keyed by it.
struct WpaComponentKey {
  core::StableId scc_id;
  WpaComponentKind component = WpaComponentKind::kReachability;

  auto operator<=>(const WpaComponentKey&) const = default;
  bool operator==(const WpaComponentKey&) const = default;
};

// The completion of one component: its key, the immutable object-store key of
// the stored result, and the result itself.
struct WpaComponentCompletion {
  WpaComponentKey key;
  std::string result_object_key;
  WpaComponentResult result;
};

// Derives the content-addressed cache key for a component: everything that
// identifies the exact result independent of revision and run identity. The
// logical input hash is known before execution, so the key can be looked up
// ahead of a run.
std::string DeriveResultCacheKey(const facts::AnalysisRunManifest& run,
                                 const WpaComponentKey& key,
                                 std::string_view logical_input_hash);

class WpaRunRepository {
 public:
  // Opens the metadata database and the component-result object store.
  static StatusOr<WpaRunRepository> Open(const std::filesystem::path& db_path);

  WpaRunRepository(WpaRunRepository&&) noexcept;
  WpaRunRepository& operator=(WpaRunRepository&&) noexcept;

  // Records a run as in progress. Idempotent for the same run_id.
  Status BeginRun(const facts::AnalysisRunManifest& run);

  // Loads a reusable result from the cache, validating every identity field and
  // the stored object. Returns nullopt when nothing matches.
  StatusOr<std::optional<WpaComponentResult>> LoadReusableComponent(
      const std::string& result_cache_key);

  // Stores a successful component result: the immutable object, the cache row,
  // and the run's component state, in one transaction.
  StatusOr<WpaComponentCompletion> StoreSuccessfulComponent(
      const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
      const WpaComponentResult& result);

  // Records a failed component with diagnostics; publishes no result.
  Status RecordComponentFailure(const facts::AnalysisRunManifest& run,
                                const WpaComponentKey& key,
                                std::string diagnostics);

  Status CompleteRun(const facts::AnalysisRunManifest& run);

  Status MarkIncomplete(const facts::AnalysisRunManifest& run);

  // The shared metadata connection, for the incremental scheduler's own
  // repositories to key off the same database.
  summarydb::MetadataStore& metadata_store() { return metadata_store_; }

  // Test/query accessors.
  StatusOr<WpaRunStatus> RunStatus(core::StableId run_id);
  StatusOr<std::optional<std::string>> ResultObjectKey(
      core::StableId run_id, const WpaComponentKey& key);

 private:
  WpaRunRepository(summarydb::MetadataStore store,
                   std::unique_ptr<summarydb::ObjectStore> results);

  summarydb::MetadataStore metadata_store_;
  std::unique_ptr<summarydb::ObjectStore> component_results_;

  WpaRunRepository(const WpaRunRepository&) = delete;
  WpaRunRepository& operator=(const WpaRunRepository&) = delete;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_RUN_REPOSITORY_H_
