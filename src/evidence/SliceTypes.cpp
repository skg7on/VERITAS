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

#include "veritas/evidence/SliceTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::evidence {

namespace {

namespace sem = analysis::semantic;
namespace fp = veritas::fact::v1;

core::StableId StableIdFromText(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(
      kind, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// ---------------------------------------------------------------------------
// Stable text spellings for the enums that appear in diagnostic JSON and in
// the canonical completion-fact cells.
// ---------------------------------------------------------------------------

std::string_view AliasKindText(sem::AliasKind kind) {
  switch (kind) {
    case sem::AliasKind::kMustAlias: return "must_alias";
    case sem::AliasKind::kMayAlias: return "may_alias";
    case sem::AliasKind::kNoAlias: return "no_alias";
    case sem::AliasKind::kUnknownAlias: return "unknown_alias";
  }
  return "unknown_alias";
}

std::string_view EpistemicText(sem::EpistemicState state) {
  switch (state) {
    case sem::EpistemicState::kMust: return "must";
    case sem::EpistemicState::kMay: return "may";
    case sem::EpistemicState::kMustNot: return "must_not";
    case sem::EpistemicState::kInferred: return "inferred";
    case sem::EpistemicState::kAssumed: return "assumed";
    case sem::EpistemicState::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view DispatchKindText(sem::DispatchKind kind) {
  switch (kind) {
    case sem::DispatchKind::kDirect: return "direct";
    case sem::DispatchKind::kIndirect: return "indirect";
    case sem::DispatchKind::kVirtual: return "virtual";
    case sem::DispatchKind::kCallback: return "callback";
    case sem::DispatchKind::kExternal: return "external";
    case sem::DispatchKind::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view ByteRangeKindText(sem::ByteRangeKind kind) {
  switch (kind) {
    case sem::ByteRangeKind::kKnown: return "known";
    case sem::ByteRangeKind::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view NodeKindText(cpg::NodeKind kind) {
  switch (kind) {
    case cpg::NodeKind::kFunction: return "function";
    case cpg::NodeKind::kParameter: return "parameter";
    case cpg::NodeKind::kGlobal: return "global";
    case cpg::NodeKind::kCallSite: return "callsite";
    case cpg::NodeKind::kMemoryObject: return "memory_object";
    case cpg::NodeKind::kBasicBlockSummary: return "basic_block_summary";
    case cpg::NodeKind::kSummary: return "summary";
    case cpg::NodeKind::kUnknown: return "unknown";
  }
  return "unknown";
}

std::string_view EdgeKindText(cpg::EdgeKind kind) {
  switch (kind) {
    case cpg::EdgeKind::kContains: return "contains";
    case cpg::EdgeKind::kDeclares: return "declares";
    case cpg::EdgeKind::kCalls: return "calls";
    case cpg::EdgeKind::kMayCall: return "may_call";
    case cpg::EdgeKind::kReads: return "reads";
    case cpg::EdgeKind::kWrites: return "writes";
    case cpg::EdgeKind::kFlowsTo: return "flows_to";
    case cpg::EdgeKind::kAliases: return "aliases";
    case cpg::EdgeKind::kDominatesSummary: return "dominates_summary";
    case cpg::EdgeKind::kSummarizedBy: return "summarized_by";
    case cpg::EdgeKind::kUnknownAt: return "unknown_at";
  }
  return "unknown_at";
}

std::string_view ProducerKindText(facts::ProducerKind kind) {
  switch (kind) {
    case facts::ProducerKind::kWpaSouffle: return "wpa_souffle";
    case facts::ProducerKind::kWpaCppConformance: return "wpa_cpp_conformance";
    case facts::ProducerKind::kWpaCppEmergency: return "wpa_cpp_emergency";
    case facts::ProducerKind::kExternal: return "external";
  }
  return "external";
}

std::string_view ProtoProducerKindText(fp::ProducerKind kind) {
  switch (kind) {
    case fp::ProducerKind::PRODUCER_WPA_SOUFFLE: return "wpa_souffle";
    case fp::ProducerKind::PRODUCER_WPA_CPP_CONFORMANCE:
      return "wpa_cpp_conformance";
    case fp::ProducerKind::PRODUCER_WPA_CPP_EMERGENCY: return "wpa_cpp_emergency";
    case fp::ProducerKind::PRODUCER_EXTERNAL: return "external";
    default: return "unspecified";
  }
}

// ---------------------------------------------------------------------------
// Deterministic JSON building blocks.
// ---------------------------------------------------------------------------

void AppendEscaped(std::string* out, std::string_view text) {
  out->push_back('"');
  for (const char c : text) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const char* hex = "0123456789abcdef";
          const auto v = static_cast<unsigned char>(c);
          out->append("\\u00");
          out->push_back(hex[(v >> 4) & 0xF]);
          out->push_back(hex[v & 0xF]);
        } else {
          out->push_back(c);
        }
        break;
    }
  }
  out->push_back('"');
}

void AppendStringValue(std::string* out, std::string_view text) {
  AppendEscaped(out, text);
}

void AppendSizeValue(std::string* out, std::size_t value) {
  out->append(std::to_string(value));
}

void AppendInt64Value(std::string* out, std::int64_t value) {
  out->append(std::to_string(value));
}

void AppendUint64Value(std::string* out, std::uint64_t value) {
  out->append(std::to_string(value));
}

void AppendBoolValue(std::string* out, bool value) {
  out->append(value ? "true" : "false");
}

void AppendStableIdValue(std::string* out, const core::StableId& id) {
  AppendStringValue(out, core::ToString(id));
}

void AppendSemanticCell(std::string* out,
                        const facts::SemanticCellValue& cell) {
  std::visit(
      [out](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          AppendStableIdValue(out, value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          AppendInt64Value(out, value);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          AppendUint64Value(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          AppendStringValue(out, value);
        } else if constexpr (std::is_same_v<T, sem::DispatchKind>) {
          AppendStringValue(out, DispatchKindText(value));
        } else if constexpr (std::is_same_v<T, sem::AliasKind>) {
          AppendStringValue(out, AliasKindText(value));
        } else if constexpr (std::is_same_v<T, sem::ByteRangeKind>) {
          AppendStringValue(out, ByteRangeKindText(value));
        } else if constexpr (std::is_same_v<T, sem::EpistemicState>) {
          AppendStringValue(out, EpistemicText(value));
        }
      },
      cell);
}

void AppendFact(std::string* out, const facts::AnalysisFact& fact) {
  out->append("{\"fact_id\":");
  AppendStableIdValue(out, fact.fact_id);
  out->append(",\"relation\":");
  AppendStringValue(out, facts::RelationsV2().Get(fact.row.relation).name);
  out->append(",\"cells\":[");
  for (std::size_t i = 0; i < fact.row.cells.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendSemanticCell(out, fact.row.cells[i]);
  }
  out->append("]}");
}

void AppendMetadata(std::string* out, const QueryResultMetadata& metadata) {
  // Truncation reasons serialize in enum order, so the diagnostic form is
  // canonical regardless of the order the caller recorded them in.
  std::vector<TruncationReason> reasons = metadata.truncation_reasons;
  std::sort(reasons.begin(), reasons.end());

  out->append("{\"completeness\":");
  AppendStringValue(out, ToString(metadata.completeness));
  out->append(",\"truncation_reasons\":[");
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendStringValue(out, ToString(reasons[i]));
  }
  out->append("],\"examined_items\":");
  AppendSizeValue(out, metadata.examined_items);
  out->append(",\"analysis_run_id\":");
  AppendStableIdValue(out, metadata.analysis_run_id);
  out->append(",\"query_provenance_id\":");
  AppendStableIdValue(out, metadata.query_provenance_id);
  out->push_back('}');
}

std::vector<facts::AnalysisFact> SortedFacts(
    std::vector<facts::AnalysisFact> facts) {
  std::sort(facts.begin(), facts.end(),
            [](const facts::AnalysisFact& a, const facts::AnalysisFact& b) {
              return a.fact_id < b.fact_id;
            });
  return facts;
}

void AppendFactArray(std::string* out,
                     const std::vector<facts::AnalysisFact>& facts) {
  out->push_back('[');
  for (std::size_t i = 0; i < facts.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendFact(out, facts[i]);
  }
  out->push_back(']');
}

void AppendFactSet(std::string* out, const EvidenceFactSet& set) {
  out->append("{\"facts\":");
  AppendFactArray(out, SortedFacts(set.facts));
  out->append(",\"metadata\":");
  AppendMetadata(out, set.metadata);
  out->push_back('}');
}

void AppendSupportRef(std::string* out, const cpg::SupportRef& support) {
  out->append("{\"function_summary_id\":");
  AppendStableIdValue(out, support.function_summary_id);
  out->append(",\"provenance_ref\":");
  AppendStringValue(out, support.provenance_ref);
  out->push_back('}');
}

void AppendNode(std::string* out, const cpg::CpgNode& node) {
  out->append("{\"node_id\":");
  AppendStableIdValue(out, node.node_id);
  out->append(",\"kind\":");
  AppendStringValue(out, NodeKindText(node.kind));
  out->append(",\"label\":");
  AppendStringValue(out, node.label);
  out->push_back('}');
}

void AppendEdge(std::string* out, const cpg::CpgEdge& edge) {
  out->append("{\"edge_id\":");
  AppendStableIdValue(out, edge.edge_id);
  out->append(",\"kind\":");
  AppendStringValue(out, EdgeKindText(edge.kind));
  out->append(",\"source_node_id\":");
  AppendStableIdValue(out, edge.source_node_id);
  out->append(",\"target_node_id\":");
  AppendStableIdValue(out, edge.target_node_id);
  out->append(",\"alias_state\":");
  if (edge.alias_state.has_value()) {
    AppendStringValue(out, AliasKindText(
                               static_cast<sem::AliasKind>(*edge.alias_state)));
  } else {
    out->append("null");
  }
  out->append(",\"expandable\":");
  AppendBoolValue(out, edge.expandable);
  out->append(",\"support\":[");
  for (std::size_t i = 0; i < edge.support.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendSupportRef(out, edge.support[i]);
  }
  out->append("]}");
}

void AppendFlowSlice(std::string* out, const FlowSlice& slice) {
  std::vector<cpg::CpgNode> nodes = slice.nodes;
  std::sort(nodes.begin(), nodes.end(),
            [](const cpg::CpgNode& a, const cpg::CpgNode& b) {
              return a.node_id < b.node_id;
            });
  std::vector<cpg::CpgEdge> edges = slice.edges;
  std::sort(edges.begin(), edges.end(),
            [](const cpg::CpgEdge& a, const cpg::CpgEdge& b) {
              return a.edge_id < b.edge_id;
            });
  std::vector<core::StableId> provenance_refs = slice.provenance_refs;
  std::sort(provenance_refs.begin(), provenance_refs.end());

  out->append("{\"nodes\":[");
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendNode(out, nodes[i]);
  }
  out->append("],\"edges\":[");
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendEdge(out, edges[i]);
  }
  out->append("],\"supporting_facts\":");
  AppendFactArray(out, SortedFacts(slice.supporting_facts));
  out->append(",\"contradicting_facts\":");
  AppendFactArray(out, SortedFacts(slice.contradicting_facts));
  out->append(",\"unknowns\":");
  AppendFactArray(out, SortedFacts(slice.unknowns));
  out->append(",\"provenance_refs\":[");
  for (std::size_t i = 0; i < provenance_refs.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendStableIdValue(out, provenance_refs[i]);
  }
  out->append("],\"metadata\":");
  AppendMetadata(out, slice.metadata);
  out->push_back('}');
}

void AppendClaimSeed(std::string* out, const ClaimSeed& seed) {
  out->append("{\"finding_id\":");
  AppendStableIdValue(out, seed.finding_id);
  out->append(",\"kind\":");
  AppendStringValue(out, ToString(seed.kind));
  out->append(",\"severity\":");
  AppendStringValue(out, ToString(seed.severity));
  out->append(",\"subject_ref\":");
  AppendStableIdValue(out, seed.subject_ref);
  out->append(",\"source_ref\":");
  AppendStableIdValue(out, seed.source_ref);
  out->append(",\"sink_ref\":");
  AppendStableIdValue(out, seed.sink_ref);
  out->push_back('}');
}

void AppendBinding(std::string* out, const facts::RunFactBinding& binding) {
  out->append("{\"run_id\":");
  AppendStableIdValue(out, binding.run_id);
  out->append(",\"fact_id\":");
  AppendStableIdValue(out, binding.fact_id);
  out->append(",\"confidence\":");
  if (binding.confidence.has_value()) {
    out->append(std::to_string(*binding.confidence));
  } else {
    out->append("null");
  }
  out->append(",\"producer_kind\":");
  AppendStringValue(out, ProducerKindText(binding.producer_kind));
  out->append(",\"analyzer_run_id\":");
  AppendStringValue(out, binding.analyzer_run_id);
  out->append(",\"scope_kind\":");
  AppendStringValue(out, binding.scope_kind);
  out->append(",\"scope_id\":");
  AppendStringValue(out, binding.scope_id);
  out->append(",\"selected_witness_id\":");
  AppendStringValue(out, binding.selected_witness_id);
  out->append(",\"is_current\":");
  AppendBoolValue(out, binding.is_current);
  out->push_back('}');
}

// Proto serialization for the provenance graph. The graph is a VERITAS-owned
// protobuf; we serialize it by hand so diagnostic JSON stays byte-deterministic
// and does not depend on protobuf's JSON emitter.
//
// The proto enum ordinals are not aligned with the semantic enums (the proto
// reserves 0 for UNSPECIFIED), so the proto cells use their own spellings.
std::string_view ProtoEpistemicText(fp::EpistemicState state) {
  switch (state) {
    case fp::EpistemicState::EPISTEMIC_MUST: return "must";
    case fp::EpistemicState::EPISTEMIC_MAY: return "may";
    case fp::EpistemicState::EPISTEMIC_MUST_NOT: return "must_not";
    case fp::EpistemicState::EPISTEMIC_INFERRED: return "inferred";
    case fp::EpistemicState::EPISTEMIC_ASSUMED: return "assumed";
    case fp::EpistemicState::EPISTEMIC_UNKNOWN: return "unknown";
    default: return "unspecified";
  }
}

std::string_view ProtoAliasKindText(fp::AliasKind kind) {
  switch (kind) {
    case fp::AliasKind::ALIAS_MUST: return "must_alias";
    case fp::AliasKind::ALIAS_MAY: return "may_alias";
    case fp::AliasKind::ALIAS_NO: return "no_alias";
    case fp::AliasKind::ALIAS_UNKNOWN: return "unknown_alias";
    default: return "unspecified";
  }
}

std::string_view ProtoDispatchKindText(fp::DispatchKind kind) {
  switch (kind) {
    case fp::DispatchKind::DISPATCH_DIRECT: return "direct";
    case fp::DispatchKind::DISPATCH_INDIRECT: return "indirect";
    case fp::DispatchKind::DISPATCH_VIRTUAL: return "virtual";
    case fp::DispatchKind::DISPATCH_CALLBACK: return "callback";
    case fp::DispatchKind::DISPATCH_EXTERNAL: return "external";
    case fp::DispatchKind::DISPATCH_UNKNOWN: return "unknown";
    default: return "unspecified";
  }
}

std::string_view ProtoByteRangeKindText(fp::ByteRangeKind kind) {
  switch (kind) {
    case fp::ByteRangeKind::BYTE_RANGE_KNOWN: return "known";
    case fp::ByteRangeKind::BYTE_RANGE_UNKNOWN: return "unknown";
    default: return "unspecified";
  }
}

void AppendProtoCell(std::string* out, const fp::Cell& cell) {
  switch (cell.value_case()) {
    case fp::Cell::kStableId:
      AppendStringValue(out, cell.stable_id());
      break;
    case fp::Cell::kInt64Value:
      AppendInt64Value(out, cell.int64_value());
      break;
    case fp::Cell::kUint64Value:
      AppendUint64Value(out, cell.uint64_value());
      break;
    case fp::Cell::kStringValue:
      AppendStringValue(out, cell.string_value());
      break;
    case fp::Cell::kDispatchKind:
      AppendStringValue(out, ProtoDispatchKindText(cell.dispatch_kind()));
      break;
    case fp::Cell::kAliasKind:
      AppendStringValue(out, ProtoAliasKindText(cell.alias_kind()));
      break;
    case fp::Cell::kByteRangeKind:
      AppendStringValue(out, ProtoByteRangeKindText(cell.byte_range_kind()));
      break;
    case fp::Cell::kEpistemic:
      AppendStringValue(out, ProtoEpistemicText(cell.epistemic()));
      break;
    case fp::Cell::VALUE_NOT_SET:
      out->append("null");
      break;
  }
}

void AppendProtoFact(std::string* out, const fp::Fact& fact) {
  out->append("{\"fact_id\":");
  AppendStringValue(out, fact.fact_id());
  out->append(",\"relation_name\":");
  AppendStringValue(out, fact.relation_name());
  out->append(",\"relation_schema_version\":");
  AppendStringValue(out, fact.relation_schema_version());
  out->append(",\"cells\":[");
  for (int i = 0; i < fact.cells_size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendProtoCell(out, fact.cells(i));
  }
  out->append("]}");
}

void AppendProtoBinding(std::string* out, const fp::RunFactBinding& binding) {
  out->append("{\"analysis_run_id\":");
  AppendStringValue(out, binding.analysis_run_id());
  out->append(",\"fact_id\":");
  AppendStringValue(out, binding.fact_id());
  out->append(",\"confidence\":");
  if (binding.has_confidence()) {
    out->append(std::to_string(binding.confidence()));
  } else {
    out->append("null");
  }
  out->append(",\"producer_kind\":");
  AppendStringValue(out, ProtoProducerKindText(binding.producer_kind()));
  out->append(",\"analyzer_run_id\":");
  AppendStringValue(out, binding.analyzer_run_id());
  out->append(",\"scope_kind\":");
  AppendStringValue(out, binding.scope_kind());
  out->append(",\"scope_id\":");
  AppendStringValue(out, binding.scope_id());
  out->append(",\"selected_witness_id\":");
  AppendStringValue(out, binding.selected_witness_id());
  out->append(",\"is_current\":");
  AppendBoolValue(out, binding.is_current());
  out->push_back('}');
}

void AppendProtoWitness(std::string* out, const fp::FactWitness& witness) {
  out->append("{\"analysis_run_id\":");
  AppendStringValue(out, witness.analysis_run_id());
  out->append(",\"output_fact_id\":");
  AppendStringValue(out, witness.output_fact_id());
  out->append(",\"witness_id\":");
  AppendStringValue(out, witness.witness_id());
  out->append(",\"selected\":");
  AppendBoolValue(out, witness.selected());
  out->append(",\"producer_kind\":");
  AppendStringValue(out, ProtoProducerKindText(witness.producer_kind()));
  out->append(",\"producer_id\":");
  AppendStringValue(out, witness.producer_id());
  out->append(",\"rule_id\":");
  AppendStringValue(out, witness.rule_id());
  out->append(",\"rule_version\":");
  AppendStringValue(out, witness.rule_version());
  out->append(",\"analyzer_run_id\":");
  AppendStringValue(out, witness.analyzer_run_id());
  out->append(",\"source_anchor_id\":");
  AppendStringValue(out, witness.source_anchor_id());
  out->append(",\"summary_id\":");
  AppendStringValue(out, witness.summary_id());
  out->append(",\"description\":");
  AppendStringValue(out, witness.description());
  out->push_back('}');
}

void AppendProtoEdge(std::string* out, const fp::FactWitnessEdge& edge) {
  out->append("{\"analysis_run_id\":");
  AppendStringValue(out, edge.analysis_run_id());
  out->append(",\"output_fact_id\":");
  AppendStringValue(out, edge.output_fact_id());
  out->append(",\"witness_id\":");
  AppendStringValue(out, edge.witness_id());
  out->append(",\"input_kind\":");
  AppendStringValue(out, edge.input_kind());
  out->append(",\"input_id\":");
  AppendStringValue(out, edge.input_id());
  out->append(",\"input_ordinal\":");
  out->append(std::to_string(edge.input_ordinal()));
  out->push_back('}');
}

void AppendProvenanceGraph(std::string* out,
                           const fp::ProvenanceGraph& graph) {
  std::vector<const fp::FactWitness*> nodes;
  nodes.reserve(static_cast<std::size_t>(graph.nodes_size()));
  for (const auto& node : graph.nodes()) nodes.push_back(&node);
  std::sort(nodes.begin(), nodes.end(),
            [](const fp::FactWitness* a, const fp::FactWitness* b) {
              if (a->output_fact_id() != b->output_fact_id())
                return a->output_fact_id() < b->output_fact_id();
              return a->witness_id() < b->witness_id();
            });

  std::vector<const fp::FactWitnessEdge*> edges;
  edges.reserve(static_cast<std::size_t>(graph.edges_size()));
  for (const auto& edge : graph.edges()) edges.push_back(&edge);
  std::sort(edges.begin(), edges.end(),
            [](const fp::FactWitnessEdge* a, const fp::FactWitnessEdge* b) {
              if (a->output_fact_id() != b->output_fact_id())
                return a->output_fact_id() < b->output_fact_id();
              if (a->witness_id() != b->witness_id())
                return a->witness_id() < b->witness_id();
              return a->input_ordinal() < b->input_ordinal();
            });

  out->append("{\"run_id\":");
  AppendStringValue(out, graph.run_id());
  out->append(",\"fact_id\":");
  AppendStringValue(out, graph.fact_id());
  out->append(",\"fact\":");
  AppendProtoFact(out, graph.fact());
  out->append(",\"binding\":");
  AppendProtoBinding(out, graph.binding());
  out->append(",\"nodes\":[");
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendProtoWitness(out, *nodes[i]);
  }
  out->append("],\"edges\":[");
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (i != 0) out->push_back(',');
    AppendProtoEdge(out, *edges[i]);
  }
  out->append("],\"truncated\":");
  AppendBoolValue(out, graph.truncated());
  out->append(",\"truncation_reason\":");
  AppendStringValue(out, graph.truncation_reason());
  out->push_back('}');
}

bool HasDuplicate(const std::vector<TruncationReason>& reasons) {
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    for (std::size_t j = i + 1; j < reasons.size(); ++j) {
      if (reasons[i] == reasons[j]) return true;
    }
  }
  return false;
}

}  // namespace

