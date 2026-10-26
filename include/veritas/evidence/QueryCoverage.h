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

#ifndef VERITAS_EVIDENCE_QUERY_COVERAGE_H_
#define VERITAS_EVIDENCE_QUERY_COVERAGE_H_

#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::evidence {

inline constexpr std::string_view kQueryCoverageVersion =
    "evidence.coverage.v1";
inline constexpr std::string_view kEvidenceHandoffVersion =
    "evidence.handoff.v2";
inline constexpr std::string_view kNativeCoverageProducerVersion =
    "veritas-evidence-query.coverage.v1";

enum class ScopeResolution { kResolved, kUnresolvedScope };
std::string_view ToString(ScopeResolution value);

// Semantic coverage is independent of traversal completion. This sidecar
// names the immutable v1 completion fact; it never changes that fact's cells.
// Frontier refs are diagnostic stable references, not EIR entity declarations.
struct QueryCoverage {
  std::string schema_version = std::string(kQueryCoverageVersion);
  core::StableId completion_fact_id{};
  core::StableId analysis_run_id{};
  std::string query_kind;
  bool predicate_supported = false;
  bool scope_closed = false;
  ScopeResolution scope_resolution = ScopeResolution::kResolved;
  std::vector<core::StableId> frontier_refs;
  std::string producer_version;

  auto operator<=>(const QueryCoverage &) const = default;
};

Status ValidateQueryCoverage(const QueryCoverage &coverage);

struct EvidenceBuildInput;
// The canonical diagnostic payload, excluding the digest field itself, is
// hashed with its version and all six sidecars. No completion FactID changes.
// Requires schema-valid rows; the handoff validator checks untrusted rows
// before invoking this serializer.
std::string ComputeEvidenceHandoffDigest(const EvidenceBuildInput &input);
// Legacy = all three envelope fields absent. Partial or malformed envelopes,
// mismatched completion/result bindings, and mixed runs are refused.
bool IsLegacyEvidenceHandoff(const EvidenceBuildInput &input);
Status ValidateEvidenceHandoffCoverage(const EvidenceBuildInput &input);

} // namespace veritas::evidence
#endif // VERITAS_EVIDENCE_QUERY_COVERAGE_H_
