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

#include "veritas/cpg/CpgRepository.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cpg/CpgCanonicalizer.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::cpg {
namespace {

// Join/separate stable IDs with a separator that never appears in a
// serialized ID ("<kind>:sha256:<hex>").
std::string JoinIds(const std::vector<core::StableId>& ids) {
  std::string out;
  for (const auto& id : ids) {
    if (!out.empty()) out.push_back('\n');
    out += core::ToString(id);
  }
  return out;
}

std::vector<core::StableId> SplitIds(const std::string& joined) {
  std::vector<core::StableId> out;
  std::string current;
  for (char c : joined) {
    if (c == '\n') {
      auto id = core::ParseStableId(current);
      if (id.ok()) out.push_back(*id);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    auto id = core::ParseStableId(current);
    if (id.ok()) out.push_back(*id);
  }
  return out;
}

}  // namespace

CpgRepository::CpgRepository(summarydb::MetadataStore& metadata_store)
    : metadata_store_(metadata_store) {}

Status CpgRepository::StageProjection(const ThinCpg& graph) {
  auto validate = graph.Validate();
  if (!validate.ok()) return validate;

  const core::StableId projection_id = CpgCanonicalizer::ProjectionId(graph);
  const std::string pid = core::ToString(projection_id);
  const auto& meta = graph.metadata();

  {
    auto status = metadata_store_.Execute(
        "INSERT OR REPLACE INTO cpg_projections (projection_id, schema_version, "
        "revision_id, build_variant_id, module_hash, summary_ids, canonical_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        {pid, meta.schema_version, core::ToString(meta.revision_id),
         core::ToString(meta.build_variant_id), meta.module_hash,
         JoinIds(meta.summary_ids), CpgCanonicalizer::CanonicalBytes(graph)});
    if (!status.ok()) return status;
  }

  for (const auto& node : graph.nodes()) {
    auto status = metadata_store_.Execute(
        "INSERT OR REPLACE INTO cpg_nodes (projection_id, node_id, node_kind, "
        "node_label) VALUES (?, ?, ?, ?)",
        {pid, core::ToString(node.node_id),
         std::to_string(static_cast<int>(node.kind)), node.label});
    if (!status.ok()) return status;
  }

  for (const auto& edge : graph.edges()) {
    auto status = metadata_store_.Execute(
        "INSERT OR REPLACE INTO cpg_edges (projection_id, edge_id, edge_kind, "
        "source_node_id, target_node_id, alias_state, expandable) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        {pid, core::ToString(edge.edge_id),
         std::to_string(static_cast<int>(edge.kind)),
         core::ToString(edge.source_node_id), core::ToString(edge.target_node_id),
         edge.alias_state ? std::to_string(static_cast<int>(*edge.alias_state))
                          : "-1",
         edge.expandable ? "1" : "0"});
    if (!status.ok()) return status;

    int position = 0;
    for (const auto& support : edge.support) {
      auto support_status = metadata_store_.Execute(
          "INSERT OR REPLACE INTO cpg_edge_support (projection_id, edge_id, "
          "position, function_summary_id, provenance_ref) VALUES (?, ?, ?, ?, ?)",
          {pid, core::ToString(edge.edge_id), std::to_string(position++),
           core::ToString(support.function_summary_id), support.provenance_ref});
      if (!support_status.ok()) return support_status;
    }
  }

  {
    auto status = metadata_store_.Execute(
        "INSERT OR REPLACE INTO current_cpg_projections (revision_id, "
        "build_variant_id, projection_id) VALUES (?, ?, ?)",
        {core::ToString(meta.revision_id), core::ToString(meta.build_variant_id),
         pid});
    if (!status.ok()) return status;
  }

  return Status::Ok();
}

