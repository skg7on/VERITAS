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
// query, and handoff tests.
//
// EvidenceScenarioBuilder assigns stable IDs from symbolic names and builds
// valid M9 facts through MakeFact. It never parses JSON or EIR-T; higher-level
// integration tests own the real pipeline.

#ifndef VERITAS_TESTING_EVIDENCE_SCENARIO_H_
#define VERITAS_TESTING_EVIDENCE_SCENARIO_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"
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
};

}  // namespace veritas::testing

#endif  // VERITAS_TESTING_EVIDENCE_SCENARIO_H_