std::string_view ToString(QueryCompleteness value) {
  switch (value) {
    case QueryCompleteness::kUnspecified: return "unspecified";
    case QueryCompleteness::kComplete: return "complete";
    case QueryCompleteness::kTruncated: return "truncated";
  }
  return "unspecified";
}

std::string_view ToString(TruncationReason value) {
  switch (value) {
    case TruncationReason::kUnspecified: return "unspecified";
    case TruncationReason::kMaxDepth: return "max_depth";
    case TruncationReason::kMaxNodes: return "max_nodes";
    case TruncationReason::kMaxPaths: return "max_paths";
    case TruncationReason::kMaxFacts: return "max_facts";
    case TruncationReason::kMaxProvenanceDepth: return "max_provenance_depth";
  }
  return "unspecified";
}

std::string_view ToString(ClaimKind value) {
  switch (value) {
    case ClaimKind::kUnspecified: return "unspecified";
    case ClaimKind::kBufferOverflow: return "buffer_overflow";
  }
  return "unspecified";
}

std::string_view ToString(Severity value) {
  switch (value) {
    case Severity::kUnspecified: return "unspecified";
    case Severity::kCritical: return "critical";
    case Severity::kHigh: return "high";
    case Severity::kMedium: return "medium";
    case Severity::kLow: return "low";
    case Severity::kInfo: return "info";
  }
  return "unspecified";
}

