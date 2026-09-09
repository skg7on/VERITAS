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

#include "veritas/evidence/EvidenceQueryService.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/evidence/QueryCompletion.h"
#include "veritas/facts/FactProto.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::evidence {

namespace {

namespace sem = analysis::semantic;
namespace fp = veritas::fact::v1;

constexpr std::string_view kKindValueFlow = "value_flow";
constexpr std::string_view kKindRange = "range";
constexpr std::string_view kKindCapacity = "capacity";
constexpr std::string_view kKindAlias = "alias";
constexpr std::string_view kKindUnknown = "unknown";
constexpr std::string_view kKindDominatingCheck = "dominating_check";
constexpr std::string_view kDominatingCheckKind = "dominating_check";
constexpr std::string_view kRuleVersion = "v1";

std::string Sha256Hex(std::string_view text) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span<const char>(text.data(), text.size()))));
}

// --- Cell accessors -------------------------------------------------------

const core::StableId* StableIdCell(const facts::SemanticRow& row,
                                   std::size_t index) {
  return index < row.cells.size()
             ? std::get_if<core::StableId>(&row.cells[index])
             : nullptr;
}

const std::string* StringCell(const facts::SemanticRow& row, std::size_t index) {
  return index < row.cells.size() ? std::get_if<std::string>(&row.cells[index])
                                  : nullptr;
}

const sem::EpistemicState* EpistemicCell(const facts::SemanticRow& row,
                                         std::size_t index) {
  return index < row.cells.size()
             ? std::get_if<sem::EpistemicState>(&row.cells[index])
             : nullptr;
}

bool FactIdLess(const facts::AnalysisFact& a, const facts::AnalysisFact& b) {
  return a.fact_id < b.fact_id;
}

// --- Snapshot fingerprint -------------------------------------------------

std::string CpgProjectionFingerprint(const cpg::ThinCpg& cpg) {
  const cpg::ProjectionMetadata& metadata = cpg.metadata();
  std::vector<core::StableId> summary_ids = metadata.summary_ids;
  std::sort(summary_ids.begin(), summary_ids.end());

  std::string canonical;
  canonical += metadata.schema_version;
  canonical.push_back('\n');
  canonical += core::ToString(metadata.revision_id);
  canonical.push_back('\n');
  canonical += core::ToString(metadata.build_variant_id);
  canonical.push_back('\n');
  canonical += metadata.module_hash;
  canonical.push_back('\n');
  for (const auto& id : summary_ids) {
    canonical += core::ToString(id);
    canonical.push_back('\n');
  }
  return Sha256Hex(canonical);
}

std::string SnapshotFingerprint(const SnapshotDescriptor& descriptor,
                                std::string_view cpg_projection_fingerprint) {
  std::string canonical;
  canonical += descriptor.repository;
  canonical.push_back('\n');
  canonical += descriptor.revision;
  canonical.push_back('\n');
  canonical += descriptor.build_variant;
  canonical.push_back('\n');
  canonical += descriptor.analysis_config;
  canonical.push_back('\n');
  canonical += core::ToString(descriptor.analysis_run_id);
  canonical.push_back('\n');
  canonical += cpg_projection_fingerprint;
  canonical.push_back('\n');
  canonical += descriptor.fact_snapshot_fingerprint;
  canonical.push_back('\n');
  return Sha256Hex(canonical);
}

// --- Completion certificate ----------------------------------------------

struct QueryCertificate {
  facts::AnalysisFact completion_fact;
  facts::RunFactBinding binding;
  facts::FactWitness witness;
};

