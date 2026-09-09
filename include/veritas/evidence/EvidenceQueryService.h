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

// EvidenceQueryService.h — the bounded, deterministic M10B query layer.
//
// Every query runs against one immutable CPG projection plus one pinned M9
// read snapshot (opened through the EvidenceReadBackend abstraction), is
// bounded by an EvidenceQueryBudget, and returns QueryResultMetadata whose
// query_provenance_id references an M9-backed evidence.query_completion.v1
// certificate. BuildEvidenceInput runs each bounded query exactly once against
// the same snapshot and returns the single immutable typed handoff M10C
// consumes; it performs no EIR mapping, identity, or serialization.

#ifndef VERITAS_EVIDENCE_EVIDENCE_QUERY_SERVICE_H_
#define VERITAS_EVIDENCE_EVIDENCE_QUERY_SERVICE_H_

#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/evidence/EvidenceReadBackend.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::evidence {

// The stable query producer identity stamped on every completion certificate's
// selected witness and run binding. M10C reuses these spellings rather than
// declaring look-alike constants.
inline constexpr std::string_view kQueryImplementationVersion =
    "veritas-evidence-query.v1";
inline constexpr std::string_view kQueryCompletionRuleId =
    "evidence.query_completion.v1";
inline constexpr std::string_view kQueryCompletionProducerId = "evidence-query";
inline constexpr std::string_view kQueryCompletionWitnessId = "completion";

// SHA-256 hex over the canonical ordered member IDs of a bounded result.
// Fact sets hash the sorted fact IDs; flow hashes the sorted node IDs then the
// sorted edge IDs. This is the returned_member_digest cell of the completion
// certificate, so callers (and tests) recompute it from the result payload.
std::string ReturnedMemberDigest(const std::vector<facts::AnalysisFact>& facts);
std::string ReturnedMemberDigest(const FlowSlice& slice);

class EvidenceQueryService {
 public:
  // Binds an immutable CPG projection and a read backend for one analysis run.
  EvidenceQueryService(const cpg::ThinCpg& cpg,
                       const EvidenceReadBackend& backend,
                       core::StableId analysis_run_id);

  StatusOr<FlowSlice> GetValueFlow(core::StableId src, core::StableId dst,
                                   EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetRanges(core::StableId value_ref,
                                      EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetCapacities(core::StableId memory_ref,
                                          EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetAliases(core::StableId memory_ref,
                                       EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetUnknowns(core::StableId scope_ref,
                                        EvidenceQueryBudget budget) const;
  StatusOr<EvidenceFactSet> GetDominatingChecks(core::StableId callsite_ref,
                                                EvidenceQueryBudget budget) const;
  StatusOr<veritas::fact::v1::ProvenanceGraph> Explain(
      core::StableId run_id, core::StableId fact_id,
      const facts::ExplainBudget& budget) const;

  StatusOr<EvidenceBuildInput> BuildEvidenceInput(const ClaimSeed& claim_seed,
                                                  EvidenceQueryBudget budget) const;

 private:
  const cpg::ThinCpg* cpg_;
  const EvidenceReadBackend* backend_;
  core::StableId run_id_;
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_QUERY_SERVICE_H_
