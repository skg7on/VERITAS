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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// Versioned, engine-scoped identity for one cacheable component result (design
// §6). The descriptor is length-prefix encoded and hashed; the resulting digest
// is the cache and object key. This intentionally invalidates the former
// delimiter-concatenated key format without rewriting historical run records.
struct ResultCacheDescriptor {
  facts::EngineIdentity engine;
  std::string engine_toolchain_identity;
  std::string logical_input_hash;
  core::StableId scc_id;
  WpaComponentKind component;
  std::string summary_schema_version;
  std::string relation_schema_version;
  std::string rule_bundle_version;
  std::string model_bundle_version;
  std::string svf_configuration_hash;
  std::string wpa_configuration_hash;

  // Canonical length-prefixed encoding; injective across field contents.
  std::string Encode() const;
  // The content-addressed cache key: SHA-256 of Encode(), lowercase hex.
  std::string Key() const;
};

// Builds the descriptor for a component from its run manifest and key. The
// logical input hash is known before execution, so the descriptor (and its key)
// can be formed ahead of a run.
ResultCacheDescriptor MakeResultCacheDescriptor(
    const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
    std::string_view logical_input_hash);

class WpaRunRepository {
 public:
  // Opens the metadata database and the component-result object store.
  static StatusOr<WpaRunRepository> Open(const std::filesystem::path& db_path);

  WpaRunRepository(WpaRunRepository&&) noexcept;
  WpaRunRepository& operator=(WpaRunRepository&&) noexcept;

  // Records a run as in progress. Idempotent for the same run_id.
  Status BeginRun(const facts::AnalysisRunManifest& run);

  // Loads a reusable result from the cache, revalidating the metadata row, the
  // stored object, the deserialized SCC/component/logical-input identity, every
  // fact identity, and the recomputed fixpoint/external hashes against the
  // expected descriptor. Any mismatch is a hard cache-integrity error. Returns
  // nullopt when no entry matches.
  StatusOr<std::optional<WpaComponentResult>> LoadReusableComponent(
      const ResultCacheDescriptor& descriptor);

  // Reloads one component of a run that has already stored it, for a caller that
  // holds the component's key but not its payload -- assembly, after the
  // orchestrator has released the in-memory result. The logical input hash that
  // keys the cache entry is read from the component's own run-state row, so the
  // descriptor is rebuilt here and the load goes through
  // `LoadReusableComponent`: the same object, the same deserialization, and the
  // same revalidation the reuse path applies.
  //
  // Fails with `FailedPrecondition` when the run has no succeeded state row for
  // the key, when that row's cache entry is gone, or with whatever
  // `LoadReusableComponent` returns when the stored object fails to revalidate.
  // Callers must have committed the run's component cache (a completed run has,
  // through `CompleteRun`) before reloading.
  StatusOr<WpaComponentResult> ReloadStoredComponent(
      const facts::AnalysisRunManifest& run, const WpaComponentKey& key);

  // Stores a successful component result: the immutable object, and the cache
  // and run-state rows that reference it. The object write is immediate and
  // unchanged; the two rows are queued and committed with the rest of the
  // batch, because a commit is a durability barrier and a run stores one
  // component per (SCC, kind) pair. The returned completion owns the payload; a
  // caller that has no further use for its own copy should pass it by move
  // rather than paying for a second copy of every fact and witness in the run.
  StatusOr<WpaComponentCompletion> StoreSuccessfulComponent(
      const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
      WpaComponentResult result);

  // Commits every queued cache and run-state row in one transaction.
  // `StoreSuccessfulComponent` calls this once a batch is full, and
  // `CompleteRun` calls it before it returns, so every component a completed
  // run stored is loadable as soon as `Run` returns. A no-op when nothing is
  // queued.
  //
  // Durability, stated plainly: rows queued since the last commit are lost if
  // the process dies before this returns. Every one of them is a cache row or
  // the state row of a cache row, so the loss costs recomputation on the next
  // run and nothing else — no published fact, no provenance edge, and not the
  // run receipt are affected, and a run that completes publishes exactly what
  // it published before.
  //
  // A failed flush abandons its whole batch — the transaction is rolled back
  // where one was opened — and reports the failure to the caller. That leaves
  // the batch neither half-written nor queued for a later flush, and the
  // caller fails the run as it does for any other store error. From
  // `CompleteRun` the caller is the orchestrator, which marks the run
  // incomplete on this error exactly as it does on the per-batch flush
  // failure; without that the run row would stay `kInProgress`, because
  // `BeginRun` inserts it with `INSERT OR IGNORE` and a later run of the same
  // id does not reset it.
  Status FlushComponentCache();

  // How many components one commit covers: the number of components whose
  // cache rows a crash can cost. The batch is also flushed whenever it reaches
  // this size, so the window never grows past it.
  static constexpr std::size_t kComponentCacheBatchSize = 256;

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

  // Queues one stored component's cache row and run-state row for the next
  // commit. Neither can be rejected: both rows are built here, with the column
  // count its statement declares.
  void QueueComponentRows(const facts::AnalysisRunManifest& run,
                          const WpaComponentKey& key,
                          const std::string& cache_key,
                          const WpaComponentResult& result);

  summarydb::MetadataStore metadata_store_;
  std::unique_ptr<summarydb::ObjectStore> component_results_;

  // Bound parameter values of the rows queued since the last commit — one row
  // per stored component in each. They are rows rather than a live
  // `BulkInsertBatcher` because a batcher binds a `MetadataStore&` and would
  // have to be rebound by every move of this repository; a batcher built at
  // flush time always binds the store this repository currently owns.
  std::vector<std::vector<std::string>> pending_cache_rows_;
  std::vector<std::vector<std::string>> pending_state_rows_;

  WpaRunRepository(const WpaRunRepository&) = delete;
  WpaRunRepository& operator=(const WpaRunRepository&) = delete;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_RUN_REPOSITORY_H_