StatusOr<QueryCertificate> MakeQueryCertificate(
    std::string_view query_kind, std::vector<core::StableId> scope_refs,
    const EvidenceQueryBudget& budget, const std::string& fingerprint,
    QueryCompleteness completeness,
    std::vector<TruncationReason> truncation_reasons, std::size_t examined_items,
    std::string returned_member_digest, core::StableId run_id) {
  // Record reasons in canonical enum order from the start so the completion
  // fact's ordered_truncation_reasons and ValidateQueryCompletion's exact-order
  // comparison stay consistent with the JSON serializer's enum-order sort.
  std::sort(truncation_reasons.begin(), truncation_reasons.end());

  QueryCompletionDescriptor descriptor;
  descriptor.query_kind = std::string(query_kind);
  descriptor.ordered_scope_refs = std::move(scope_refs);
  descriptor.budget = budget;
  descriptor.query_implementation_version = std::string(kQueryImplementationVersion);
  descriptor.input_snapshot_fingerprint = fingerprint;
  descriptor.completeness = completeness;
  descriptor.ordered_truncation_reasons = std::move(truncation_reasons);
  descriptor.examined_items = examined_items;
  descriptor.returned_member_digest = std::move(returned_member_digest);

  auto fact = MakeQueryCompletionFact(descriptor);
  if (!fact.ok()) {
    return fact.status();
  }

  QueryCertificate certificate;
  certificate.completion_fact = std::move(*fact);

  certificate.binding.run_id = run_id;
  certificate.binding.fact_id = certificate.completion_fact.fact_id;
  certificate.binding.producer_kind = facts::ProducerKind::kExternal;
  certificate.binding.analyzer_run_id = core::ToString(run_id);
  certificate.binding.scope_kind = "evidence-query";
  certificate.binding.scope_id = std::string(query_kind);
  certificate.binding.selected_witness_id = std::string(kQueryCompletionWitnessId);
  certificate.binding.is_current = true;

  certificate.witness.run_id = run_id;
  certificate.witness.output_fact_id = certificate.completion_fact.fact_id;
  certificate.witness.witness_id = std::string(kQueryCompletionWitnessId);
  certificate.witness.selected = true;
  certificate.witness.producer_kind = facts::ProducerKind::kExternal;
  certificate.witness.producer_id = std::string(kQueryCompletionProducerId);
  certificate.witness.rule_id = std::string(kQueryCompletionRuleId);
  certificate.witness.rule_version = std::string(kRuleVersion);
  certificate.witness.analyzer_run_id = core::ToString(run_id);

  return certificate;
}

// --- Fact query outcomes --------------------------------------------------

struct FactQueryOutcome {
  EvidenceFactSet result;
  QueryCertificate certificate;
};

// Applies the open-world fact budget: matching candidates are sorted by
// canonical fact ID, the canonical prefix is returned, and examined_items
// counts every matching candidate the query assessed.
StatusOr<FactQueryOutcome> RunFactQuery(
    std::vector<facts::AnalysisFact> matches, const EvidenceQueryBudget& budget,
    const std::string& fingerprint, std::string_view query_kind,
    std::vector<core::StableId> scope_refs, core::StableId run_id) {
  std::sort(matches.begin(), matches.end(), FactIdLess);
  const std::size_t examined = matches.size();
  const std::size_t limit = budget.max_facts_per_query;

  EvidenceFactSet result;
  result.metadata.analysis_run_id = run_id;
  if (matches.size() > limit) {
    result.facts.assign(matches.begin(),
                        matches.begin() + static_cast<std::ptrdiff_t>(limit));
    result.metadata.completeness = QueryCompleteness::kTruncated;
    result.metadata.truncation_reasons = {TruncationReason::kMaxFacts};
    result.metadata.examined_items = examined;
  } else {
    result.facts = std::move(matches);
    result.metadata.completeness = QueryCompleteness::kComplete;
    result.metadata.truncation_reasons.clear();
    result.metadata.examined_items = examined;
  }

  auto certificate = MakeQueryCertificate(
      query_kind, std::move(scope_refs), budget, fingerprint,
      result.metadata.completeness, result.metadata.truncation_reasons,
      result.metadata.examined_items, ReturnedMemberDigest(result.facts), run_id);
  if (!certificate.ok()) {
    return certificate.status();
  }
  result.metadata.query_provenance_id = certificate->completion_fact.fact_id;

  return FactQueryOutcome{std::move(result), std::move(*certificate)};
}