StatusOr<QueryCompleteness> ParseQueryCompleteness(std::string_view text) {
  if (text == "unspecified") return QueryCompleteness::kUnspecified;
  if (text == "complete") return QueryCompleteness::kComplete;
  if (text == "truncated") return QueryCompleteness::kTruncated;
  return Status::InvalidArgument("unrecognized completeness");
}

StatusOr<TruncationReason> ParseTruncationReason(std::string_view text) {
  if (text == "unspecified") return TruncationReason::kUnspecified;
  if (text == "max_depth") return TruncationReason::kMaxDepth;
  if (text == "max_nodes") return TruncationReason::kMaxNodes;
  if (text == "max_paths") return TruncationReason::kMaxPaths;
  if (text == "max_facts") return TruncationReason::kMaxFacts;
  if (text == "max_provenance_depth")
    return TruncationReason::kMaxProvenanceDepth;
  return Status::InvalidArgument("unrecognized truncation reason");
}

StatusOr<ClaimKind> ParseClaimKind(std::string_view text) {
  if (text == "unspecified") return ClaimKind::kUnspecified;
  if (text == "buffer_overflow") return ClaimKind::kBufferOverflow;
  return Status::InvalidArgument("unrecognized claim kind");
}

StatusOr<Severity> ParseSeverity(std::string_view text) {
  if (text == "unspecified") return Severity::kUnspecified;
  if (text == "critical") return Severity::kCritical;
  if (text == "high") return Severity::kHigh;
  if (text == "medium") return Severity::kMedium;
  if (text == "low") return Severity::kLow;
  if (text == "info") return Severity::kInfo;
  return Status::InvalidArgument("unrecognized severity");
}

