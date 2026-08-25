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

// WpaComponent.h — the engine-neutral per-SCC WPA execution contract.
//
// One execution unit evaluates one component for one SCC. The logical
// component input is the single canonical description of that unit: member
// facts, stable/dense mappings, outgoing calls, successor support, and the
// applicable models. It is engine-neutral by construction, so a production
// Souffle run and a C++ conformance run consume byte-identical input under
// two distinct execution envelopes.

#ifndef VERITAS_WPA_WPA_COMPONENT_H_
#define VERITAS_WPA_WPA_COMPONENT_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "veritas/analysis/semantic/ModelBundle.h"
#include "veritas/core/Ids.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/DenseIdMap.h"
#include "veritas/summary/SummaryArtifact.h"

namespace veritas::wpa {

enum class WpaComponentKind : std::uint8_t {
  kReachability,
  kMemoryEffects,
};

// Canonical text for a component kind. Used in the logical input hash, so the
// tokens are part of the contract and must not be renamed casually.
std::string_view ComponentKindName(WpaComponentKind component);

// Run-local dual identity. Dense IDs are assigned in sorted stable-ID order
// and never escape their AnalysisRun; every dense cell in the EDB is
// reconstructible through these maps.
struct StableIdMappings {
  facts::FunctionDenseMap functions;
  facts::ValueDenseMap values;
  facts::MemoryDenseMap memories;
  facts::CallSiteDenseMap call_sites;
  facts::DenseIdMap<facts::FactId, core::IdKind::kFact> facts;
};

// An input fact together with where it came from. Roots terminate every
// witness chain, so a published result is always traceable to declared inputs.
struct RootedInputFact {
  facts::AnalysisFact fact;
  std::string provenance_ref;
};

// The immutable, engine-neutral description of one component execution.
struct WpaLogicalComponentInput {
  core::StableId scc_id;
  WpaComponentKind component;
  StableIdMappings mappings;
  std::vector<facts::ExecutionRow> edb;
  std::vector<RootedInputFact> local_roots;
  std::vector<RootedInputFact> successor_roots;
  std::string logical_input_hash;
};

// Inputs to materialization. `summaries` supplies the whole analysed set; the
// materializer derives SCC membership from it and emits local facts only for
// members of `scc_id`. `models` may be null when no model bundle is
// configured; the bundle's content hash, when present, participates in the
// logical input hash so a model change misses the component cache.
struct WpaMaterializationRequest {
  facts::AnalysisRunSemanticDescriptor semantics;
  core::StableId scc_id;
  WpaComponentKind component = WpaComponentKind::kReachability;
  std::span<const summary::SummaryArtifact> summaries;
  std::span<const facts::AnalysisFact> successor_support;
  const analysis::semantic::ModelBundle* models = nullptr;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_COMPONENT_H_