// The dominating-check query is closed-world: proving absence requires
// examining every candidate check, so the fact budget bounds the examination
// horizon rather than only the returned prefix. A complete-empty result proves
// no check dominates; a truncated-empty result proves only that the examined
// prefix held none.
StatusOr<FactQueryOutcome> RunDominatingChecks(
    const EvidenceReadSnapshot& snapshot, core::StableId callsite_ref,
    const EvidenceQueryBudget& budget, const std::string& fingerprint,
    core::StableId run_id) {
  auto facts = snapshot.GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }

  std::vector<facts::AnalysisFact> candidates;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kSoundnessCoverage) {
      continue;
    }
    const std::string* kind = StringCell(fact.row, 1);
    if (kind == nullptr || *kind != kDominatingCheckKind) {
      continue;
    }
    candidates.push_back(fact);
  }
  std::sort(candidates.begin(), candidates.end(), FactIdLess);

  const std::size_t limit = budget.max_facts_per_query;
  const std::size_t examined = std::min(candidates.size(), limit);
  const bool truncated = candidates.size() > limit;
  const std::string scope_text = core::ToString(callsite_ref);

  EvidenceFactSet result;
  result.metadata.analysis_run_id = run_id;
  result.metadata.examined_items = examined;
  for (std::size_t i = 0; i < examined; ++i) {
    const std::string* scope = StringCell(candidates[i].row, 0);
    if (scope != nullptr && *scope == scope_text) {
      result.facts.push_back(candidates[i]);
    }
  }
  if (truncated) {
    result.metadata.completeness = QueryCompleteness::kTruncated;
    result.metadata.truncation_reasons = {TruncationReason::kMaxFacts};
  } else {
    result.metadata.completeness = QueryCompleteness::kComplete;
    result.metadata.truncation_reasons.clear();
  }

  auto certificate = MakeQueryCertificate(
      kKindDominatingCheck, {callsite_ref}, budget, fingerprint,
      result.metadata.completeness, result.metadata.truncation_reasons,
      result.metadata.examined_items, ReturnedMemberDigest(result.facts), run_id);
  if (!certificate.ok()) {
    return certificate.status();
  }
  result.metadata.query_provenance_id = certificate->completion_fact.fact_id;

  return FactQueryOutcome{std::move(result), std::move(*certificate)};
}

// --- Value-flow query -----------------------------------------------------

struct AdjacencyEntry {
  core::StableId target;
  core::StableId edge_id;
};

using Adjacency =
    std::map<core::StableId, std::vector<AdjacencyEntry>>;

struct Path {
  std::vector<core::StableId> nodes;
  std::vector<core::StableId> edges;
};

Adjacency BuildFlowAdjacency(const cpg::ThinCpg& cpg) {
  Adjacency adjacency;
  for (const auto& edge : cpg.edges()) {
    if (edge.kind != cpg::EdgeKind::kFlowsTo) {
      continue;
    }
    adjacency[edge.source_node_id].push_back(
        AdjacencyEntry{edge.target_node_id, edge.edge_id});
  }
  for (auto& [source, entries] : adjacency) {
    (void)source;
    std::sort(entries.begin(), entries.end(),
              [](const AdjacencyEntry& a, const AdjacencyEntry& b) {
                return a.target < b.target;
              });
  }
  return adjacency;
}

