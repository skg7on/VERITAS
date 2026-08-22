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

#include "veritas/wpa/SccGraph.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace veritas::wpa {
namespace {

void AppendField(std::string *output, std::string_view value) {
  output->append(std::to_string(value.size()));
  output->push_back(':');
  output->append(value);
}

core::StableId MakeSccId(std::span<const core::StableId> members) {
  std::string canonical;
  AppendField(&canonical, "veritas.scc.v1");
  for (const auto &member : members) {
    AppendField(&canonical, core::ToString(member));
  }
  return core::MakeStableId(
      core::IdKind::kScc,
      std::as_bytes(std::span(canonical.data(), canonical.size())));
}

void SortAndDeduplicate(std::vector<core::StableId> *ids) {
  std::ranges::sort(*ids);
  ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
}

} // namespace

StatusOr<SccGraph> SccGraph::Build(const CallGraph &call_graph) {
  SccGraph result;
  std::map<core::StableId, std::size_t> index;
  std::map<core::StableId, std::size_t> lowlink;
  std::vector<core::StableId> stack;
  std::set<core::StableId> on_stack;
  std::size_t next_index = 0;

  struct DfsFrame {
    core::StableId vertex;
    std::vector<core::StableId> callees;
    std::size_t next_callee = 0;
  };
  std::vector<DfsFrame> dfs;
  auto push_vertex = [&](const core::StableId &vertex) {
    index[vertex] = next_index;
    lowlink[vertex] = next_index++;
    stack.push_back(vertex);
    on_stack.insert(vertex);

    std::vector<core::StableId> callees;
    for (const auto &edge : call_graph.Outgoing(vertex))
      callees.push_back(edge.callee);
    SortAndDeduplicate(&callees);
    dfs.push_back(
        {.vertex = vertex, .callees = std::move(callees), .next_callee = 0});
  };

  for (const auto &function : call_graph.Functions()) {
    if (index.contains(function))
      continue;
    push_vertex(function);
    while (!dfs.empty()) {
      auto &frame = dfs.back();
      if (frame.next_callee < frame.callees.size()) {
        const core::StableId callee = frame.callees[frame.next_callee++];
        if (!index.contains(callee)) {
          push_vertex(callee);
        } else if (on_stack.contains(callee)) {
          lowlink[frame.vertex] =
              std::min(lowlink[frame.vertex], index[callee]);
        }
        continue;
      }

      const core::StableId vertex = frame.vertex;
      if (lowlink[vertex] == index[vertex]) {
        std::vector<core::StableId> members;
        for (;;) {
          const core::StableId member = stack.back();
          stack.pop_back();
          on_stack.erase(member);
          members.push_back(member);
          if (member == vertex)
            break;
        }
        std::ranges::sort(members);
        const core::StableId scc_id = MakeSccId(members);
        result.members_.emplace(scc_id, members);
        for (const auto &member : members)
          result.function_to_scc_.emplace(member, scc_id);
      }

      dfs.pop_back();
      if (!dfs.empty()) {
        const auto &parent = dfs.back().vertex;
        lowlink[parent] = std::min(lowlink[parent], lowlink[vertex]);
      }
    }
  }

  for (const auto &[scc_id, members] : result.members_) {
    static_cast<void>(members);
    result.predecessors_.emplace(scc_id, std::vector<core::StableId>{});
    result.successors_.emplace(scc_id, std::vector<core::StableId>{});
  }
  for (const auto &caller : call_graph.Functions()) {
    auto caller_it = result.function_to_scc_.find(caller);
    if (caller_it == result.function_to_scc_.end()) {
      return Status::Internal("call-graph caller has no SCC mapping");
    }
    const core::StableId caller_scc = caller_it->second;
    for (const auto &edge : call_graph.Outgoing(caller)) {
      auto callee_it = result.function_to_scc_.find(edge.callee);
      if (callee_it == result.function_to_scc_.end()) {
        return Status::Internal("call-graph callee has no SCC mapping");
      }
      const core::StableId callee_scc = callee_it->second;
      if (caller_scc == callee_scc)
        continue;
      result.successors_[caller_scc].push_back(callee_scc);
      result.predecessors_[callee_scc].push_back(caller_scc);
    }
  }
  for (auto &[scc_id, successors] : result.successors_) {
    static_cast<void>(scc_id);
    SortAndDeduplicate(&successors);
  }
  for (auto &[scc_id, predecessors] : result.predecessors_) {
    static_cast<void>(scc_id);
    SortAndDeduplicate(&predecessors);
  }

  std::map<core::StableId, std::size_t> remaining_successors;
  std::priority_queue<core::StableId, std::vector<core::StableId>,
                      std::greater<>>
      ready;
  for (const auto &[scc_id, successors] : result.successors_) {
    remaining_successors.emplace(scc_id, successors.size());
    if (successors.empty())
      ready.push(scc_id);
  }
  while (!ready.empty()) {
    const core::StableId current = ready.top();
    ready.pop();
    result.reverse_topological_order_.push_back(current);
    auto predecessor_it = result.predecessors_.find(current);
    if (predecessor_it == result.predecessors_.end()) {
      return Status::Internal("SCC is missing its predecessor list");
    }
    for (const auto &predecessor : predecessor_it->second) {
      auto remaining_it = remaining_successors.find(predecessor);
      if (remaining_it == remaining_successors.end()) {
        return Status::Internal("SCC predecessor has no edge count");
      }
      auto &remaining = remaining_it->second;
      if (remaining == 0u) {
        return Status::Internal("invalid SCC condensation edge count");
      }
      --remaining;
      if (remaining == 0u)
        ready.push(predecessor);
    }
  }
  if (result.reverse_topological_order_.size() != result.members_.size()) {
    return Status::Internal("SCC condensation graph contains a cycle");
  }
  return result;
}

StatusOr<core::StableId>
SccGraph::SccForFunction(core::StableId function_variant_id) const {
  auto it = function_to_scc_.find(function_variant_id);
  if (it == function_to_scc_.end()) {
    return Status::NotFound("function is not present in the SCC graph");
  }
  return it->second;
}

StatusOr<std::span<const core::StableId>>
SccGraph::Members(core::StableId scc_id) const {
  auto it = members_.find(scc_id);
  if (it == members_.end()) {
    return Status::NotFound("SCC is not present in the graph");
  }
  return std::span<const core::StableId>(it->second);
}

StatusOr<std::span<const core::StableId>>
SccGraph::Predecessors(core::StableId scc_id) const {
  auto it = predecessors_.find(scc_id);
  if (it == predecessors_.end()) {
    return Status::NotFound("SCC is not present in the graph");
  }
  return std::span<const core::StableId>(it->second);
}

StatusOr<std::span<const core::StableId>>
SccGraph::Successors(core::StableId scc_id) const {
  auto it = successors_.find(scc_id);
  if (it == successors_.end()) {
    return Status::NotFound("SCC is not present in the graph");
  }
  return std::span<const core::StableId>(it->second);
}

} // namespace veritas::wpa
