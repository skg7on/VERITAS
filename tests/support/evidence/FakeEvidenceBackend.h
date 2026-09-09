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

// FakeEvidenceBackend.h — a test double for the EvidenceReadBackend that
// serves deterministic in-memory facts and provenance and can simulate a
// "current binding" change mid-assembly (HND-002). Header-only so the query
// and handoff tests share it without a new library target.

#ifndef VERITAS_TESTING_FAKE_EVIDENCE_BACKEND_H_
#define VERITAS_TESTING_FAKE_EVIDENCE_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceReadBackend.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactProto.h"

namespace veritas::testing {

namespace fake_proto = veritas::fact::v1;

// A single-run read backend. The current fact set and a generation counter are
// mutable; OpenSnapshot captures the generation and any read after a later
// BumpGeneration fails with the stable retryable text.
class FakeEvidenceBackend : public evidence::EvidenceReadBackend {
 public:
  explicit FakeEvidenceBackend(evidence::SnapshotDescriptor descriptor)
      : descriptor_(std::move(descriptor)),
        generation_(0),
        provenance_depth_(3),
        auto_bump_(false) {}

  void SetFacts(std::vector<facts::AnalysisFact> facts) {
    facts_ = std::move(facts);
  }

  // The length of the deterministic witness chain Explain synthesizes.
  void SetProvenanceDepth(std::uint32_t depth) { provenance_depth_ = depth; }

  // Simulates the backend's current binding changing mid-assembly.
  void BumpGeneration() { ++generation_; }

  // When enabled, every successful fact read advances the generation, so the
  // next read (e.g. the next BuildEvidenceInput subquery) sees a stale binding
  // and fails with the stable retryable text.
  void SetAutoBump(bool enabled) { auto_bump_ = enabled; }

  core::StableId run_id() const { return descriptor_.analysis_run_id; }

  StatusOr<std::unique_ptr<evidence::EvidenceReadSnapshot>> OpenSnapshot(
      core::StableId analysis_run_id) const override {
    if (analysis_run_id != descriptor_.analysis_run_id) {
      return Status::NotFound("unknown analysis run");
    }
    return std::unique_ptr<evidence::EvidenceReadSnapshot>(
        new Snapshot(this, generation_));
  }

 private:
  class Snapshot : public evidence::EvidenceReadSnapshot {
   public:
    Snapshot(const FakeEvidenceBackend* backend, std::uint64_t generation)
        : backend_(backend), generation_(generation) {}

    const evidence::SnapshotDescriptor& descriptor() const override {
      return backend_->descriptor_;
    }

    StatusOr<std::vector<facts::AnalysisFact>> GetCurrentFacts() const override {
      if (backend_->generation_ != generation_) {
        return Status::FailedPrecondition("evidence snapshot changed; retry");
      }
      std::vector<facts::AnalysisFact> facts = backend_->facts_;
      if (backend_->auto_bump_) {
        ++backend_->generation_;
      }
      return facts;
    }

    StatusOr<fake_proto::ProvenanceGraph> Explain(
        core::StableId fact_id,
        const facts::ExplainBudget& budget) const override {
      if (backend_->generation_ != generation_) {
        return Status::FailedPrecondition("evidence snapshot changed; retry");
      }
      return backend_->Explain(fact_id, budget);
    }

   private:
    const FakeEvidenceBackend* backend_;
    std::uint64_t generation_;
  };

  // Builds a deterministic provenance graph for one fact: the semantic fact is
  // always present, followed by a witness chain of provenance_depth_ nodes. The
  // graph truncates with "max_depth" when the budget cannot reach the full
  // chain, mirroring ProvenanceStore::Explain's fact-preserving truncation.
  StatusOr<fake_proto::ProvenanceGraph> Explain(
      core::StableId fact_id, const facts::ExplainBudget& budget) const {
    const facts::AnalysisFact* fact = nullptr;
    for (const auto& candidate : facts_) {
      if (candidate.fact_id == fact_id) {
        fact = &candidate;
        break;
      }
    }
    if (fact == nullptr) {
      return Status::NotFound("fact not found");
    }

    fake_proto::ProvenanceGraph graph;
    graph.set_run_id(core::ToString(descriptor_.analysis_run_id));
    graph.set_fact_id(core::ToString(fact_id));

    auto proto_fact = facts::ToProtoFact(*fact);
    if (!proto_fact.ok()) {
      return proto_fact.status();
    }
    *graph.mutable_fact() = std::move(*proto_fact);

    auto* binding = graph.mutable_binding();
    binding->set_analysis_run_id(core::ToString(descriptor_.analysis_run_id));
    binding->set_fact_id(core::ToString(fact_id));
    binding->set_selected_witness_id("w0");
    binding->set_is_current(true);

    for (std::uint32_t i = 0; i < provenance_depth_; ++i) {
      auto* node = graph.add_nodes();
      node->set_analysis_run_id(core::ToString(descriptor_.analysis_run_id));
      node->set_output_fact_id(core::ToString(fact_id));
      node->set_witness_id("w" + std::to_string(i));
      node->set_selected(i == 0);
      node->set_rule_id("evidence.fake_rule.v1");
    }

    graph.set_truncated(budget.max_depth < provenance_depth_);
    if (graph.truncated()) {
      graph.set_truncation_reason("max_depth");
    }
    return graph;
  }

  evidence::SnapshotDescriptor descriptor_;
  std::vector<facts::AnalysisFact> facts_;
  mutable std::uint64_t generation_;
  std::uint32_t provenance_depth_;
  bool auto_bump_;
};

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_FAKE_EVIDENCE_BACKEND_H_
