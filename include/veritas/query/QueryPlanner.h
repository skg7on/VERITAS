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

// QueryPlanner.h — plans the immutable closed/open scope of one query.
//
// Planning is the only place that decides which SCCs a demand query may
// execute. It computes a conservative relevance set (backward from the sinks,
// intersected with forward reachability from the sources), maps it to whole
// SCCs, and never splits a recursive SCC to fit a budget. Every boundary that
// is not admitted and not covered by a readable boundary summary is recorded
// as an explicit frontier instead of being silently dropped.

#ifndef VERITAS_QUERY_QUERY_PLANNER_H_
#define VERITAS_QUERY_QUERY_PLANNER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/query/QueryTypes.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::query {

// One admitted SCC in deterministic reverse-topological execution order.
struct PlannedScc {
  core::StableId scc_id;
  std::vector<core::StableId> members;
  // Every successor SCC of this member set, sorted. A successor that is not
  // admitted must be covered by a boundary summary or the frontier stays open.
  std::vector<core::StableId> successors;

  bool operator==(const PlannedScc &) const = default;
};

struct QueryPlan {
  QueryId query_id;
  QueryDescriptor descriptor;
  // Binds this plan to the exact dependency snapshot it was planned from.
  std::string snapshot_digest;
  core::StableId revision_id{};
  facts::AnalysisRunSemanticDescriptor semantics;
  wpa::WpaComponentKind component = wpa::WpaComponentKind::kReachability;
  facts::RelationId relation = facts::RelationId::kReachableCall;
  std::vector<facts::RelationId> local_relations;
  // Admitted SCCs, successors first. Empty when the whole relevant scope is
  // blocked; an empty plan is an incomplete answer, never a negative fact.
  std::vector<PlannedScc> sccs;
  // Relevant SCCs that were not admitted because of the function or SCC
  // budget. They are retained so the frontier names exactly what was skipped.
  std::vector<PlannedScc> excluded_sccs;
  // Deterministically sorted relevant function identities.
  std::vector<core::StableId> relevant_functions;
  // The descriptor's admitted boundary summaries, in descriptor order.
  std::vector<BoundarySummary> boundary_summaries;
  std::vector<QueryFrontierEntry> frontier;
  // True only when no frontier entry was recorded.
  bool scope_closed = false;
};

// Plans `descriptor` against `snapshot`.
//
// Rejections (InvalidArgument unless stated otherwise):
//   * malformed descriptor, endpoint kinds that disagree with the predicate,
//     zero limits, malformed boundary-summary identities;
//   * a producer set that disagrees with the snapshot's semantics
//     (FailedPrecondition);
//   * an admitted boundary-summary identity the snapshot does not contain
//     (NotFound);
//   * a function endpoint/subject absent from the snapshot (NotFound).
//
// Budget overruns are not rejections: an SCC larger than the function limit or
// a relevant set larger than the SCC limit is planned as an explicit
// incomplete frontier. The oversized SCC is never split.
StatusOr<QueryPlan> PlanQuery(const QueryDescriptor &descriptor,
                              const DependencySnapshot &snapshot);

} // namespace veritas::query

#endif // VERITAS_QUERY_QUERY_PLANNER_H_
