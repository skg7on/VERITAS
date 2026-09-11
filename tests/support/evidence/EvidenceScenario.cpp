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

#include "evidence/EvidenceScenario.h"

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace veritas::testing {

namespace {

namespace sem = veritas::analysis::semantic;
namespace ev = veritas::evidence;

std::string Suffixed(std::string_view name, std::string_view suffix) {
  std::string out(name);
  out += suffix;
  return out;
}

// Builds a fact from a known-valid row. Test-support setup failures abort
// rather than throw (the project builds with exceptions disabled).
veritas::facts::AnalysisFact MakeFactOrAbort(
    veritas::facts::SemanticRow row) {
  auto fact = veritas::facts::MakeFact(std::move(row));
  if (!fact.ok()) {
    std::abort();
  }
  return std::move(fact).value();
}

core::IdKind NodeIdKind(veritas::cpg::NodeKind kind) {
  switch (kind) {
    case veritas::cpg::NodeKind::kFunction:
      return core::IdKind::kFunctionVariant;
    case veritas::cpg::NodeKind::kParameter:
      return core::IdKind::kValueRef;
    case veritas::cpg::NodeKind::kGlobal:
    case veritas::cpg::NodeKind::kMemoryObject:
      return core::IdKind::kMemoryRef;
    case veritas::cpg::NodeKind::kCallSite:
      return core::IdKind::kCallSite;
    case veritas::cpg::NodeKind::kBasicBlockSummary:
      return core::IdKind::kBasicBlockSummary;
    case veritas::cpg::NodeKind::kSummary:
      return core::IdKind::kFunctionSummary;
    case veritas::cpg::NodeKind::kUnknown:
      return core::IdKind::kUnknownNode;
  }
  return core::IdKind::kUnknownNode;
}

}  // namespace

core::StableId EvidenceScenarioBuilder::Id(core::IdKind kind,
                                           std::string_view name) const {
  return core::MakeStableId(
      kind, std::as_bytes(std::span<const char>(name.data(), name.size())));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeAliasFact(
    std::string_view name, sem::AliasKind alias,
    sem::EpistemicState epistemic) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {Id(core::IdKind::kMemoryRef, Suffixed(name, ":left")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":right")), alias,
               epistemic};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeRangeFact(
    std::string_view name, std::int64_t offset, std::uint64_t size) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectRead;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":mem")),
               sem::ByteRangeKind::kKnown, offset, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeCapacityFact(
    std::string_view name, std::uint64_t size) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectWrite;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":mem")),
               sem::ByteRangeKind::kKnown, std::int64_t{0}, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeUnknownFact(
    std::string_view name, std::string_view reason) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kUnknownEffect;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               std::string(name), std::string(reason),
               sem::EpistemicState::kUnknown};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeCheckFact(
    std::string_view name) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kSoundnessCoverage;
  row.cells = {std::string(name), std::string("dominating_check"),
               std::uint64_t{1}, sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

StatusOr<facts::AnalysisFact> EvidenceScenarioBuilder::MakeCompletionFact(
    const ev::QueryCompletionDescriptor& descriptor) const {
  return ev::MakeQueryCompletionFact(descriptor);
}

facts::RunFactBinding EvidenceScenarioBuilder::MakeBinding(
    core::StableId run_id, core::StableId fact_id,
    std::string_view witness_id) const {
  facts::RunFactBinding binding;
  binding.run_id = run_id;
  binding.fact_id = fact_id;
  binding.producer_kind = facts::ProducerKind::kWpaSouffle;
  binding.analyzer_run_id = "scenario";
  binding.scope_kind = "scope";
  binding.scope_id = "scenario";
  binding.selected_witness_id = std::string(witness_id);
  binding.is_current = true;
  return binding;
}

facts::FactWitness EvidenceScenarioBuilder::MakeWitness(
    core::StableId run_id, core::StableId fact_id,
    std::string_view witness_id) const {
  facts::FactWitness witness;
  witness.run_id = run_id;
  witness.output_fact_id = fact_id;
  witness.witness_id = std::string(witness_id);
  witness.selected = true;
  witness.producer_kind = facts::ProducerKind::kWpaSouffle;
  witness.producer_id = "evidence-query";
  witness.rule_id = "evidence.query_completion.v1";
  witness.rule_version = "v1";
  witness.analyzer_run_id = "scenario";
  return witness;
}

cpg::CpgNode EvidenceScenarioBuilder::MakeNode(std::string_view name,
                                               cpg::NodeKind kind) const {
  cpg::CpgNode node;
  node.node_id = Id(NodeIdKind(kind), name);
  node.kind = kind;
  node.label = std::string(name);
  return node;
}

cpg::CpgEdge EvidenceScenarioBuilder::MakeEdge(std::string_view name,
                                               cpg::EdgeKind kind,
                                               core::StableId source,
                                               core::StableId target) const {
  cpg::CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, name);
  edge.kind = kind;
  edge.source_node_id = source;
  edge.target_node_id = target;
  edge.expandable = false;
  return edge;
}

evidence::QueryResultMetadata EvidenceScenarioBuilder::MakeMetadata(
    std::string_view run_name, std::string_view provenance_name,
    evidence::QueryCompleteness completeness,
    std::vector<evidence::TruncationReason> reasons,
    std::size_t examined_items) const {
  evidence::QueryResultMetadata metadata;
  metadata.completeness = completeness;
  metadata.truncation_reasons = std::move(reasons);
  metadata.examined_items = examined_items;
  metadata.analysis_run_id = Id(core::IdKind::kAnalysisRun, run_name);
  metadata.query_provenance_id = Id(core::IdKind::kFact, provenance_name);
  return metadata;
}

}  // namespace veritas::testing
