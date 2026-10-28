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

#include "veritas/evidence/QueryCoverage.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <variant>

#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/SliceTypes.h"

namespace veritas::evidence {
namespace {
bool ValidId(const core::StableId &id, core::IdKind kind) {
  return id.kind == kind && core::ParseStableId(core::ToString(id)).ok();
}
bool KnownKind(std::string_view kind) {
  return kind == "value_flow" || kind == "range" || kind == "capacity" ||
         kind == "alias" || kind == "unknown" || kind == "dominating_check";
}
const std::string *TextCell(const facts::AnalysisFact &fact,
                            std::size_t index) {
  return index < fact.row.cells.size()
             ? std::get_if<std::string>(&fact.row.cells[index])
             : nullptr;
}
} // namespace

std::string_view ToString(ScopeResolution value) {
  switch (value) {
  case ScopeResolution::kResolved:
    return "RESOLVED";
  case ScopeResolution::kUnresolvedScope:
    return "UNRESOLVED_SCOPE";
  }
  return "UNSPECIFIED";
}

Status ValidateQueryCoverage(const QueryCoverage &coverage) {
  if (coverage.schema_version != kQueryCoverageVersion ||
      coverage.producer_version.empty() || !KnownKind(coverage.query_kind) ||
      !ValidId(coverage.completion_fact_id, core::IdKind::kFact) ||
      !ValidId(coverage.analysis_run_id, core::IdKind::kAnalysisRun)) {
    return Status::InvalidArgument(
        "invalid query coverage version, producer, kind or identity");
  }
  if (coverage.scope_resolution != ScopeResolution::kResolved &&
      coverage.scope_resolution != ScopeResolution::kUnresolvedScope) {
    return Status::InvalidArgument("invalid query coverage scope resolution");
  }
  if (coverage.scope_closed &&
      (coverage.scope_resolution != ScopeResolution::kResolved ||
       !coverage.frontier_refs.empty())) {
    return Status::InvalidArgument(
        "closed query coverage cannot have an unresolved scope or frontier");
  }
  std::set<core::StableId> refs;
  for (const auto &ref : coverage.frontier_refs) {
    if (!core::ParseStableId(core::ToString(ref)).ok() ||
        !refs.insert(ref).second) {
      return Status::InvalidArgument(
          "invalid or duplicate query coverage frontier");
    }
  }
  return Status::Ok();
}

bool IsLegacyEvidenceHandoff(const EvidenceBuildInput &input) {
  return input.handoff_version.empty() && input.query_coverage.empty() &&
         input.handoff_digest.empty();
}

Status ValidateEvidenceHandoffCoverage(const EvidenceBuildInput &input) {
  if (!input.provenance_version.empty() &&
      input.provenance_version != kEvidenceProvenanceVersion)
    return Status::InvalidArgument("unknown evidence provenance capability");
  if ((input.provenance_version.empty() && !input.fact_explanations.empty()) ||
      (!input.provenance_version.empty() && IsLegacyEvidenceHandoff(input)))
    return Status::InvalidArgument("partial evidence provenance capability");
  if (input.summaries.size() != input.summary_selections.size())
    return Status::InvalidArgument("summary payload/selection mismatch");
  std::map<core::StableId, const SummarySelection *> selections;
  std::set<core::StableId> functions;
  for (const auto &selection : input.summary_selections) {
    if (!ValidId(selection.summary_id, core::IdKind::kFunctionSummary) ||
        !ValidId(selection.function_variant_id,
                 core::IdKind::kFunctionVariant) ||
        selection.analysis_run_id !=
            input.flow_slice.metadata.analysis_run_id ||
        !selections.emplace(selection.summary_id, &selection).second ||
        !functions.insert(selection.function_variant_id).second)
      return Status::InvalidArgument(
          "invalid or stale summary selection binding");
  }
  std::set<core::StableId> artifacts;
  for (const auto &artifact : input.summaries) {
    const auto expected_schema =
        std::holds_alternative<summary::v1::FunctionSummary>(artifact)
            ? "summary.v1"
            : "summary.v2";
    if (summary::SchemaVersion(artifact) != expected_schema)
      return Status::InvalidArgument(
          "summary schema does not match its payload");
    auto id = summary::ComputeFunctionSummaryId(artifact);
    if (!id.ok())
      return id.status();
    auto selected = selections.find(*id);
    if (selected == selections.end() || !artifacts.insert(*id).second ||
        summary::Identity(artifact).function_variant_id() !=
            core::ToString(selected->second->function_variant_id))
      return Status::InvalidArgument("stale summary identity binding");
    auto computed = summary::ComputeComponentDigests(artifact);
    auto supplied = selected->second->components;
    auto order = [](const auto &a, const auto &b) { return a.kind < b.kind; };
    std::sort(computed.begin(), computed.end(), order);
    std::sort(supplied.begin(), supplied.end(), order);
    if (computed != supplied)
      return Status::InvalidArgument("stale summary component binding");
    bool valid = std::visit(
        [&](const auto &value) {
          if (static_cast<std::size_t>(value.component_digests_size()) !=
              computed.size())
            return false;
          std::set<int> seen;
          for (const auto &digest : value.component_digests()) {
            const auto it = std::ranges::find(
                computed, digest.kind(), &summary::ComponentDigestInfo::kind);
            if (it == computed.end() ||
                !seen.insert(static_cast<int>(digest.kind())).second ||
                digest.semantic_hash() !=
                    std::string(reinterpret_cast<const char *>(
                                    it->semantic_hash.data()),
                                it->semantic_hash.size()) ||
                digest.evidence_hash() !=
                    std::string(reinterpret_cast<const char *>(
                                    it->evidence_hash.data()),
                                it->evidence_hash.size()) ||
                digest.item_count() != it->item_count ||
                digest.payload_offset() != it->payload_offset ||
                digest.payload_length() != it->payload_length)
              return false;
          }
          return true;
        },
        artifact);
    if (!valid)
      return Status::InvalidArgument(
          "summary advertises invalid component hashes");
  }
  if (!selections.empty()) {
    for (const auto &node : input.provenance.nodes()) {
      if (node.summary_id().empty())
        continue;
      auto id = core::ParseStableId(node.summary_id());
      if (!id.ok() || !selections.contains(*id))
        return Status::InvalidArgument(
            "provenance references a stale or missing summary: " +
            node.summary_id());
    }
  }
  if (IsLegacyEvidenceHandoff(input))
    return Status::Ok();
  if (input.handoff_version != kEvidenceHandoffVersion ||
      input.query_coverage.size() != 6 ||
      input.query_completion_facts.size() != 6 ||
      input.query_completion_bindings.size() != 6) {
    return Status::InvalidArgument(
        "invalid evidence handoff coverage envelope or digest");
  }
  // The diagnostic serializer assumes schema-valid rows when naming relations.
  // Refuse malformed payloads before hashing can reach its unchecked registry
  // lookup. This also keeps non-finite annotations out of the modern JSON.
  for (const auto *rows :
       {&input.query_completion_facts, &input.ranges.facts,
        &input.capacities.facts, &input.aliases.facts,
        &input.dominating_checks.facts, &input.unknowns.facts,
        &input.flow_slice.supporting_facts,
        &input.flow_slice.contradicting_facts, &input.flow_slice.unknowns}) {
    for (const auto &fact : *rows) {
      if (auto status = facts::ValidateSemanticRow(fact.row); !status.ok())
        return status;
    }
  }
  for (const auto &binding : input.query_completion_bindings) {
    if (binding.confidence && !std::isfinite(*binding.confidence))
      return Status::InvalidArgument("nonfinite handoff binding confidence");
  }
  if (input.provenance.binding().has_confidence() &&
      !std::isfinite(input.provenance.binding().confidence()))
    return Status::InvalidArgument("nonfinite handoff provenance confidence");
  if (input.provenance_version == kEvidenceProvenanceVersion) {
    const std::string run =
        core::ToString(input.flow_slice.metadata.analysis_run_id);
    std::set<core::StableId> selected, explained;
    for (const auto *rows :
         {&input.ranges.facts, &input.capacities.facts, &input.aliases.facts,
          &input.dominating_checks.facts, &input.unknowns.facts,
          &input.flow_slice.supporting_facts,
          &input.flow_slice.contradicting_facts, &input.flow_slice.unknowns})
      for (const auto &fact : *rows)
        selected.insert(fact.fact_id);
    std::map<std::string, std::string> witnesses;
    for (const auto &node : input.provenance.nodes()) {
      if (node.analysis_run_id() != run)
        return Status::InvalidArgument("provenance witness mixes runs");
      if (node.selected() &&
          !witnesses.emplace(node.output_fact_id(), node.witness_id()).second)
        return Status::InvalidArgument("duplicate selected provenance witness");
    }
    for (const auto &explanation : input.fact_explanations) {
      const auto &binding = explanation.binding;
      if (core::ToString(binding.run_id) != run || !binding.is_current ||
          binding.selected_witness_id.empty() ||
          !selected.contains(binding.fact_id) ||
          !explained.insert(binding.fact_id).second ||
          (binding.confidence && !std::isfinite(*binding.confidence)))
        return Status::InvalidArgument("invalid selected fact binding");
      const auto witness = witnesses.find(core::ToString(binding.fact_id));
      if (witness != witnesses.end() &&
          witness->second != binding.selected_witness_id)
        return Status::InvalidArgument("stale selected fact witness binding");
      if (explanation.truncation_reason.empty() &&
          (witness == witnesses.end() || !explanation.frontier_refs.empty()))
        return Status::InvalidArgument(
            "complete fact explanation has a missing witness or frontier");
      if (!explanation.truncation_reason.empty() &&
          (!input.provenance.truncated() || explanation.frontier_refs.empty()))
        return Status::InvalidArgument(
            "truncated fact explanation lost its frontier");
      std::set<core::StableId> frontier;
      for (const auto &ref : explanation.frontier_refs)
        if (!ValidId(ref, core::IdKind::kFact) || !frontier.insert(ref).second)
          return Status::InvalidArgument("invalid provenance frontier");
    }
    if (selected != explained)
      return Status::InvalidArgument(
          "selected fact has no explanation binding");
    std::map<std::string, std::set<std::string>> successors;
    std::map<std::string, std::size_t> indegree;
    for (const auto &edge : input.provenance.edges()) {
      if (edge.analysis_run_id() != run ||
          !witnesses.contains(edge.output_fact_id()) ||
          witnesses[edge.output_fact_id()] != edge.witness_id() ||
          !core::ParseStableId(edge.input_id()).ok())
        return Status::InvalidArgument("invalid provenance edge binding");
      if (edge.input_kind() == "derived") {
        indegree.try_emplace(edge.output_fact_id(), 0);
        indegree.try_emplace(edge.input_id(), 0);
        if (successors[edge.output_fact_id()].insert(edge.input_id()).second)
          ++indegree[edge.input_id()];
      } else if (edge.input_kind() != "rooted") {
        return Status::InvalidArgument("unknown provenance input kind");
      }
    }
    std::vector<std::string> ready;
    for (const auto &[id, degree] : indegree)
      if (degree == 0)
        ready.push_back(id);
    for (std::size_t i = 0; i < ready.size(); ++i)
      for (const auto &next : successors[ready[i]])
        if (--indegree[next] == 0)
          ready.push_back(next);
    if (ready.size() != indegree.size())
      return Status::InvalidArgument("selected provenance contains a cycle");
  }
  if (input.handoff_digest != ComputeEvidenceHandoffDigest(input))
    return Status::InvalidArgument(
        "invalid evidence handoff coverage envelope or digest");
  const auto &run = input.flow_slice.metadata.analysis_run_id;
  if (input.provenance.run_id() != core::ToString(run)) {
    return Status::InvalidArgument("query coverage mixes analysis runs");
  }
  const std::pair<std::string_view, const QueryResultMetadata *> slots[] = {
      {"value_flow", &input.flow_slice.metadata},
      {"range", &input.ranges.metadata},
      {"capacity", &input.capacities.metadata},
      {"alias", &input.aliases.metadata},
      {"unknown", &input.unknowns.metadata},
      {"dominating_check", &input.dominating_checks.metadata}};
  std::map<core::StableId, const QueryCoverage *> records;
  for (const auto &coverage : input.query_coverage) {
    if (auto status = ValidateQueryCoverage(coverage); !status.ok())
      return status;
    if (coverage.analysis_run_id != run ||
        !records.emplace(coverage.completion_fact_id, &coverage).second) {
      return Status::InvalidArgument(
          "duplicate coverage or mixed analysis run");
    }
  }
  std::set<core::StableId> consumed;
  std::string snapshot;
  for (const auto &[kind, metadata] : slots) {
    if (auto status = ValidateQueryResultMetadata(*metadata); !status.ok())
      return status;
    auto record = records.find(metadata->query_provenance_id);
    if (metadata->analysis_run_id != run || record == records.end() ||
        record->second->query_kind != kind ||
        !consumed.insert(metadata->query_provenance_id).second) {
      return Status::InvalidArgument(
          "query coverage does not bind its result and run");
    }
    const auto fact = std::ranges::find(input.query_completion_facts,
                                        metadata->query_provenance_id,
                                        &facts::AnalysisFact::fact_id);
    if (fact == input.query_completion_facts.end() ||
        fact->row.relation != facts::RelationId::kQueryCompletion ||
        fact->row.cells.size() != 9) {
      return Status::InvalidArgument("query coverage has no completion fact");
    }
    auto derived = facts::DeriveFactId(fact->row);
    if (!derived.ok() || *derived != fact->fact_id) {
      return Status::InvalidArgument(
          "query coverage completion identity does not rederive");
    }
    const auto *query = TextCell(*fact, 0);
    const auto *scope = TextCell(*fact, 1);
    const auto *fingerprint = TextCell(*fact, 4);
    const auto *complete = TextCell(*fact, 5);
    const auto *reasons = TextCell(*fact, 6);
    const auto *examined = std::get_if<std::uint64_t>(&fact->row.cells[7]);
    const auto *digest = TextCell(*fact, 8);
    std::string expected_reasons;
    auto ordered = metadata->truncation_reasons;
    std::sort(ordered.begin(), ordered.end());
    for (const auto reason : ordered) {
      if (!expected_reasons.empty())
        expected_reasons += ",";
      expected_reasons += ToString(reason);
    }
    const auto expected_digest =
        kind == "value_flow"
            ? ReturnedMemberDigest(input.flow_slice)
            : ReturnedMemberDigest(kind == "range"      ? input.ranges.facts
                                   : kind == "capacity" ? input.capacities.facts
                                   : kind == "alias"    ? input.aliases.facts
                                   : kind == "unknown"
                                       ? input.unknowns.facts
                                       : input.dominating_checks.facts);
    if (!query || *query != kind || !scope || !fingerprint ||
        fingerprint->empty() || !complete ||
        *complete != ToString(metadata->completeness) || !reasons ||
        *reasons != expected_reasons || !examined ||
        *examined != metadata->examined_items || !digest ||
        *digest != expected_digest) {
      return Status::InvalidArgument(
          "query coverage completion does not describe its result");
    }
    if ((record->second->scope_resolution ==
         ScopeResolution::kUnresolvedScope) != scope->empty()) {
      return Status::InvalidArgument(
          "query coverage scope disagrees with completion scope");
    }
    if (snapshot.empty())
      snapshot = *fingerprint;
    if (*fingerprint != snapshot) {
      return Status::InvalidArgument("query coverage mixes input snapshots");
    }
    const auto binding =
        std::ranges::find(input.query_completion_bindings, fact->fact_id,
                          &facts::RunFactBinding::fact_id);
    if (binding == input.query_completion_bindings.end() ||
        binding->run_id != run) {
      return Status::InvalidArgument(
          "query coverage has no same-run completion binding");
    }
  }
  return Status::Ok();
}
} // namespace veritas::evidence