Status ValidateQueryResultMetadata(const QueryResultMetadata& metadata) {
  switch (metadata.completeness) {
    case QueryCompleteness::kUnspecified:
      return Status::InvalidArgument("completeness is unspecified");
    case QueryCompleteness::kComplete:
      if (!metadata.truncation_reasons.empty())
        return Status::InvalidArgument(
            "complete result carries truncation reasons");
      break;
    case QueryCompleteness::kTruncated:
      if (metadata.truncation_reasons.empty())
        return Status::InvalidArgument("truncated result carries no reason");
      break;
  }
  for (const TruncationReason reason : metadata.truncation_reasons) {
    if (reason == TruncationReason::kUnspecified)
      return Status::InvalidArgument("unspecified truncation reason");
  }
  if (HasDuplicate(metadata.truncation_reasons))
    return Status::InvalidArgument("duplicate truncation reason");
  if (metadata.analysis_run_id.kind != core::IdKind::kAnalysisRun)
    return Status::InvalidArgument("analysis run reference has wrong kind");
  if (metadata.query_provenance_id.kind != core::IdKind::kFact)
    return Status::InvalidArgument("query provenance reference has wrong kind");
  return Status::Ok();
}

Status ValidateEvidenceQueryBudget(const EvidenceQueryBudget& budget) {
  if (budget.max_depth == 0) return Status::InvalidArgument("max_depth is zero");
  if (budget.max_nodes == 0) return Status::InvalidArgument("max_nodes is zero");
  if (budget.max_paths == 0) return Status::InvalidArgument("max_paths is zero");
  if (budget.max_facts_per_query == 0)
    return Status::InvalidArgument("max_facts_per_query is zero");
  if (budget.max_provenance_depth == 0)
    return Status::InvalidArgument("max_provenance_depth is zero");
  return Status::Ok();
}