// All simple paths from src to dst with at most max_depth kFlowsTo edges, in
// canonical (lexicographic) order because the adjacency lists are sorted.
std::vector<Path> EnumeratePaths(const Adjacency& adjacency,
                                 core::StableId src, core::StableId dst,
                                 std::size_t max_depth) {
  std::vector<Path> paths;
  if (src == dst) {
    return paths;
  }

  std::vector<core::StableId> node_stack{src};
  std::vector<core::StableId> edge_stack;
  std::set<core::StableId> visited{src};

  std::function<void(core::StableId)> dfs = [&](core::StableId node) {
    if (node == dst) {
      paths.push_back(Path{node_stack, edge_stack});
      return;
    }
    if (edge_stack.size() >= max_depth) {
      return;  // cannot extend without exceeding the depth budget
    }
    auto it = adjacency.find(node);
    if (it == adjacency.end()) {
      return;
    }
    for (const AdjacencyEntry& entry : it->second) {
      if (visited.count(entry.target) != 0) {
        continue;
      }
      visited.insert(entry.target);
      node_stack.push_back(entry.target);
      edge_stack.push_back(entry.edge_id);
      dfs(entry.target);
      edge_stack.pop_back();
      node_stack.pop_back();
      visited.erase(entry.target);
    }
  };
  dfs(src);
  return paths;
}

bool Reachable(const Adjacency& adjacency, core::StableId src,
               core::StableId dst) {
  std::set<core::StableId> visited;
  std::vector<core::StableId> stack{src};
  while (!stack.empty()) {
    const core::StableId node = stack.back();
    stack.pop_back();
    if (node == dst) {
      return true;
    }
    if (!visited.insert(node).second) {
      continue;
    }
    auto it = adjacency.find(node);
    if (it == adjacency.end()) {
      continue;
    }
    for (const AdjacencyEntry& entry : it->second) {
      stack.push_back(entry.target);
    }
  }
  return false;
}

struct FlowQueryOutcome {
  FlowSlice result;
  QueryCertificate certificate;
};

StatusOr<FlowQueryOutcome> RunValueFlow(
    const cpg::ThinCpg& cpg, const EvidenceReadSnapshot& snapshot,
    core::StableId src, core::StableId dst, const EvidenceQueryBudget& budget,
    const std::string& fingerprint, core::StableId run_id) {
  const Adjacency adjacency = BuildFlowAdjacency(cpg);
  std::vector<Path> paths = EnumeratePaths(adjacency, src, dst, budget.max_depth);

  FlowSlice slice;
  slice.metadata.analysis_run_id = run_id;

  std::vector<TruncationReason> reasons;
  std::size_t examined = 0;

  if (paths.empty()) {
    if (Reachable(adjacency, src, dst)) {
      // Reachable only at a depth beyond the budget: truncated-empty.
      reasons = {TruncationReason::kMaxDepth};
      slice.metadata.completeness = QueryCompleteness::kTruncated;
      examined = 0;
    } else {
      slice.metadata.completeness = QueryCompleteness::kComplete;
      examined = 0;
    }
  } else {
    std::set<core::StableId> result_nodes;
    std::set<core::StableId> result_edges;
    std::vector<Path> returned;
    bool truncated = false;

    for (const Path& path : paths) {
      if (returned.size() == budget.max_paths) {
        reasons = {TruncationReason::kMaxPaths};
        examined = budget.max_paths + 1;  // one extra probe proves overflow
        truncated = true;
        break;
      }
      std::size_t new_nodes = 0;
      for (const auto& node : path.nodes) {
        if (result_nodes.count(node) == 0) {
          ++new_nodes;
        }
      }
      if (result_nodes.size() + new_nodes > budget.max_nodes) {
        reasons = {TruncationReason::kMaxNodes};
        examined = returned.size() + 1;
        truncated = true;
        break;
      }
      returned.push_back(path);
      for (const auto& node : path.nodes) {
        result_nodes.insert(node);
      }
      for (const auto& edge : path.edges) {
        result_edges.insert(edge);
      }
    }

    if (!truncated) {
      slice.metadata.completeness = QueryCompleteness::kComplete;
      examined = returned.size();
    } else {
      slice.metadata.completeness = QueryCompleteness::kTruncated;
    }

    for (const auto& node_id : result_nodes) {
      for (const auto& node : cpg.nodes()) {
        if (node.node_id == node_id) {
          slice.nodes.push_back(node);
          break;
        }
      }
    }
    for (const auto& edge_id : result_edges) {
      for (const auto& edge : cpg.edges()) {
        if (edge.edge_id == edge_id) {
          slice.edges.push_back(edge);
          break;
        }
      }
    }
  }

  slice.metadata.truncation_reasons = reasons;
  slice.metadata.examined_items = examined;

  // Supporting, contradicting, and unknown flow facts are classified from the
  // value-flow closure relations and stay separate.
  auto facts = snapshot.GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kGlobalFlow &&
        fact.row.relation != facts::RelationId::kSupportGlobalFlow) {
      continue;
    }
    const sem::EpistemicState* state = EpistemicCell(fact.row, 2);
    if (state == nullptr) {
      continue;
    }
    if (*state == sem::EpistemicState::kMustNot) {
      slice.contradicting_facts.push_back(fact);
    } else if (*state == sem::EpistemicState::kUnknown) {
      slice.unknowns.push_back(fact);
    } else {
      slice.supporting_facts.push_back(fact);
    }
  }
  std::sort(slice.supporting_facts.begin(), slice.supporting_facts.end(),
            FactIdLess);
  std::sort(slice.contradicting_facts.begin(), slice.contradicting_facts.end(),
            FactIdLess);
  std::sort(slice.unknowns.begin(), slice.unknowns.end(), FactIdLess);

  for (const auto& fact : slice.supporting_facts) {
    slice.provenance_refs.push_back(fact.fact_id);
  }
  for (const auto& fact : slice.contradicting_facts) {
    slice.provenance_refs.push_back(fact.fact_id);
  }
  for (const auto& fact : slice.unknowns) {
    slice.provenance_refs.push_back(fact.fact_id);
  }
  std::sort(slice.provenance_refs.begin(), slice.provenance_refs.end());

  auto certificate = MakeQueryCertificate(
      kKindValueFlow, {src, dst}, budget, fingerprint,
      slice.metadata.completeness, slice.metadata.truncation_reasons,
      slice.metadata.examined_items, ReturnedMemberDigest(slice), run_id);
  if (!certificate.ok()) {
    return certificate.status();
  }
  slice.metadata.query_provenance_id = certificate->completion_fact.fact_id;

  return FlowQueryOutcome{std::move(slice), std::move(*certificate)};
}

