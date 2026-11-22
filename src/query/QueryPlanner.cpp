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

#include "veritas/query/QueryPlanner.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::query {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;

using core::StableId;

void AddFrontier(std::vector<QueryFrontierEntry> *frontier,
                 QueryFrontierKind kind, std::optional<StableId> ref,
                 std::string detail) {
  frontier->push_back(
      QueryFrontierEntry{kind, std::move(ref), std::move(detail)});
}

std::set<StableId>
Reachable(const std::map<StableId, std::vector<StableId>> &adjacency,
          const std::set<StableId> &starts) {
  std::set<StableId> seen = starts;
  std::deque<StableId> queue(starts.begin(), starts.end());
  while (!queue.empty()) {
    const StableId current = queue.front();
    queue.pop_front();
    const auto found = adjacency.find(current);
    if (found == adjacency.end()) {
      continue;
    }
    for (const auto &next : found->second) {
      if (seen.insert(next).second) {
        queue.push_back(next);
      }
    }
  }
  return seen;
}

std::set<StableId> Union(const std::set<StableId> &a,
                         const std::set<StableId> &b) {
  std::set<StableId> result = a;
  result.insert(b.begin(), b.end());
  return result;
}

std::set<StableId> Intersect(const std::set<StableId> &a,
                             const std::set<StableId> &b) {
  std::set<StableId> result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(result, result.end()));
  return result;
}

std::set<StableId> Anchors(const QueryDescriptor &descriptor) {
  std::set<StableId> anchors;
  for (const auto *refs :
       {&descriptor.sources, &descriptor.sinks, &descriptor.subjects}) {
    for (const auto &ref : *refs) {
      anchors.insert(ref.enclosing_function);
    }
  }
  return anchors;
}

// A boundary summary is the only thing allowed to substitute for an
// unadmitted successor or to close an unresolved call site.
const BoundarySummary *
CoveringBoundary(const std::vector<const BoundarySummary *> &candidates,
                 QueryPredicate predicate,
                 const std::optional<StableId> &call_site) {
  for (const auto *boundary : candidates) {
    if (!BoundarySupports(*boundary, predicate)) {
      continue;
    }
    if (call_site.has_value() &&
        std::find(boundary->covered_call_sites.begin(),
                  boundary->covered_call_sites.end(),
                  *call_site) == boundary->covered_call_sites.end()) {
      continue;
    }
    return boundary;
  }
  return nullptr;
}

struct OpenCall {
  StableId call_site;
  bool external = false;
  std::string detail;
};

