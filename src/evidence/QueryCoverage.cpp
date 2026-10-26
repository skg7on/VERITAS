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
  if (IsLegacyEvidenceHandoff(input))
    return Status::Ok();
  if (input.handoff_version != kEvidenceHandoffVersion ||
      input.query_coverage.size() != 6 ||
      input.query_completion_facts.size() != 6 ||
      input.query_completion_bindings.size() != 6 ||
      input.handoff_digest != ComputeEvidenceHandoffDigest(input)) {
    return Status::InvalidArgument(
        "invalid evidence handoff coverage envelope or digest");
  }
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
