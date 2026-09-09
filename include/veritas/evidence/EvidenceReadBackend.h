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

// EvidenceReadBackend.h — the narrow M9 read-backend abstraction the
// EvidenceQueryService queries through.
//
// The concrete FactStore/ProvenanceStore adapter belongs to Task 3. This
// header defines only the interface the query service needs plus the pinned
// snapshot descriptor: a read-only window into one analysis run whose
// identity is fixed for the lifetime of a single assembly. Keeping the
// backend behind an interface is what lets the synthetic HND-002 test inject
// a fake whose "current binding" changes mid-assembly.

#ifndef VERITAS_EVIDENCE_EVIDENCE_READ_BACKEND_H_
#define VERITAS_EVIDENCE_EVIDENCE_READ_BACKEND_H_

#include <memory>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/fact/v1/fact.pb.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::evidence {

// The identity of a pinned analysis snapshot, minus the CPG projection and the
// combined fingerprint, which the query service derives from the projection it
// owns. Every field is part of the snapshot-fingerprint contract.
struct SnapshotDescriptor {
  std::string repository;
  std::string revision;
  std::string build_variant;
  std::string analysis_config;
  core::StableId analysis_run_id;
  std::string fact_snapshot_fingerprint;
};

// A read-only window into one analysis run. All reads must return data from
// the snapshot the backend opened and nothing else. If the backend cannot
// retain a stable binding for that snapshot, every read returns
// Status::FailedPrecondition with the exact stable text
// "evidence snapshot changed; retry"; the query service propagates it and
// never retries a subquery against a newer run.
class EvidenceReadSnapshot {
 public:
  virtual ~EvidenceReadSnapshot() = default;

  virtual const SnapshotDescriptor& descriptor() const = 0;

  // The facts currently bound in the pinned run, in backend order. Callers
  // sort by canonical semantic ID before applying any budget.
  virtual StatusOr<std::vector<facts::AnalysisFact>> GetCurrentFacts() const = 0;

  // Bounded explanation of one fact within the pinned run.
  virtual StatusOr<veritas::fact::v1::ProvenanceGraph> Explain(
      core::StableId fact_id, const facts::ExplainBudget& budget) const = 0;
};

// Opens a pinned snapshot for one analysis run. The concrete adapter (Task 3)
// pins every read to that run; a backend that cannot retain the binding
// reports the stable retryable failure described on EvidenceReadSnapshot.
class EvidenceReadBackend {
 public:
  virtual ~EvidenceReadBackend() = default;

  virtual StatusOr<std::unique_ptr<EvidenceReadSnapshot>> OpenSnapshot(
      core::StableId analysis_run_id) const = 0;
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_READ_BACKEND_H_