StatusOr<ThinCpg> CpgRepository::LoadProjection(
    const core::StableId& projection_id) const {
  const std::string pid = core::ToString(projection_id);

  auto proj = metadata_store_.Query(
      "SELECT schema_version, revision_id, build_variant_id, module_hash, "
      "summary_ids FROM cpg_projections WHERE projection_id = ?",
      {pid});
  if (!proj.ok()) return proj.status();
  if (proj->empty()) return Status::NotFound("projection not found");
  const auto& row = (*proj)[0];

  auto revision = core::ParseStableId(row[1]);
  auto build = core::ParseStableId(row[2]);
  if (!revision.ok() || !build.ok()) {
    return Status::Internal("invalid projection metadata");
  }

  ThinCpg graph;
  ProjectionMetadata meta;
  meta.schema_version = row[0];
  meta.revision_id = *revision;
  meta.build_variant_id = *build;
  meta.module_hash = row[3];
  meta.summary_ids = SplitIds(row[4]);
  graph.SetMetadata(std::move(meta));

  auto nodes = metadata_store_.Query(
      "SELECT node_id, node_kind, node_label FROM cpg_nodes WHERE projection_id "
      "= ? ORDER BY node_id",
      {pid});
  if (!nodes.ok()) return nodes.status();
  for (const auto& node_row : *nodes) {
    auto node_id = core::ParseStableId(node_row[0]);
    if (!node_id.ok()) return node_id.status();
    auto status = graph.AddNode(
        CpgNode{*node_id, static_cast<NodeKind>(std::stoi(node_row[1])),
                node_row[2]});
    if (!status.ok()) return status;
  }

  // Load support records first so they can be attached to edges.
  std::map<std::string, std::vector<SupportRef>> support_by_edge;
  auto support_rows = metadata_store_.Query(
      "SELECT edge_id, function_summary_id, provenance_ref FROM cpg_edge_support "
      "WHERE projection_id = ? ORDER BY edge_id, position",
      {pid});
  if (!support_rows.ok()) return support_rows.status();
  for (const auto& support_row : *support_rows) {
    auto summary_id = core::ParseStableId(support_row[1]);
    if (!summary_id.ok()) return summary_id.status();
    support_by_edge[support_row[0]].push_back(
        SupportRef{*summary_id, support_row[2]});
  }

  auto edges = metadata_store_.Query(
      "SELECT edge_id, edge_kind, source_node_id, target_node_id, alias_state, "
      "expandable FROM cpg_edges WHERE projection_id = ? ORDER BY edge_id",
      {pid});
  if (!edges.ok()) return edges.status();
  for (const auto& edge_row : *edges) {
    auto edge_id = core::ParseStableId(edge_row[0]);
    auto source = core::ParseStableId(edge_row[2]);
    auto target = core::ParseStableId(edge_row[3]);
    if (!edge_id.ok() || !source.ok() || !target.ok()) {
      return Status::Internal("invalid edge row");
    }
    CpgEdge edge;
    edge.edge_id = *edge_id;
    edge.kind = static_cast<EdgeKind>(std::stoi(edge_row[1]));
    edge.source_node_id = *source;
    edge.target_node_id = *target;
    const int alias_state = std::stoi(edge_row[4]);
    if (alias_state >= 0) {
      edge.alias_state = static_cast<AliasState>(alias_state);
    }
    edge.expandable = (edge_row[5] == "1");
    edge.support = support_by_edge[edge_row[0]];
    auto status = graph.AddEdge(std::move(edge));
    if (!status.ok()) return status;
  }

  return graph;
}

StatusOr<core::StableId> CpgRepository::OpenCurrent(
    const core::StableId& revision_id,
    const core::StableId& build_variant_id) const {
  auto result = metadata_store_.Query(
      "SELECT projection_id FROM current_cpg_projections WHERE revision_id = ? "
      "AND build_variant_id = ?",
      {core::ToString(revision_id), core::ToString(build_variant_id)});
  if (!result.ok()) return result.status();
  if (result->empty()) return Status::NotFound("no current CPG projection");
  return core::ParseStableId((*result)[0][0]);
}

}  // namespace veritas::cpg