EvidenceFactSet EmptyFactSet(
    QueryCompleteness completeness,
    std::vector<TruncationReason> truncation_reasons) {
  EvidenceFactSet result;
  result.metadata.completeness = completeness;
  result.metadata.truncation_reasons = std::move(truncation_reasons);
  result.metadata.examined_items = 0;
  result.metadata.analysis_run_id =
      StableIdFromText(core::IdKind::kAnalysisRun, "evidence.empty.run");
  result.metadata.query_provenance_id =
      StableIdFromText(core::IdKind::kFact, "evidence.empty.provenance");
  return result;
}

EvidenceFactSet ApplyFactBudget(std::vector<facts::AnalysisFact> candidates,
                                const EvidenceQueryBudget& budget,
                                QueryResultMetadata base_metadata) {
  std::sort(candidates.begin(), candidates.end(),
            [](const facts::AnalysisFact& a, const facts::AnalysisFact& b) {
              return a.fact_id < b.fact_id;
            });
  const std::size_t limit = budget.max_facts_per_query;
  const bool overflow = candidates.size() > limit;

  EvidenceFactSet result;
  result.metadata = base_metadata;
  if (overflow) {
    result.facts.assign(
        candidates.begin(),
        candidates.begin() + static_cast<std::ptrdiff_t>(limit));
    result.metadata.completeness = QueryCompleteness::kTruncated;
    result.metadata.truncation_reasons = {TruncationReason::kMaxFacts};
    // The query probed one row beyond the limit to detect the overflow; that
    // extra assessment proves the boundary without exposing the probe row.
    result.metadata.examined_items = limit + 1;
  } else {
    result.facts = std::move(candidates);
    result.metadata.completeness = QueryCompleteness::kComplete;
    result.metadata.truncation_reasons.clear();
    result.metadata.examined_items = result.facts.size();
  }
  return result;
}