// Collects every call whose target set is not a closed direct edge. A resolved
// indirect/callback target is still an open target set: the summary names one
// candidate, not every callee the program can dispatch to.
StatusOr<std::vector<OpenCall>>
InspectCalls(const summary::SummaryArtifact &artifact,
             const std::set<StableId> &functions) {
  std::vector<OpenCall> open;
  auto parse_callsite = [](std::string_view text) -> StatusOr<StableId> {
    auto parsed = core::ParseStableId(text);
    if (!parsed.ok() || parsed->kind != core::IdKind::kCallSite) {
      return Status::InvalidArgument(
          "summary call has an invalid call-site id");
    }
    return *parsed;
  };
  auto resolved_target_is_known = [&functions](std::string_view text) {
    auto parsed = core::ParseStableId(text);
    return parsed.ok() && parsed->kind == core::IdKind::kFunctionVariant &&
           functions.contains(*parsed);
  };

  if (const auto *current = std::get_if<v2::FunctionSummary>(&artifact)) {
    for (const auto &call : current->calls()) {
      auto site = parse_callsite(call.call_site_id());
      if (!site.ok()) {
        return site.status();
      }
      bool is_open = false;
      bool is_external = false;
      switch (call.dispatch()) {
      case v2::DISPATCH_KIND_DIRECT:
        is_open = !resolved_target_is_known(
            call.resolved_callee_function_variant_id());
        break;
      case v2::DISPATCH_KIND_EXTERNAL:
        is_open = true;
        is_external = true;
        break;
      case v2::DISPATCH_KIND_INDIRECT:
      case v2::DISPATCH_KIND_VIRTUAL:
      case v2::DISPATCH_KIND_CALLBACK:
      case v2::DISPATCH_KIND_UNKNOWN:
      case v2::DISPATCH_KIND_UNSPECIFIED:
        is_open = true;
        break;
      default:
        // A future or sentinel dispatch kind is an open target set, never a
        // closed direct edge.
        is_open = true;
        break;
      }
      if (is_open) {
        open.push_back(OpenCall{
            *site, is_external,
            "dispatch " + std::to_string(static_cast<int>(call.dispatch()))});
      }
    }
  } else {
    const auto &legacy = std::get<v1::FunctionSummary>(artifact);
    for (const auto &call : legacy.calls()) {
      auto site = parse_callsite(call.call_site_anchor_id());
      if (!site.ok()) {
        return site.status();
      }
      if (!resolved_target_is_known(
              call.resolved_callee_function_variant_id())) {
        open.push_back(OpenCall{*site, false, "unresolved v1 direct call"});
      }
    }
  }
  std::sort(open.begin(), open.end(), [](const OpenCall &a, const OpenCall &b) {
    if (a.call_site != b.call_site) {
      return a.call_site < b.call_site;
    }
    return a.detail < b.detail;
  });
  open.erase(std::unique(open.begin(), open.end(),
                         [](const OpenCall &a, const OpenCall &b) {
                           return a.call_site == b.call_site &&
                                  a.detail == b.detail;
                         }),
             open.end());
  return open;
}

} // namespace

