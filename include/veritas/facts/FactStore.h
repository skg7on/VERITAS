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

// FactStore.h — the durable M9 fact sink.
//
// Implements AnalysisFactSink: a successful WPA run is published atomically as
// canonical facts, their run bindings, and the rooted witness DAG that derives
// them. Fact identity is the witness-independent FactID from MakeFact, so the
// same semantic row published by different runs shares one analysis_facts row
// and differs only in its run_fact_bindings. Replacing a current fact flips
// the prior binding's is_current off and inserts a new one, keeping history
// readable.

#ifndef VERITAS_FACTS_FACT_STORE_H_
#define VERITAS_FACTS_FACT_STORE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisFactBus.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::facts {

// Who produced a fact. Distinct from epistemic state; persisted so two engines
// deriving the same fact stay distinguishable at the binding level.
enum class ProducerKind : std::uint8_t {
  kWpaSouffle,
  kWpaCppConformance,
  kWpaCppEmergency,
  kExternal,
};

ProducerKind ProducerKindForEngine(EngineIdentity engine);

// The occurrence of a fact within one analysis run. Carries the run-local and
// provenance context that the canonical fact itself excludes.
struct RunFactBinding {
  core::StableId run_id;
  core::StableId fact_id;
  std::optional<double> confidence;  // separate from epistemic state
  ProducerKind producer_kind = ProducerKind::kWpaSouffle;
  std::string analyzer_run_id;
  std::string scope_kind;
  std::string scope_id;
  std::string selected_witness_id;
  bool is_current = true;
};

class FactStore : public AnalysisFactSink {
 public:
  // Opens (or creates) the fact database at <db_path>/metadata.db (the same
  // file the WPA run repository uses) and applies the schema.
  static StatusOr<FactStore> Open(const std::filesystem::path& db_path);

  FactStore(FactStore&&) noexcept;
  FactStore& operator=(FactStore&&) noexcept;
  ~FactStore();

  // Publishes one canonical batch atomically: facts, bindings, and the witness
  // DAG are all committed together or not at all.
  Status Publish(const AnalysisFactBatch& batch) override;

  // The canonical fact, or NotFound if it was never published.
  StatusOr<AnalysisFact> GetFact(core::StableId fact_id);

  // The current binding for (run_id, fact_id), or NotFound.
  StatusOr<RunFactBinding> GetBinding(core::StableId run_id,
                                      core::StableId fact_id);

  // Every binding for (run_id, fact_id), newest first (current, then history).
  StatusOr<std::vector<RunFactBinding>> GetBindings(core::StableId run_id,
                                                    core::StableId fact_id);

  // The facts currently bound in a run (their current bindings).
  StatusOr<std::vector<AnalysisFact>> GetCurrentFacts(core::StableId run_id);

  // The shared SQLite connection, for ProvenanceStore::Explain.
  summarydb::MetadataStore& metadata_store() { return metadata_store_; }

 private:
  explicit FactStore(summarydb::MetadataStore store);

  Status PutFact(const AnalysisFact& fact);
  Status PutBinding(const RunFactBinding& binding);

  summarydb::MetadataStore metadata_store_;

  FactStore(const FactStore&) = delete;
  FactStore& operator=(const FactStore&) = delete;
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_FACT_STORE_H_
