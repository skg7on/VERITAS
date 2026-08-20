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

#ifndef VERITAS_CPG_CPGQUERY_H_
#define VERITAS_CPG_CPGQUERY_H_

#include <cstddef>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"

namespace veritas::cpg {

class CpgRepository;

// TruncationReason identifies which budget exhausted a traversal.
enum class TruncationReason { kMaxDepth, kMaxNodes, kMaxPaths };

// QueryBudget bounds a traversal. A result that reached a budget exactly is
// complete; a result that would have explored more work is truncated.
struct QueryBudget {
  std::size_t max_depth;
  std::size_t max_nodes;
  std::size_t max_paths;
};

// CpgPath is a sequence of node IDs from source to destination.
struct CpgPath {
  std::vector<core::StableId> nodes;
};

// TraversalResult carries the completed paths, the exact reasons the traversal
// was truncated (empty means complete), and the explored node/path counts.
template <typename T>
struct TraversalResult {
  std::vector<T> items;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t explored_nodes = 0;
  std::size_t explored_paths = 0;
};

// CpgQuery is bound to one immutable ProjectionID. Adjacency queries return
// direct neighbors; traversal queries run a budgeted BFS and report truncation.
class CpgQuery {
 public:
  static StatusOr<CpgQuery> OpenProjection(const CpgRepository& repository,
                                           core::StableId projection_id);
  static StatusOr<CpgQuery> OpenCurrent(const CpgRepository& repository,
                                        core::StableId revision_id,
                                        core::StableId build_variant_id);

  core::StableId projection_id() const { return projection_id_; }

  StatusOr<std::vector<CpgNode>> GetCallees(
      core::StableId function_variant_id) const;
  StatusOr<std::vector<CpgNode>> GetCallers(
      core::StableId function_variant_id) const;
  StatusOr<std::vector<CpgNode>> GetWriters(
      core::StableId memory_object_id) const;

  StatusOr<TraversalResult<CpgPath>> GetValueFlow(
      core::StableId src, core::StableId dst, QueryBudget budget) const;
  StatusOr<TraversalResult<CpgPath>> GetCallPaths(
      core::StableId src, core::StableId dst, QueryBudget budget) const;

 private:
  CpgQuery(core::StableId projection_id, std::vector<CpgNode> nodes,
           std::vector<CpgEdge> edges);

  const CpgNode* FindNode(core::StableId node_id) const;
  StatusOr<TraversalResult<CpgPath>> Traverse(
      core::StableId src, core::StableId dst, QueryBudget budget,
      bool call_edges) const;

  core::StableId projection_id_;
  std::vector<CpgNode> nodes_;
  std::vector<CpgEdge> edges_;
};

}  // namespace veritas::cpg

#endif  // VERITAS_CPG_CPGQUERY_H_