// --- Provenance proto helpers --------------------------------------------

fp::ProducerKind ToProtoProducerKind(facts::ProducerKind kind) {
  switch (kind) {
    case facts::ProducerKind::kWpaSouffle:
      return fp::PRODUCER_WPA_SOUFFLE;
    case facts::ProducerKind::kWpaCppConformance:
      return fp::PRODUCER_WPA_CPP_CONFORMANCE;
    case facts::ProducerKind::kWpaCppEmergency:
      return fp::PRODUCER_WPA_CPP_EMERGENCY;
    case facts::ProducerKind::kExternal:
      return fp::PRODUCER_EXTERNAL;
  }
  return fp::PRODUCER_UNSPECIFIED;
}

void FillProtoBinding(fp::RunFactBinding* out,
                      const facts::RunFactBinding& binding) {
  out->set_analysis_run_id(core::ToString(binding.run_id));
  out->set_fact_id(core::ToString(binding.fact_id));
  if (binding.confidence.has_value()) {
    out->set_confidence(*binding.confidence);
  }
  out->set_producer_kind(ToProtoProducerKind(binding.producer_kind));
  out->set_analyzer_run_id(binding.analyzer_run_id);
  out->set_scope_kind(binding.scope_kind);
  out->set_scope_id(binding.scope_id);
  out->set_selected_witness_id(binding.selected_witness_id);
  out->set_is_current(binding.is_current);
}

void FillProtoWitness(fp::FactWitness* out, const facts::FactWitness& witness) {
  out->set_analysis_run_id(core::ToString(witness.run_id));
  out->set_output_fact_id(core::ToString(witness.output_fact_id));
  out->set_witness_id(witness.witness_id);
  out->set_selected(witness.selected);
  out->set_producer_kind(ToProtoProducerKind(witness.producer_kind));
  out->set_producer_id(witness.producer_id);
  out->set_rule_id(witness.rule_id);
  out->set_rule_version(witness.rule_version);
  out->set_analyzer_run_id(witness.analyzer_run_id);
  out->set_source_anchor_id(witness.source_anchor_id);
  out->set_summary_id(witness.summary_id);
  out->set_description(witness.description);
}