std::string ToDiagnosticJson(const EvidenceFactSet& set) {
  std::string out;
  AppendFactSet(&out, set);
  out.push_back('\n');
  return out;
}

std::string ToDiagnosticJson(const EvidenceBuildInput& input) {
  std::vector<facts::AnalysisFact> completion_facts =
      input.query_completion_facts;
  std::sort(completion_facts.begin(), completion_facts.end(),
            [](const facts::AnalysisFact& a, const facts::AnalysisFact& b) {
              return a.fact_id < b.fact_id;
            });
  std::vector<facts::RunFactBinding> bindings = input.query_completion_bindings;
  std::sort(bindings.begin(), bindings.end(),
            [](const facts::RunFactBinding& a,
               const facts::RunFactBinding& b) {
              if (a.run_id != b.run_id) return a.run_id < b.run_id;
              return a.fact_id < b.fact_id;
            });

  std::string out;
  out.append("{\"claim_seed\":");
  AppendClaimSeed(&out, input.claim_seed);
  out.append(",\"flow_slice\":");
  AppendFlowSlice(&out, input.flow_slice);
  out.append(",\"ranges\":");
  AppendFactSet(&out, input.ranges);
  out.append(",\"capacities\":");
  AppendFactSet(&out, input.capacities);
  out.append(",\"aliases\":");
  AppendFactSet(&out, input.aliases);
  out.append(",\"dominating_checks\":");
  AppendFactSet(&out, input.dominating_checks);
  out.append(",\"unknowns\":");
  AppendFactSet(&out, input.unknowns);
  out.append(",\"query_completion_facts\":");
  AppendFactArray(&out, completion_facts);
  out.append(",\"query_completion_bindings\":[");
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    if (i != 0) out.push_back(',');
    AppendBinding(&out, bindings[i]);
  }
  out.append("],\"provenance\":");
  AppendProvenanceGraph(&out, input.provenance);
  out.push_back('}');
  out.push_back('\n');
  return out;
}

}  // namespace veritas::evidence
