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

#include "cpg/CpgCanonicalizer.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace veritas::cpg {
namespace {

// Append a length-prefixed field so concatenation is unambiguous.
void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

std::vector<CpgNode> SortedNodes(const ThinCpg& graph) {
  std::vector<CpgNode> nodes(graph.nodes().begin(), graph.nodes().end());
  std::sort(nodes.begin(), nodes.end(), [](const CpgNode& a, const CpgNode& b) {
    return core::ToString(a.node_id) < core::ToString(b.node_id);
  });
  return nodes;
}

std::vector<CpgEdge> SortedEdges(const ThinCpg& graph) {
  std::vector<CpgEdge> edges(graph.edges().begin(), graph.edges().end());
  std::sort(edges.begin(), edges.end(), [](const CpgEdge& a, const CpgEdge& b) {
    return core::ToString(a.edge_id) < core::ToString(b.edge_id);
  });
  return edges;
}

}  // namespace

std::string CpgCanonicalizer::CanonicalBytes(const ThinCpg& graph) {
  const ProjectionMetadata& meta = graph.metadata();
  std::string out;

  AppendField(&out, meta.schema_version);
  AppendField(&out, core::ToString(meta.revision_id));
  AppendField(&out, core::ToString(meta.build_variant_id));
  AppendField(&out, meta.module_hash);

  std::vector<core::StableId> summary_ids = meta.summary_ids;
  std::sort(summary_ids.begin(), summary_ids.end());
  for (const auto& id : summary_ids) {
    AppendField(&out, core::ToString(id));
  }

  for (const auto& node : SortedNodes(graph)) {
    AppendField(&out, core::ToString(node.node_id));
    AppendField(&out, std::to_string(static_cast<int>(node.kind)));
    AppendField(&out, node.label);
  }

  for (const auto& edge : SortedEdges(graph)) {
    AppendField(&out, core::ToString(edge.edge_id));
    AppendField(&out, std::to_string(static_cast<int>(edge.kind)));
    AppendField(&out, core::ToString(edge.source_node_id));
    AppendField(&out, core::ToString(edge.target_node_id));
    AppendField(&out, edge.alias_state
                          ? std::to_string(static_cast<int>(*edge.alias_state))
                          : "");
    AppendField(&out, edge.expandable ? "1" : "0");
    for (const auto& support : edge.support) {
      AppendField(&out, core::ToString(support.function_summary_id));
      AppendField(&out, support.provenance_ref);
    }
  }

  return out;
}

core::StableId CpgCanonicalizer::ProjectionId(const ThinCpg& graph) {
  const std::string bytes = CanonicalBytes(graph);
  return core::MakeStableId(
      core::IdKind::kCpgProjection,
      std::as_bytes(std::span(bytes.data(), bytes.size())));
}

}  // namespace veritas::cpg
