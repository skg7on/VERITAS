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

// FactStoreEvidenceBackend.h — the concrete M9-backed EvidenceReadBackend.
//
// This is the Task 3 production-path adapter: OpenSnapshot pins every read to
// one AnalysisRun in a real FactStore, serving facts through
// FactStore::GetCurrentFacts and provenance through ProvenanceStore::Explain.
// The FactStore is immutable per run (facts are content-addressed and published
// idempotently under a schema-v4 receipt), so unlike the synthetic fake there
// is no mutable "current binding" that can move mid-assembly; reads are stable
// for the lifetime of the run.
//
// Header-only so integration tests share it without a new library target.

#ifndef VERITAS_TESTING_FACT_STORE_EVIDENCE_BACKEND_H_
#define VERITAS_TESTING_FACT_STORE_EVIDENCE_BACKEND_H_

#include <memory>
#include <utility>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceReadBackend.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::testing {

namespace fact_proto = veritas::fact::v1;

// A real-pipeline read backend bound to one FactStore and one analysis run.
// The caller keeps the FactStore alive for the lifetime of the backend.
class FactStoreEvidenceBackend : public evidence::EvidenceReadBackend {
 public:
  FactStoreEvidenceBackend(facts::FactStore& store,
                           evidence::SnapshotDescriptor descriptor)
      : store_(store), descriptor_(std::move(descriptor)) {}

  StatusOr<std::unique_ptr<evidence::EvidenceReadSnapshot>> OpenSnapshot(
      core::StableId analysis_run_id) const override {
    if (analysis_run_id != descriptor_.analysis_run_id) {
      return Status::NotFound("unknown analysis run");
    }
    return std::unique_ptr<evidence::EvidenceReadSnapshot>(
        new Snapshot(&store_, descriptor_));
  }

 private:
  class Snapshot : public evidence::EvidenceReadSnapshot {
   public:
    Snapshot(facts::FactStore* store, const evidence::SnapshotDescriptor& descriptor)
        : store_(store), descriptor_(descriptor) {}

    const evidence::SnapshotDescriptor& descriptor() const override {
      return descriptor_;
    }

    StatusOr<std::vector<facts::AnalysisFact>> GetCurrentFacts() const override {
      return store_->GetCurrentFacts(descriptor_.analysis_run_id);
    }

    StatusOr<fact_proto::ProvenanceGraph> Explain(
        core::StableId fact_id, const facts::ExplainBudget& budget) const override {
      facts::ProvenanceStore provenance(store_->metadata_store());
      return provenance.Explain(descriptor_.analysis_run_id, fact_id, budget);
    }

   private:
    facts::FactStore* store_;
    evidence::SnapshotDescriptor descriptor_;
  };

  facts::FactStore& store_;
  evidence::SnapshotDescriptor descriptor_;
};

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_FACT_STORE_EVIDENCE_BACKEND_H_
