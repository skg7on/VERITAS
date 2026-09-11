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

// FactStoreEvidenceBackend.h — the concrete M9-backed EvidenceReadBackend and
// the snapshot-descriptor derivation shared by the query CLI and the evidence
// integration tests.
//
// OpenSnapshot pins every read to one AnalysisRun in a real FactStore, serving
// facts through FactStore::GetCurrentFacts and provenance through
// ProvenanceStore::Explain. The FactStore is immutable per run (facts are
// content-addressed and published idempotently under a schema-v4 receipt), so
// unlike the synthetic fake there is no mutable "current binding" that can move
// mid-assembly; reads are stable for the lifetime of the run.
//
// DeriveSnapshotDescriptor is the single authority for the descriptor the query
// service pins. It is deliberately minimal: it derives the descriptor from an
// already-loaded CPG projection, an already-open FactStore, and the run's
// current facts. Materializing the analysis itself (the M1→M6 pipeline) is not
// this header's job — `veritas-build analyze` and the integration-test
// harnesses own that.

#ifndef VERITAS_EVIDENCE_FACT_STORE_EVIDENCE_BACKEND_H_
#define VERITAS_EVIDENCE_FACT_STORE_EVIDENCE_BACKEND_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/EvidenceReadBackend.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::evidence {

namespace fact_proto = veritas::fact::v1;

// The canonical analysis-configuration identifier the V1 pipeline runs under.
// It is part of the snapshot fingerprint, so it must be stable across runs.
inline constexpr std::string_view kAnalysisConfigV1 =
    "veritas-analysis-config.v1";

// A real-pipeline read backend bound to one FactStore and one analysis run.
// The caller keeps the FactStore alive for the lifetime of the backend.
class FactStoreEvidenceBackend : public EvidenceReadBackend {
 public:
  FactStoreEvidenceBackend(facts::FactStore& store,
                           SnapshotDescriptor descriptor)
      : store_(store), descriptor_(std::move(descriptor)) {}

  StatusOr<std::unique_ptr<EvidenceReadSnapshot>> OpenSnapshot(
      core::StableId analysis_run_id) const override {
    if (analysis_run_id != descriptor_.analysis_run_id) {
      return Status::NotFound("unknown analysis run");
    }
    return std::unique_ptr<EvidenceReadSnapshot>(
        new Snapshot(&store_, descriptor_));
  }

 private:
  class Snapshot : public EvidenceReadSnapshot {
   public:
    Snapshot(facts::FactStore* store, const SnapshotDescriptor& descriptor)
        : store_(store), descriptor_(descriptor) {}

    const SnapshotDescriptor& descriptor() const override {
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
    SnapshotDescriptor descriptor_;
  };

  facts::FactStore& store_;
  SnapshotDescriptor descriptor_;
};

// Derives the pinned snapshot descriptor for one materialized analysis run.
//
// `repository` is the content-derived RepositoryID (never the checkout path),
// `revision`/`build_variant` are read back from the CPG ProjectionMetadata so
// they agree with the projection by construction, and
// `fact_snapshot_fingerprint` is the canonical digest over the run's current
// fact IDs (order-independent). Falls back to the empty string for `repository`
// when the caller has none, which changes the fingerprint but not its
// determinism.
inline SnapshotDescriptor DeriveSnapshotDescriptor(
    const cpg::ThinCpg& cpg, std::string repository,
    core::StableId analysis_run_id,
    const std::vector<facts::AnalysisFact>& current_facts) {
  SnapshotDescriptor descriptor;
  descriptor.repository = std::move(repository);
  descriptor.revision = core::ToString(cpg.metadata().revision_id);
  descriptor.build_variant = core::ToString(cpg.metadata().build_variant_id);
  descriptor.analysis_config = std::string(kAnalysisConfigV1);
  descriptor.analysis_run_id = analysis_run_id;
  descriptor.fact_snapshot_fingerprint = ReturnedMemberDigest(current_facts);
  return descriptor;
}

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_FACT_STORE_EVIDENCE_BACKEND_H_
