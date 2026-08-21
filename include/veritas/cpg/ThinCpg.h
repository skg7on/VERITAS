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

#ifndef VERITAS_CPG_THINCPG_H_
#define VERITAS_CPG_THINCPG_H_

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"

namespace veritas::cpg {

// ThinCpg is the validated in-memory Code Property Graph built by the M6
// projection stage. It owns the nodes and edges plus the projection metadata,
// and enforces stable-ID idempotency: a duplicate ID with identical content is
// a no-op, while a duplicate ID with different content is a fatal consistency
// error.
class ThinCpg {
 public:
  void SetMetadata(ProjectionMetadata metadata);
  const ProjectionMetadata& metadata() const { return metadata_; }

  Status AddNode(CpgNode node);
  Status AddEdge(CpgEdge edge);
  Status Validate() const;

  std::span<const CpgNode> nodes() const { return nodes_; }
  std::span<const CpgEdge> edges() const { return edges_; }

  bool HasNode(const core::StableId& node_id) const;
  bool HasEdge(const core::StableId& edge_id) const;

 private:
  ProjectionMetadata metadata_;
  std::vector<CpgNode> nodes_;
  std::vector<CpgEdge> edges_;
  std::unordered_map<std::string, size_t> node_positions_;
  std::unordered_map<std::string, size_t> edge_positions_;
};

}  // namespace veritas::cpg

#endif  // VERITAS_CPG_THINCPG_H_
