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
#include <map>
#include <set>
#include <string>
#include <utility>

#include "veritas/core/Hash.h"
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

Status ThinCpg::AddCallSiteBinding(CallSiteBinding binding) {
  std::sort(binding.arguments.begin(), binding.arguments.end(),
            [](const auto &a, const auto &b) { return a.index < b.index; });
  const auto valid = [](const core::StableId &id, core::IdKind kind) {
    return id.kind == kind && core::HexToDigest(id.digest_hex).has_value();
  };
  if (!valid(binding.callsite_id, core::IdKind::kCallSite) ||
      !valid(binding.function_variant_id, core::IdKind::kFunctionVariant) ||
      !(valid(binding.callee_ref, core::IdKind::kFunctionVariant) ||
        valid(binding.callee_ref, core::IdKind::kUnknownNode)) ||
      (binding.result_ref &&
       !valid(*binding.result_ref, core::IdKind::kValueRef))) {
    return Status::InvalidArgument("invalid callsite binding identity");
  }
  for (std::size_t i = 0; i < binding.arguments.size(); ++i) {
    const auto &arg = binding.arguments[i];
    if (arg.index != i || !valid(arg.value_ref, core::IdKind::kValueRef) ||
        (arg.memory_ref && !valid(*arg.memory_ref, core::IdKind::kMemoryRef))) {
      return Status::InvalidArgument(
          "invalid or noncontiguous callsite argument");
    }
  }
  if (!binding.normalized_operation.empty() &&
      (binding.normalized_operation != "memcpy.v1" ||
       (binding.arguments.size() != 3 && binding.arguments.size() != 4) ||
       !binding.arguments[0].memory_ref || !binding.arguments[1].memory_ref)) {
    return Status::InvalidArgument("unsupported normalized callsite operation");
  }
  const auto key = core::ToString(binding.callsite_id);
  auto it = callsite_positions_.find(key);
  if (it != callsite_positions_.end()) {
    return callsites_[it->second] == binding
               ? Status::Ok()
               : Status::FailedPrecondition("conflicting callsite binding");
  }
  callsite_positions_.emplace(key, callsites_.size());
  callsites_.push_back(std::move(binding));
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
  if (!HasCompilerOwnership() && !callsites_.empty()) {
    return Status::FailedPrecondition(
        "legacy CPG cannot declare compiler call bindings");
  }
  if (HasCompilerOwnership()) {
    for (const auto &node : nodes_) {
      if (node.kind == NodeKind::kCallSite &&
          !callsite_positions_.contains(core::ToString(node.node_id))) {
        return Status::FailedPrecondition(
            "compiler callsite binding is missing");
      }
    }
    std::map<core::StableId, core::StableId> owners;
    std::set<std::pair<core::StableId, core::StableId>> call_targets;
    for (const auto &edge : edges_) {
      if (edge.kind == EdgeKind::kCalls || edge.kind == EdgeKind::kMayCall) {
        call_targets.emplace(edge.source_node_id, edge.target_node_id);
      }
      if (edge.kind != EdgeKind::kContains)
        continue;
      const auto &source =
          nodes_[node_positions_.find(core::ToString(edge.source_node_id))
                     ->second];
      if (source.kind != NodeKind::kFunction) {
        return Status::InvalidArgument(
            "compiler containment requires a function owner");
      }
      auto [it, inserted] =
          owners.emplace(edge.target_node_id, edge.source_node_id);
      if (!inserted && it->second != edge.source_node_id) {
        return Status::FailedPrecondition(
            "conflicting compiler containment owners");
      }
    }
    for (const auto &binding : callsites_) {
      auto owner = owners.find(binding.callsite_id);
      if (owner == owners.end() ||
          owner->second != binding.function_variant_id ||
          !HasNode(binding.callee_ref) ||
          (binding.result_ref && !HasNode(*binding.result_ref))) {
        return Status::FailedPrecondition(
            "callsite binding has missing owner or node");
      }
      const auto &site =
          nodes_[node_positions_.find(core::ToString(binding.callsite_id))
                     ->second];
      if (site.kind != NodeKind::kCallSite) {
        return Status::InvalidArgument(
            "callsite binding references a non-callsite node");
      }
      for (const auto &arg : binding.arguments) {
        if (!HasNode(arg.value_ref) ||
            (arg.memory_ref && !HasNode(*arg.memory_ref))) {
          return Status::FailedPrecondition(
              "callsite argument references a missing node");
        }
        auto arg_owner = owners.find(arg.value_ref);
        if (arg_owner != owners.end() &&
            arg_owner->second != binding.function_variant_id) {
          return Status::FailedPrecondition(
              "callsite argument belongs to another caller");
        }
      }
      if (!call_targets.contains({binding.callsite_id, binding.callee_ref})) {
        return Status::FailedPrecondition("callsite target edge missing");
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