// The function containing a CPG node, via a kContains edge, or nullopt.
std::optional<core::StableId> FindContainingFunction(const cpg::ThinCpg& cpg,
                                                     core::StableId node_id) {
  std::map<core::StableId, cpg::NodeKind> kinds;
  for (const auto& node : cpg.nodes()) {
    kinds[node.node_id] = node.kind;
  }
  for (const auto& edge : cpg.edges()) {
    if (edge.kind != cpg::EdgeKind::kContains ||
        edge.target_node_id != node_id) {
      continue;
    }
    auto it = kinds.find(edge.source_node_id);
    if (it != kinds.end() && it->second == cpg::NodeKind::kFunction) {
      return edge.source_node_id;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string ReturnedMemberDigest(const std::vector<facts::AnalysisFact>& facts) {
  std::vector<facts::AnalysisFact> sorted = facts;
  std::sort(sorted.begin(), sorted.end(), FactIdLess);
  std::string canonical;
  for (const auto& fact : sorted) {
    canonical += core::ToString(fact.fact_id);
  }
  return Sha256Hex(canonical);
}

std::string ReturnedMemberDigest(const FlowSlice& slice) {
  std::vector<core::StableId> node_ids;
  node_ids.reserve(slice.nodes.size());
  for (const auto& node : slice.nodes) {
    node_ids.push_back(node.node_id);
  }
  std::vector<core::StableId> edge_ids;
  edge_ids.reserve(slice.edges.size());
  for (const auto& edge : slice.edges) {
    edge_ids.push_back(edge.edge_id);
  }
  std::sort(node_ids.begin(), node_ids.end());
  std::sort(edge_ids.begin(), edge_ids.end());

  std::string canonical;
  for (const auto& id : node_ids) {
    canonical += core::ToString(id);
  }
  for (const auto& id : edge_ids) {
    canonical += core::ToString(id);
  }
  return Sha256Hex(canonical);
}

EvidenceQueryService::EvidenceQueryService(const cpg::ThinCpg& cpg,
                                           const EvidenceReadBackend& backend,
                                           core::StableId analysis_run_id)
    : cpg_(&cpg), backend_(&backend), run_id_(analysis_run_id) {}

StatusOr<FlowSlice> EvidenceQueryService::GetValueFlow(
    core::StableId src, core::StableId dst, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunValueFlow(*cpg_, **snapshot, src, dst, budget, fingerprint,
                              run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<EvidenceFactSet> EvidenceQueryService::GetRanges(
    core::StableId value_ref, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = (*snapshot)->GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }
  std::vector<facts::AnalysisFact> matches;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kDirectRead) {
      continue;
    }
    const core::StableId* memory = StableIdCell(fact.row, 1);
    if (memory != nullptr && *memory == value_ref) {
      matches.push_back(fact);
    }
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunFactQuery(std::move(matches), budget, fingerprint,
                              kKindRange, {value_ref}, run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<EvidenceFactSet> EvidenceQueryService::GetCapacities(
    core::StableId memory_ref, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = (*snapshot)->GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }
  std::vector<facts::AnalysisFact> matches;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kDirectWrite) {
      continue;
    }
    const core::StableId* memory = StableIdCell(fact.row, 1);
    if (memory != nullptr && *memory == memory_ref) {
      matches.push_back(fact);
    }
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunFactQuery(std::move(matches), budget, fingerprint,
                              kKindCapacity, {memory_ref}, run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<EvidenceFactSet> EvidenceQueryService::GetAliases(
    core::StableId memory_ref, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = (*snapshot)->GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }
  std::vector<facts::AnalysisFact> matches;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kAlias) {
      continue;
    }
    const core::StableId* left = StableIdCell(fact.row, 0);
    const core::StableId* right = StableIdCell(fact.row, 1);
    if ((left != nullptr && *left == memory_ref) ||
        (right != nullptr && *right == memory_ref)) {
      matches.push_back(fact);
    }
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunFactQuery(std::move(matches), budget, fingerprint,
                              kKindAlias, {memory_ref}, run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<EvidenceFactSet> EvidenceQueryService::GetUnknowns(
    core::StableId scope_ref, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = (*snapshot)->GetCurrentFacts();
  if (!facts.ok()) {
    return facts.status();
  }
  std::vector<facts::AnalysisFact> matches;
  for (const auto& fact : *facts) {
    if (fact.row.relation != facts::RelationId::kUnknownEffect &&
        fact.row.relation != facts::RelationId::kSupportUnknownEffect) {
      continue;
    }
    const core::StableId* function = StableIdCell(fact.row, 0);
    if (function != nullptr && *function == scope_ref) {
      matches.push_back(fact);
    }
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunFactQuery(std::move(matches), budget, fingerprint,
                              kKindUnknown, {scope_ref}, run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<EvidenceFactSet> EvidenceQueryService::GetDominatingChecks(
    core::StableId callsite_ref, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));
  auto outcome = RunDominatingChecks(**snapshot, callsite_ref, budget, fingerprint,
                                     run_id_);
  if (!outcome.ok()) {
    return outcome.status();
  }
  return std::move(outcome->result);
}

StatusOr<veritas::fact::v1::ProvenanceGraph> EvidenceQueryService::Explain(
    core::StableId run_id, core::StableId fact_id,
    const facts::ExplainBudget& budget) const {
  auto snapshot = backend_->OpenSnapshot(run_id);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  return (*snapshot)->Explain(fact_id, budget);
}

StatusOr<EvidenceBuildInput> EvidenceQueryService::BuildEvidenceInput(
    const ClaimSeed& claim_seed, EvidenceQueryBudget budget) const {
  if (Status s = ValidateEvidenceQueryBudget(budget); !s.ok()) {
    return s;
  }
  auto snapshot = backend_->OpenSnapshot(run_id_);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  const std::string fingerprint =
      SnapshotFingerprint((*snapshot)->descriptor(),
                          CpgProjectionFingerprint(*cpg_));

  EvidenceBuildInput input;
  input.claim_seed = claim_seed;

  std::vector<QueryCertificate> certificates;

  auto flow = RunValueFlow(*cpg_, **snapshot, claim_seed.source_ref,
                           claim_seed.sink_ref, budget, fingerprint, run_id_);
  if (!flow.ok()) {
    return flow.status();
  }
  input.flow_slice = std::move(flow->result);
  certificates.push_back(std::move(flow->certificate));

  const std::optional<core::StableId> function_scope =
      FindContainingFunction(*cpg_, claim_seed.sink_ref);
  const core::StableId unknown_scope =
      function_scope.value_or(core::StableId{core::IdKind::kFunctionVariant, {}});

  // Run each fact query against the same snapshot. Results and certificates
  // are validated together before the handoff is returned.
  auto run_fact_query = [&](EvidenceFactSet* slot, core::StableId ref,
                            std::string_view kind) -> Status {
    auto facts = (*snapshot)->GetCurrentFacts();
    if (!facts.ok()) {
      return facts.status();
    }
    std::vector<facts::AnalysisFact> matches;
    for (const auto& fact : *facts) {
      if (kind == kKindRange) {
        if (fact.row.relation != facts::RelationId::kDirectRead) continue;
        const core::StableId* cell = StableIdCell(fact.row, 1);
        if (cell != nullptr && *cell == ref) matches.push_back(fact);
      } else if (kind == kKindCapacity) {
        if (fact.row.relation != facts::RelationId::kDirectWrite) continue;
        const core::StableId* cell = StableIdCell(fact.row, 1);
        if (cell != nullptr && *cell == ref) matches.push_back(fact);
      } else if (kind == kKindAlias) {
        if (fact.row.relation != facts::RelationId::kAlias) continue;
        const core::StableId* left = StableIdCell(fact.row, 0);
        const core::StableId* right = StableIdCell(fact.row, 1);
        if ((left != nullptr && *left == ref) ||
            (right != nullptr && *right == ref)) {
          matches.push_back(fact);
        }
      } else if (kind == kKindUnknown) {
        if (fact.row.relation != facts::RelationId::kUnknownEffect &&
            fact.row.relation != facts::RelationId::kSupportUnknownEffect) {
          continue;
        }
        const core::StableId* cell = StableIdCell(fact.row, 0);
        if (cell != nullptr && *cell == ref) matches.push_back(fact);
      }
    }
    auto outcome = RunFactQuery(std::move(matches), budget,
                                fingerprint, kind, {ref}, run_id_);
    if (!outcome.ok()) {
      return outcome.status();
    }
    *slot = std::move(outcome->result);
    certificates.push_back(std::move(outcome->certificate));
    return Status::Ok();
  };

  if (Status s =
          run_fact_query(&input.ranges, claim_seed.source_ref, kKindRange);
      !s.ok()) {
    return s;
  }
  if (Status s =
          run_fact_query(&input.capacities, claim_seed.subject_ref, kKindCapacity);
      !s.ok()) {
    return s;
  }
  if (Status s =
          run_fact_query(&input.aliases, claim_seed.subject_ref, kKindAlias);
      !s.ok()) {
    return s;
  }
  if (Status s =
          run_fact_query(&input.unknowns, unknown_scope, kKindUnknown);
      !s.ok()) {
    return s;
  }

  auto checks =
      RunDominatingChecks(**snapshot, claim_seed.sink_ref, budget, fingerprint,
                          run_id_);
  if (!checks.ok()) {
    return checks.status();
  }
  input.dominating_checks = std::move(checks->result);
  certificates.push_back(std::move(checks->certificate));

  // Validate every result and its completion certificate before handing off.
  for (const auto* metadata : {&input.flow_slice.metadata, &input.ranges.metadata,
                               &input.capacities.metadata,
                               &input.aliases.metadata,
                               &input.dominating_checks.metadata,
                               &input.unknowns.metadata}) {
    if (Status s = ValidateQueryResultMetadata(*metadata); !s.ok()) {
      return s;
    }
  }

  // Sort the bundle deterministically and record the provenance closure.
  std::sort(certificates.begin(), certificates.end(),
            [](const QueryCertificate& a, const QueryCertificate& b) {
              return a.completion_fact.fact_id < b.completion_fact.fact_id;
            });
  for (const auto& certificate : certificates) {
    input.query_completion_facts.push_back(certificate.completion_fact);
    input.query_completion_bindings.push_back(certificate.binding);
  }

  const facts::AnalysisFact* flow_certificate = nullptr;
  for (const auto& certificate : certificates) {
    const std::string* kind = StringCell(certificate.completion_fact.row, 0);
    if (kind != nullptr && *kind == kKindValueFlow) {
      flow_certificate = &certificate.completion_fact;
      break;
    }
  }

  auto& provenance = input.provenance;
  provenance.set_run_id(core::ToString(run_id_));
  if (flow_certificate != nullptr) {
    provenance.set_fact_id(core::ToString(flow_certificate->fact_id));
    auto proto_fact = facts::ToProtoFact(*flow_certificate);
    if (proto_fact.ok()) {
      *provenance.mutable_fact() = std::move(*proto_fact);
    }
    for (const auto& binding : input.query_completion_bindings) {
      if (binding.fact_id == flow_certificate->fact_id) {
        FillProtoBinding(provenance.mutable_binding(), binding);
        break;
      }
    }
  }
  for (const auto& certificate : certificates) {
    FillProtoWitness(provenance.add_nodes(), certificate.witness);
  }
  provenance.set_truncated(false);

  return input;
}

}  // namespace veritas::evidence
