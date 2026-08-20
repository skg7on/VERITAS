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

#include "veritas/cpg/ThinCpg.h"

#include <algorithm>
#include <string>
#include <utility>

#include "veritas/core/Ids.h"

namespace veritas::cpg {
namespace {

// Sort and deduplicate support records so equivalent input produces identical
// canonical bytes regardless of insertion order.
void CanonicalizeSupport(std::vector<SupportRef>* support) {
  std::sort(support->begin(), support->end());
  support->erase(std::unique(support->begin(), support->end()), support->end());
}

bool SameNodeContent(const CpgNode& a, const CpgNode& b) {
  return a.kind == b.kind && a.label == b.label;
}

bool SameEdgeContent(const CpgEdge& a, const CpgEdge& b) {
  return a.kind == b.kind && a.source_node_id == b.source_node_id &&
         a.target_node_id == b.target_node_id && a.alias_state == b.alias_state &&
         a.expandable == b.expandable && a.support == b.support;
}

}  // namespace

void ThinCpg::SetMetadata(ProjectionMetadata metadata) {
  metadata_ = std::move(metadata);
}

Status ThinCpg::AddNode(CpgNode node) {
  const std::string key = core::ToString(node.node_id);
  auto it = node_positions_.find(key);
  if (it != node_positions_.end()) {
    if (!SameNodeContent(nodes_[it->second], node)) {
      return Status::FailedPrecondition(
          "stable-ID collision with different content: " + key);
    }
    return Status::Ok();  // idempotent
  }
  node_positions_.emplace(key, nodes_.size());
  nodes_.push_back(std::move(node));
  return Status::Ok();
}

Status ThinCpg::AddEdge(CpgEdge edge) {
  CanonicalizeSupport(&edge.support);
  if (edge.kind == EdgeKind::kAliases && !edge.alias_state.has_value()) {
    return Status::InvalidArgument("ALIASES edge requires alias_state");
  }
  if (edge.kind != EdgeKind::kAliases && edge.alias_state.has_value()) {
    return Status::InvalidArgument("alias_state is valid only for ALIASES");
  }

  const std::string key = core::ToString(edge.edge_id);
  auto it = edge_positions_.find(key);
  if (it != edge_positions_.end()) {
    if (!SameEdgeContent(edges_[it->second], edge)) {
      return Status::FailedPrecondition(
          "edge-ID collision with different content: " + key);
    }
    return Status::Ok();  // idempotent
  }
  edge_positions_.emplace(key, edges_.size());
  edges_.push_back(std::move(edge));
  return Status::Ok();
}

Status ThinCpg::Validate() const {
  if (metadata_.revision_id.digest_hex.empty() ||
      metadata_.build_variant_id.digest_hex.empty()) {
    return Status::FailedPrecondition(
        "projection metadata requires revision and build-variant identity");
  }

  for (const auto& edge : edges_) {
    if (!HasNode(edge.source_node_id)) {
      return Status::FailedPrecondition(
          "edge source node missing: " + core::ToString(edge.edge_id));
    }
    if (!HasNode(edge.target_node_id)) {
      return Status::FailedPrecondition(
          "edge target node missing: " + core::ToString(edge.edge_id));
    }
    for (size_t i = 1; i < edge.support.size(); ++i) {
      if (!(edge.support[i - 1] < edge.support[i])) {
        return Status::InvalidArgument("support records are not sorted");
      }
    }
  }
  return Status::Ok();
}

bool ThinCpg::HasNode(const core::StableId& node_id) const {
  return node_positions_.find(core::ToString(node_id)) != node_positions_.end();
}

bool ThinCpg::HasEdge(const core::StableId& edge_id) const {
  return edge_positions_.find(core::ToString(edge_id)) != edge_positions_.end();
}

}  // namespace veritas::cpg
