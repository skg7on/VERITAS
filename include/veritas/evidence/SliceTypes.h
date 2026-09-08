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

// SliceTypes.h — the public M10B evidence slice, budget, seed, and handoff
// types.
//
// These are the typed values the EvidenceQueryService produces and the single
// immutable handoff M10C consumes. Every flow or fact result carries the shared
// QueryResultMetadata, so complete absence and truncated absence stay
// distinguishable through assembly. The types carry no EIR model/codec types
// and no third-party native types.

#ifndef VERITAS_EVIDENCE_SLICE_TYPES_H_
#define VERITAS_EVIDENCE_SLICE_TYPES_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/fact/v1/fact.pb.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"

namespace veritas::evidence {

// QueryCompleteness distinguishes a complete result (the backend proved no
// additional matching member exists) from a truncated one (a budget cut the
// result short). kUnspecified is invalid at every public boundary.
enum class QueryCompleteness {
  kUnspecified,
  kComplete,
  kTruncated,
};

// TruncationReason names the budget that cut a result short. Declaration order
// is the canonical serialization order for an ordered reason list.
enum class TruncationReason {
  kUnspecified,
  kMaxDepth,
  kMaxNodes,
  kMaxPaths,
  kMaxFacts,
  kMaxProvenanceDepth,
};

// ClaimKind is the M10B-owned finding classification that crosses the M10B/M10C
// boundary. M10C reuses this type rather than declaring a look-alike enum.
enum class ClaimKind {
  kUnspecified,
  kBufferOverflow,
};

// Severity classifies a finding seed. M10B owns this enum for the same reason it
// owns ClaimKind: the seed crosses the M10B/M10C boundary.
enum class Severity {
  kUnspecified,
  kCritical,
  kHigh,
  kMedium,
  kLow,
  kInfo,
};

// Stable text and rejecting parse helpers for the textual enums. The spellings
// are stable contract: they appear in diagnostic JSON and in the canonical
// completion-fact cells.
std::string_view ToString(QueryCompleteness value);
std::string_view ToString(TruncationReason value);
std::string_view ToString(ClaimKind value);
std::string_view ToString(Severity value);

StatusOr<QueryCompleteness> ParseQueryCompleteness(std::string_view text);
StatusOr<TruncationReason> ParseTruncationReason(std::string_view text);
StatusOr<ClaimKind> ParseClaimKind(std::string_view text);
StatusOr<Severity> ParseSeverity(std::string_view text);

// QueryResultMetadata is the shared completion record for FlowSlice and every
// EvidenceFactSet. query_provenance_id references an M9-backed
// evidence.query_completion.v1 fact of kind kFact.
struct QueryResultMetadata {
  QueryCompleteness completeness = QueryCompleteness::kUnspecified;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t examined_items = 0;
  core::StableId analysis_run_id;
  core::StableId query_provenance_id;

  auto operator<=>(const QueryResultMetadata&) const = default;
};

// EvidenceQueryBudget bounds every query. All limits must be positive; reaching
// a limit exactly is complete, while discovering one additional match returns
// the canonical prefix and records the matching truncation reason.
struct EvidenceQueryBudget {
  std::size_t max_depth = 0;
  std::size_t max_nodes = 0;
  std::size_t max_paths = 0;
  std::size_t max_facts_per_query = 0;
  std::size_t max_provenance_depth = 0;

  auto operator<=>(const EvidenceQueryBudget&) const = default;
};

// Validates result metadata at a public boundary. Rejects unspecified
// completeness, a complete result carrying reasons, a truncated result without
// a reason, duplicate or unspecified reasons, and missing/wrong-kind analysis
// run and query provenance references.
Status ValidateQueryResultMetadata(const QueryResultMetadata& metadata);

// Validates a budget: every limit must be positive.
Status ValidateEvidenceQueryBudget(const EvidenceQueryBudget& budget);

// EvidenceFactSet is one bounded fact query result: the canonical facts plus
// their shared completion metadata.
struct EvidenceFactSet {
  std::vector<facts::AnalysisFact> facts;
  QueryResultMetadata metadata;
};

// FlowSlice is the bounded value-flow query result. Supporting and
// contradicting facts stay separate so assembly never resolves a conflict by
// dropping one side.
struct FlowSlice {
  std::vector<cpg::CpgNode> nodes;
  std::vector<cpg::CpgEdge> edges;
  std::vector<facts::AnalysisFact> supporting_facts;
  std::vector<facts::AnalysisFact> contradicting_facts;
  std::vector<facts::AnalysisFact> unknowns;
  std::vector<core::StableId> provenance_refs;
  QueryResultMetadata metadata;
};

// ClaimSeed is the finding M10C wraps into an EvidenceCase. All references are
// stable IDs; kind and severity are the M10B-owned enums.
struct ClaimSeed {
  core::StableId finding_id;
  ClaimKind kind;
  Severity severity;
  core::StableId subject_ref;
  core::StableId source_ref;
  core::StableId sink_ref;
};

// EvidenceBuildInput is the single immutable handoff M10C consumes. Summary and
// source-anchor references ride inside FlowSlice (CPG nodes/edges and their
// support) and the provenance graph; there are no separate top-level
// summaries/source-anchor collections.
struct EvidenceBuildInput {
  ClaimSeed claim_seed;
  FlowSlice flow_slice;
  EvidenceFactSet ranges;
  EvidenceFactSet capacities;
  EvidenceFactSet aliases;
  EvidenceFactSet dominating_checks;
  EvidenceFactSet unknowns;
  std::vector<facts::AnalysisFact> query_completion_facts;
  std::vector<facts::RunFactBinding> query_completion_bindings;
  veritas::fact::v1::ProvenanceGraph provenance;
};

// Builds an empty fact set whose metadata validates for its declared state:
// synthetic but well-formed analysis-run and query-provenance references are
// attached, so complete-empty and truncated-empty stay distinguishable.
EvidenceFactSet EmptyFactSet(
    QueryCompleteness completeness,
    std::vector<TruncationReason> truncation_reasons);

// Deterministically applies max_facts_per_query to a candidate list. Candidates
// are sorted by fact ID before the limit applies, so the returned prefix is
// canonical and independent of backend insertion order. An overflow marks the
// result kTruncated with kMaxFacts and records examined_items as limit + 1 (the
// single extra probe), which proves the boundary without exposing the probe row.
EvidenceFactSet ApplyFactBudget(std::vector<facts::AnalysisFact> candidates,
                                const EvidenceQueryBudget& budget,
                                QueryResultMetadata base_metadata);

// Deterministic diagnostic JSON (canonical semantic ordering by stable ID, one
// trailing newline). This is a debug representation; M10C never parses it.
std::string ToDiagnosticJson(const EvidenceFactSet& set);
std::string ToDiagnosticJson(const EvidenceBuildInput& input);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_SLICE_TYPES_H_
