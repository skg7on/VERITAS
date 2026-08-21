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

#ifndef VERITAS_SUMMARYDB_SUMMARY_DELTA_H_
#define VERITAS_SUMMARYDB_SUMMARY_DELTA_H_

#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::summarydb {

// ComponentDelta records the per-component semantic and evidence hash change
// between two revisions of a summary. A component is present in a SummaryDelta
// only when at least one of its hashes changed.
struct ComponentDelta {
  summary::v1::ComponentKind component_kind;
  core::SHA256Digest old_semantic_hash;
  core::SHA256Digest new_semantic_hash;
  core::SHA256Digest old_evidence_hash;
  core::SHA256Digest new_evidence_hash;

  bool SemanticChanged() const { return old_semantic_hash != new_semantic_hash; }
  bool EvidenceChanged() const { return old_evidence_hash != new_evidence_hash; }

  bool operator==(const ComponentDelta&) const = default;
};

// SummaryDelta is the M7 scheduling signal: which components of a summary
// changed semantically (must re-run semantic consumers) versus evidence-only
// (must re-run evidence consumers). It carries the old and new summary IDs so
// the reverse dependency index can be queried against the old summary.
struct SummaryDelta {
  core::StableId old_summary_id;
  core::StableId new_summary_id;
  std::vector<ComponentDelta> changed_components;
  bool semantic_changed = false;
  bool evidence_changed = false;

  // True when the given component kind appears among the changed components.
  bool HasChanged(summary::v1::ComponentKind kind) const;
};

// Diff two summaries component-by-component using the M3 component hashes.
// Returns the resulting SummaryDelta, or an error if either summary cannot be
// serialized to compute its FunctionSummaryID.
StatusOr<SummaryDelta> DiffSummaries(
    const summary::v1::FunctionSummary& old_summary,
    const summary::v1::FunctionSummary& new_summary);

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_SUMMARY_DELTA_H_
