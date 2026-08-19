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

#include "veritas/cpg/CpgQuery.h"

#include <functional>
#include <set>
#include <string>
#include <utility>

#include "veritas/cpg/CpgRepository.h"

namespace veritas::cpg {

CpgQuery::CpgQuery(core::StableId projection_id, std::vector<CpgNode> nodes,
                   std::vector<CpgEdge> edges)
    : projection_id_(projection_id),
      nodes_(std::move(nodes)),
      edges_(std::move(edges)) {}

StatusOr<CpgQuery> CpgQuery::OpenProjection(const CpgRepository& repository,
                                            core::StableId projection_id) {
  auto graph = repository.LoadProjection(projection_id);
  if (!graph.ok()) return graph.status();
  std::vector<CpgNode> nodes(graph->nodes().begin(), graph->nodes().end());
  std::vector<CpgEdge> edges(graph->edges().begin(), graph->edges().end());
  return CpgQuery(projection_id, std::move(nodes), std::move(edges));
}

StatusOr<CpgQuery> CpgQuery::OpenCurrent(const CpgRepository& repository,
                                         core::StableId revision_id,
                                         core::StableId build_variant_id) {
  auto projection_id = repository.OpenCurrent(revision_id, build_variant_id);
  if (!projection_id.ok()) return projection_id.status();
  return OpenProjection(repository, *projection_id);
}

const CpgNode* CpgQuery::FindNode(core::StableId node_id) const {
  for (const auto& node : nodes_) {
    if (node.node_id == node_id) return &node;
  }
  return nullptr;
}

StatusOr<std::vector<CpgNode>> CpgQuery::GetCallees(
    core::StableId function_variant_id) const {
  std::vector<CpgNode> result;
  for (const auto& edge : edges_) {
    if (edge.source_node_id != function_variant_id) continue;
    if (edge.kind != EdgeKind::kCalls && edge.kind != EdgeKind::kMayCall) {
      continue;
    }
    if (const auto* node = FindNode(edge.target_node_id)) {
      result.push_back(*node);
    }
  }
  return result;
}

StatusOr<std::vector<CpgNode>> CpgQuery::GetCallers(
    core::StableId function_variant_id) const {
  std::vector<CpgNode> result;
  for (const auto& edge : edges_) {
    if (edge.target_node_id != function_variant_id) continue;
    if (edge.kind != EdgeKind::kCalls && edge.kind != EdgeKind::kMayCall) {
      continue;
    }
    if (const auto* node = FindNode(edge.source_node_id)) {
      result.push_back(*node);
    }
  }
  return result;
}

StatusOr<std::vector<CpgNode>> CpgQuery::GetWriters(
    core::StableId memory_object_id) const {
  std::vector<CpgNode> result;
  for (const auto& edge : edges_) {
    if (edge.target_node_id != memory_object_id) continue;
    if (edge.kind != EdgeKind::kWrites) continue;
    if (const auto* node = FindNode(edge.source_node_id)) {
      result.push_back(*node);
    }
  }
  return result;
}

StatusOr<TraversalResult<CpgPath>> CpgQuery::Traverse(
    core::StableId src, core::StableId dst, QueryBudget budget,
    bool call_edges) const {
  TraversalResult<CpgPath> result;
  std::set<std::string> explored;
  std::set<TruncationReason> reasons;

  std::function<void(const core::StableId&, CpgPath, std::size_t)> dfs =
      [&](const core::StableId& node, CpgPath path, std::size_t depth) {
        path.nodes.push_back(node);
        explored.insert(core::ToString(node));
        if (node == dst) {
          if (result.items.size() >= budget.max_paths) {
            reasons.insert(TruncationReason::kMaxPaths);
            return;
          }
          result.items.push_back(std::move(path));
          ++result.explored_paths;
          return;
        }

        for (const auto& edge : edges_) {
          if (edge.source_node_id != node) continue;
          const bool relevant =
              call_edges
                  ? (edge.kind == EdgeKind::kCalls || edge.kind == EdgeKind::kMayCall)
                  : (edge.kind == EdgeKind::kFlowsTo);
          if (!relevant) continue;

          const std::size_t next_depth = depth + 1;
          if (next_depth > budget.max_depth) {
            reasons.insert(TruncationReason::kMaxDepth);
            continue;
          }
          const std::string key = core::ToString(edge.target_node_id);
          const bool is_new = (explored.find(key) == explored.end());
          if (is_new && explored.size() >= budget.max_nodes) {
            reasons.insert(TruncationReason::kMaxNodes);
            continue;
          }
          dfs(edge.target_node_id, path, next_depth);
        }
      };

  dfs(src, CpgPath{}, 0);
  result.explored_nodes = explored.size();
  result.truncation_reasons.assign(reasons.begin(), reasons.end());
  return result;
}

StatusOr<TraversalResult<CpgPath>> CpgQuery::GetValueFlow(
    core::StableId src, core::StableId dst, QueryBudget budget) const {
  return Traverse(src, dst, budget, /*call_edges=*/false);
}

StatusOr<TraversalResult<CpgPath>> CpgQuery::GetCallPaths(
    core::StableId src, core::StableId dst, QueryBudget budget) const {
  return Traverse(src, dst, budget, /*call_edges=*/true);
}

}  // namespace veritas::cpg
