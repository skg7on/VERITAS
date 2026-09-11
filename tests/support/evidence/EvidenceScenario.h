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

// EvidenceScenario.h — deterministic typed evidence fixtures for the contract,
// query, handoff, semantic-model, and serialization tests.
//
// EvidenceScenarioBuilder assigns stable IDs from symbolic names and builds
// valid M9 facts through MakeFact. It never parses JSON or EIR-T; higher-level
// integration tests own the real pipeline.
//
// Since M10C it also owns the two semantic-case fixtures every later M10C unit
// test starts from, so no test invents a look-alike case:
//
//   * `MakeValidMinimalEvidenceCase()` — the smallest valid initial L0 case.
//   * `MakeOverflowEvidenceCase()` — the fully populated L1 buffer-overflow case
//     of the DEM-001 fixture, with the truncated dominating-check query visible
//     as an unknown plus an expandable omission.
//
// Both are projections of one typed M10B handoff (`BuildRequest`) and both
// carry test-local handles: the local IDs are readable, deterministic handles
// for one case, while the stable IDs they resolve come from the typed scenario.

#ifndef VERITAS_TESTING_EVIDENCE_SCENARIO_H_
#define VERITAS_TESTING_EVIDENCE_SCENARIO_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/QueryCompletion.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::testing {

// Deterministic, typed evidence-scenario constructor. It is test-only support
// and may depend on public VERITAS types; production headers never depend on
// it.
class EvidenceScenarioBuilder {
 public:
  EvidenceScenarioBuilder() = default;

  // A deterministic stable ID of `kind` derived from a symbolic name.
  core::StableId Id(core::IdKind kind, std::string_view name) const;

  // Typed fact factories. Each returns a valid, deterministic AnalysisFact.
  facts::AnalysisFact MakeAliasFact(
      std::string_view name, analysis::semantic::AliasKind alias,
      analysis::semantic::EpistemicState epistemic) const;
  // An alias fact over two explicit memory references: the demo's
  // "the source buffer may alias the destination buffer" premise, which the
  // name-derived factory above cannot express.
  facts::AnalysisFact MakeAliasFact(
      core::StableId left, core::StableId right,
      analysis::semantic::AliasKind alias,
      analysis::semantic::EpistemicState epistemic) const;
  facts::AnalysisFact MakeRangeFact(std::string_view name, std::int64_t offset,
                                    std::uint64_t size) const;
  facts::AnalysisFact MakeCapacityFact(std::string_view name,
                                       std::uint64_t size) const;
  facts::AnalysisFact MakeUnknownFact(std::string_view name,
                                      std::string_view reason) const;
  facts::AnalysisFact MakeCheckFact(std::string_view name) const;

  // Builds a completion fact through the public M10B path.
  StatusOr<facts::AnalysisFact> MakeCompletionFact(
      const evidence::QueryCompletionDescriptor& descriptor) const;

  // A run binding and its selected witness for one completion fact.
  facts::RunFactBinding MakeBinding(core::StableId run_id,
                                    core::StableId fact_id,
                                    std::string_view witness_id) const;
  facts::FactWitness MakeWitness(core::StableId run_id,
                                 core::StableId fact_id,
                                 std::string_view witness_id) const;

  // Flow members.
  cpg::CpgNode MakeNode(std::string_view name, cpg::NodeKind kind) const;
  cpg::CpgEdge MakeEdge(std::string_view name, cpg::EdgeKind kind,
                        core::StableId source, core::StableId target) const;

  // A complete or truncated metadata record bound to named run and provenance
  // references.
  evidence::QueryResultMetadata MakeMetadata(
      std::string_view run_name, std::string_view provenance_name,
      evidence::QueryCompleteness completeness,
      std::vector<evidence::TruncationReason> reasons,
      std::size_t examined_items) const;

  // Reverses a vector in place; the determinism-mutation helper for tests that
  // assert insertion-order independence.
  template <typename T>
  static void Reverse(std::vector<T>& values) {
    std::reverse(values.begin(), values.end());
  }

  // --- M10C semantic-case fixtures ----------------------------------------

  // Switches the dominating-check query result to a truncated-empty one. The
  // metadata, the completion certificate, the run binding, and the projected
  // case's truncation unknown and omission are created together, so the request
  // never carries a truncated result without visible provenance for it. A
  // truncated result is never converted into negative evidence.
  EvidenceScenarioBuilder& WithTruncatedDominatingChecks(
      evidence::TruncationReason reason);

  // The complete typed M10C assembly request at `level`: one M10B
  // `EvidenceBuildInput` whose six query results each carry a validating
  // completion certificate and run binding, plus the program context the input
  // was taken from.
  evidence::EvidenceBuildRequest BuildRequest(
      evidence::EvidenceLevel level) const;

  // The smallest valid initial L0 case: one buffer-overflow claim whose subject
  // resolves to a declared entity, one supporting fact that carries resolvable
  // provenance, the primary path reference, one expandable omission for the
  // withheld L1 slice, and the initial verification state. L0 carries no proof
  // obligation; the omission is how the withheld slice stays visible.
  evidence::EvidenceCase MakeValidMinimalEvidenceCase() const;

  // The fully populated L1 buffer-overflow case of the DEM-001 fixture: the
  // ordered value-flow path to the sink, the range/capacity/alias facts, the
  // summary reference and dependencies, the vendor-validate unknown, the
  // truncated dominating-check unknown with its expandable omission, and the
  // PENDING proof obligation. It is projected from the truncated
  // dominating-check request, so the missing check stays visible.
  evidence::EvidenceCase MakeOverflowEvidenceCase() const;

 private:
  // The shared request builder behind `BuildRequest` and the case factories.
  evidence::EvidenceBuildRequest BuildRequestFor(
      evidence::EvidenceLevel level, bool truncated_dominating_checks,
      evidence::TruncationReason reason) const;

  bool truncated_dominating_checks_ = false;
  evidence::TruncationReason dominating_checks_reason_ =
      evidence::TruncationReason::kUnspecified;
};

// The shared M10C case fixtures, so later M10C tests can call them unqualified.
// Each one is `EvidenceScenarioBuilder().Make...EvidenceCase()`.
evidence::EvidenceCase MakeValidMinimalEvidenceCase();
evidence::EvidenceCase MakeOverflowEvidenceCase();

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_EVIDENCE_SCENARIO_H_
