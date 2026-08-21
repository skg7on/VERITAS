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

#include "veritas/summarydb/SummaryDelta.h"

#include "veritas/summary/ComponentHash.h"
#include "veritas/summary/FunctionSummary.h"

namespace veritas::summarydb {

bool SummaryDelta::HasChanged(summary::v1::ComponentKind kind) const {
  for (const auto& component : changed_components) {
    if (component.component_kind == kind) {
      return true;
    }
  }
  return false;
}

StatusOr<SummaryDelta> DiffSummaries(
    const summary::v1::FunctionSummary& old_summary,
    const summary::v1::FunctionSummary& new_summary) {
  auto old_id_result = summary::ComputeFunctionSummaryId(old_summary);
  if (!old_id_result.ok()) {
    return old_id_result.status();
  }
  auto new_id_result = summary::ComputeFunctionSummaryId(new_summary);
  if (!new_id_result.ok()) {
    return new_id_result.status();
  }

  SummaryDelta delta;
  delta.old_summary_id = *old_id_result;
  delta.new_summary_id = *new_id_result;

  // Compare every component kind. Only record a ComponentDelta when the
  // semantic or evidence hash changed; a component whose hashes are identical
  // carries no scheduling signal.
  for (int i = summary::v1::COMPONENT_KIND_CALLS;
       i <= summary::v1::COMPONENT_KIND_PROVENANCE; ++i) {
    const auto kind = static_cast<summary::v1::ComponentKind>(i);
    const auto old_digest = summary::ComputeComponentDigest(kind, old_summary);
    const auto new_digest = summary::ComputeComponentDigest(kind, new_summary);

    const bool semantic_changed =
        old_digest.semantic_hash != new_digest.semantic_hash;
    const bool evidence_changed =
        old_digest.evidence_hash != new_digest.evidence_hash;
    if (!semantic_changed && !evidence_changed) {
      continue;
    }

    delta.changed_components.push_back(ComponentDelta{
        kind,
        old_digest.semantic_hash,
        new_digest.semantic_hash,
        old_digest.evidence_hash,
        new_digest.evidence_hash,
    });
    delta.semantic_changed = delta.semantic_changed || semantic_changed;
    delta.evidence_changed = delta.evidence_changed || evidence_changed;
  }

  return delta;
}

}  // namespace veritas::summarydb