StatusOr<QueryPlan> PlanQuery(const QueryDescriptor &descriptor,
                              const DependencySnapshot &snapshot) {
  Status descriptor_valid = ValidateQueryDescriptor(descriptor);
  if (!descriptor_valid.ok()) {
    return descriptor_valid;
  }
  auto query_id = ComputeQueryId(descriptor);
  if (!query_id.ok()) {
    return query_id.status();
  }
  Status snapshot_valid = ValidateDependencySnapshot(snapshot);
  if (!snapshot_valid.ok()) {
    return snapshot_valid;
  }
  Status producers =
      ValidateProducers(descriptor.producers, snapshot.semantics);
  if (!producers.ok()) {
    return producers;
  }

  const QueryPredicate predicate = descriptor.predicate;

  QueryPlan plan{.query_id = *query_id};
  plan.descriptor = descriptor;
  plan.snapshot_digest = ComputeDependencySnapshotDigest(snapshot);
  plan.revision_id = snapshot.revision_id;
  plan.semantics = snapshot.semantics;
  plan.component = PredicateComponent(predicate);
  plan.relation = PredicateRelation(predicate);
  plan.local_relations = PredicateLocalRelations(predicate);

  // Resolve the descriptor's admitted boundary summaries in descriptor order.
  std::map<std::string, const BoundarySummary *> boundaries_by_id;
  for (const auto &boundary : snapshot.boundary_summaries) {
    auto id = ComputeBoundarySummaryId(boundary);
    if (!id.ok()) {
      return id.status();
    }
    boundaries_by_id.emplace(id->value(), &boundary);
  }
  std::vector<std::string> seen_boundary_ids;
  for (const auto &id : descriptor.boundary_summaries) {
    const auto found = boundaries_by_id.find(id.value());
    if (found == boundaries_by_id.end()) {
      return Status::NotFound(
          "query boundary summary is not present in the dependency snapshot");
    }
    if (std::find(seen_boundary_ids.begin(), seen_boundary_ids.end(),
                  id.value()) != seen_boundary_ids.end()) {
      return Status::InvalidArgument("duplicate admitted boundary summary");
    }
    seen_boundary_ids.push_back(id.value());
    plan.boundary_summaries.push_back(*found->second);
  }
  std::map<StableId, std::vector<const BoundarySummary *>>
      boundaries_by_function;
  for (const auto &boundary : plan.boundary_summaries) {
    boundaries_by_function[boundary.function_variant_id].push_back(&boundary);
  }

  // The program graph and its whole-program SCC decomposition.
  auto call_graph = wpa::CallGraph::FromSummaries(snapshot.summaries);
  if (!call_graph.ok()) {
    return call_graph.status();
  }
  auto scc_graph = wpa::SccGraph::Build(*call_graph);
  if (!scc_graph.ok()) {
    return scc_graph.status();
  }
  const std::set<StableId> functions(call_graph->Functions().begin(),
                                     call_graph->Functions().end());

  // Every function anchor must exist in this snapshot; otherwise the query is
  // about a different program and cannot be silently answered as empty.
  for (const auto *refs :
       {&descriptor.sources, &descriptor.sinks, &descriptor.subjects}) {
    for (const auto &ref : *refs) {
      if (!functions.contains(ref.enclosing_function)) {
        return Status::NotFound(
            "query endpoint function is not in the dependency snapshot");
      }
    }
  }

  std::map<StableId, std::vector<StableId>> forward;
  std::map<StableId, std::vector<StableId>> backward;
  for (const auto &caller : call_graph->Functions()) {
    for (const auto &edge : call_graph->Outgoing(caller)) {
      forward[caller].push_back(edge.callee);
      backward[edge.callee].push_back(caller);
    }
  }
  for (auto *adjacency : {&forward, &backward}) {
    for (auto &[node, neighbours] : *adjacency) {
      std::sort(neighbours.begin(), neighbours.end());
      neighbours.erase(std::unique(neighbours.begin(), neighbours.end()),
                       neighbours.end());
    }
  }

  std::set<StableId> source_functions;
  for (const auto &ref : descriptor.sources) {
    source_functions.insert(ref.enclosing_function);
  }
  std::set<StableId> sink_functions;
  for (const auto &ref : descriptor.sinks) {
    sink_functions.insert(ref.enclosing_function);
  }

  std::set<StableId> relevant;
  switch (predicate) {
  case QueryPredicate::kReachableCall: {
    const auto forward_set = Reachable(forward, source_functions);
    if (sink_functions.empty()) {
      relevant = forward_set;
    } else {
      relevant = Intersect(forward_set, Reachable(backward, sink_functions));
    }
    break;
  }
  case QueryPredicate::kGlobalFlow: {
    // Value flow crosses parameter edges downward and return edges upward, so
    // conservative reachability over the call graph is undirected.
    std::map<StableId, std::vector<StableId>> undirected = forward;
    for (const auto &[node, neighbours] : backward) {
      auto &target = undirected[node];
      target.insert(target.end(), neighbours.begin(), neighbours.end());
      std::sort(target.begin(), target.end());
      target.erase(std::unique(target.begin(), target.end()), target.end());
    }
    const auto from_source = Reachable(undirected, source_functions);
    if (sink_functions.empty()) {
      relevant = from_source;
    } else {
      relevant = Intersect(from_source, Reachable(undirected, sink_functions));
    }
    break;
  }
  case QueryPredicate::kMayRead:
  case QueryPredicate::kMayWrite:
  case QueryPredicate::kUnknownEffect:
    // Memory/effect summaries propagate from callees to callers; the callers
    // are computed later from the successor support this set supplies.
    relevant = Reachable(forward, source_functions);
    break;
  }
  relevant = Union(relevant, Anchors(descriptor));

  // Map to whole SCCs. Never split a recursive SCC.
  std::set<StableId> relevant_scc_ids;
  for (const auto &function : relevant) {
    auto scc_id = scc_graph->SccForFunction(function);
    if (!scc_id.ok()) {
      return scc_id.status();
    }
    relevant_scc_ids.insert(*scc_id);
  }

  std::vector<StableId> ordered_relevant;
  for (const auto &scc_id : scc_graph->ReverseTopologicalOrder()) {
    if (relevant_scc_ids.contains(scc_id)) {
      ordered_relevant.push_back(scc_id);
    }
  }

  // A summary index for the open-target inspection below.
  std::map<StableId, const summary::SummaryArtifact *> summary_by_function;
  for (const auto &artifact : snapshot.summaries) {
    auto parsed =
        core::ParseStableId(summary::Identity(artifact).function_variant_id());
    if (!parsed.ok()) {
      return Status::InvalidArgument(
          "dependency snapshot summary has an invalid function identity");
    }
    summary_by_function.emplace(*parsed, &artifact);
  }

  auto make_planned = [&](const StableId &scc_id) -> StatusOr<PlannedScc> {
    PlannedScc planned;
    planned.scc_id = scc_id;
    auto members = scc_graph->Members(scc_id);
    if (!members.ok()) {
      return members.status();
    }
    planned.members.assign(members->begin(), members->end());
    auto successors = scc_graph->Successors(scc_id);
    if (!successors.ok()) {
      return successors.status();
    }
    planned.successors.assign(successors->begin(), successors->end());
    std::sort(planned.members.begin(), planned.members.end());
    std::sort(planned.successors.begin(), planned.successors.end());
    return planned;
  };

  // Admission: reverse-topological (successors first), then the function limit
  // per whole SCC, then the SCC-count limit. Over-budget SCCs are excluded
  // whole and named in the frontier.
  std::vector<StableId> admitted;
  for (const auto &scc_id : ordered_relevant) {
    auto planned = make_planned(scc_id);
    if (!planned.ok()) {
      return planned.status();
    }
    if (planned->members.size() > descriptor.limits.max_functions) {
      plan.excluded_sccs.push_back(std::move(*planned));
      AddFrontier(&plan.frontier, QueryFrontierKind::kOversizedScc, scc_id,
                  "recursive SCC has " +
                      std::to_string(plan.excluded_sccs.back().members.size()) +
                      " functions (limit " +
                      std::to_string(descriptor.limits.max_functions) +
                      "); it is never split");
      continue;
    }
    admitted.push_back(scc_id);
  }
  if (admitted.size() > descriptor.limits.max_sccs) {
    for (std::size_t i = descriptor.limits.max_sccs; i < admitted.size(); ++i) {
      auto planned = make_planned(admitted[i]);
      if (!planned.ok()) {
        return planned.status();
      }
      plan.excluded_sccs.push_back(std::move(*planned));
      AddFrontier(&plan.frontier, QueryFrontierKind::kSccBudget, admitted[i],
                  "relevant SCC exceeds the admitted SCC limit " +
                      std::to_string(descriptor.limits.max_sccs));
    }
    admitted.resize(descriptor.limits.max_sccs);
  }

  std::set<StableId> admitted_ids;
  for (const auto &scc_id : admitted) {
    auto planned = make_planned(scc_id);
    if (!planned.ok()) {
      return planned.status();
    }
    admitted_ids.insert(scc_id);
    plan.sccs.push_back(std::move(*planned));
  }
  std::set<StableId> excluded_ids;
  for (const auto &planned : plan.excluded_sccs) {
    excluded_ids.insert(planned.scc_id);
    for (const auto &member : planned.members) {
      plan.relevant_functions.push_back(member);
    }
  }
  for (const auto &planned : plan.sccs) {
    for (const auto &member : planned.members) {
      plan.relevant_functions.push_back(member);
    }
  }
  std::sort(plan.relevant_functions.begin(), plan.relevant_functions.end());
  plan.relevant_functions.erase(std::unique(plan.relevant_functions.begin(),
                                            plan.relevant_functions.end()),
                                plan.relevant_functions.end());

  // Boundaries and unresolved targets. A successor is either admitted, or
  // safely irrelevant for a path-local predicate, or it opens the frontier.
  for (const auto &scc : plan.sccs) {
    for (const auto &successor : scc.successors) {
      if (admitted_ids.contains(successor)) {
        continue;
      }
      const bool relevant_successor = relevant_scc_ids.contains(successor);
      bool covered = false;
      for (const auto &member : plan.excluded_sccs) {
        if (member.scc_id != successor) {
          continue;
        }
        for (const auto &function : member.members) {
          const auto found = boundaries_by_function.find(function);
          if (found != boundaries_by_function.end() &&
              CoveringBoundary(found->second, predicate, std::nullopt) !=
                  nullptr) {
            covered = true;
            break;
          }
        }
      }
      if (covered) {
        continue;
      }
      if (!relevant_successor && PredicateIsPathLocal(predicate)) {
        // A callee that cannot reach the sink cannot contribute to a
        // reachability/value-flow fact between the query endpoints.
        continue;
      }
      AddFrontier(&plan.frontier, QueryFrontierKind::kMissingBoundary,
                  successor,
                  "successor SCC is neither admitted nor covered by a "
                  "boundary summary");
    }

    // Inspect the admitted members' own summaries for open target sets and
    // explicit unknowns.
    for (const auto &function : scc.members) {
      const auto summary_it = summary_by_function.find(function);
      if (summary_it == summary_by_function.end()) {
        return Status::Internal("planned SCC member has no summary");
      }
      const summary::SummaryArtifact *artifact = summary_it->second;
      auto open_calls = InspectCalls(*artifact, functions);
      if (!open_calls.ok()) {
        return open_calls.status();
      }
      const std::vector<const BoundarySummary *> no_boundaries;
      const auto boundary_it = boundaries_by_function.find(function);
      const auto &boundaries = boundary_it == boundaries_by_function.end()
                                   ? no_boundaries
                                   : boundary_it->second;
      for (const auto &call : *open_calls) {
        if (CoveringBoundary(boundaries, predicate, call.call_site) !=
            nullptr) {
          continue;
        }
        AddFrontier(&plan.frontier,
                    call.external ? QueryFrontierKind::kExternalBoundary
                                  : QueryFrontierKind::kUnresolvedCall,
                    call.call_site, call.detail);
      }
      const auto *v2_summary = std::get_if<v2::FunctionSummary>(artifact);
      const auto *v1_summary = std::get_if<v1::FunctionSummary>(artifact);
      std::size_t unknown_count = 0;
      if (v2_summary != nullptr) {
        unknown_count = static_cast<std::size_t>(v2_summary->unknowns().size());
      } else if (v1_summary != nullptr) {
        unknown_count = static_cast<std::size_t>(v1_summary->unknowns().size());
      }
      if (unknown_count > 0 &&
          CoveringBoundary(boundaries, predicate, std::nullopt) == nullptr) {
        AddFrontier(&plan.frontier, QueryFrontierKind::kUnknownEffect, function,
                    std::to_string(unknown_count) +
                        " explicit unknown(s) keep the scope open");
      }
      // Shared or escaped memory is open unless a versioned boundary contract
      // covers it. An alias fact that is not an explicit no-alias claim is the
      // available conservative signal.
      if (v2_summary != nullptr && PredicateUsesMemory(predicate)) {
        std::size_t shared_aliases = 0;
        for (const auto &alias : v2_summary->alias_facts()) {
          if (alias.kind() != v2::ALIAS_KIND_NO_ALIAS) {
            ++shared_aliases;
          }
        }
        if (shared_aliases > 0 &&
            CoveringBoundary(boundaries, predicate, std::nullopt) == nullptr) {
          AddFrontier(&plan.frontier, QueryFrontierKind::kSharedMemory,
                      function,
                      std::to_string(shared_aliases) +
                          " alias fact(s) may share memory");
        }
      }
    }
  }

  CanonicalizeQueryFrontier(&plan.frontier);
  plan.scope_closed = plan.frontier.empty();
  return plan;
}

} // namespace veritas::query
