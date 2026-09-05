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

#include "veritas/facts/ProvenanceStore.h"

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "veritas/facts/HexCodec.h"

namespace veritas::facts {

namespace {

namespace fp = fact_proto;

fp::ProducerKind ToProtoProducer(ProducerKind kind) {
  switch (kind) {
    case ProducerKind::kWpaSouffle:
      return fp::PRODUCER_WPA_SOUFFLE;
    case ProducerKind::kWpaCppConformance:
      return fp::PRODUCER_WPA_CPP_CONFORMANCE;
    case ProducerKind::kWpaCppEmergency:
      return fp::PRODUCER_WPA_CPP_EMERGENCY;
    case ProducerKind::kExternal:
      return fp::PRODUCER_EXTERNAL;
  }
  return fp::PRODUCER_UNSPECIFIED;
}

}  // namespace

ProvenanceStore::ProvenanceStore(summarydb::MetadataStore& store)
    : store_(store) {}

Status ProvenanceStore::PutNode(const FactWitness& node) {
  const char* sql =
      "INSERT OR IGNORE INTO provenance_nodes (run_id, output_fact_id,"
      " witness_id, selected, producer_kind, producer_id, rule_id, rule_version,"
      " analyzer_run_id, source_anchor_id, summary_id, description)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  return store_.Execute(
      sql,
      {core::ToString(node.run_id), core::ToString(node.output_fact_id),
       node.witness_id, node.selected ? "1" : "0",
       std::to_string(static_cast<int>(node.producer_kind)), node.producer_id,
       node.rule_id, node.rule_version, node.analyzer_run_id,
       node.source_anchor_id, node.summary_id, node.description});
}

Status ProvenanceStore::PutEdge(const FactWitnessEdge& edge) {
  const char* sql =
      "INSERT OR IGNORE INTO provenance_edges (run_id, output_fact_id,"
      " witness_id, input_kind, input_id, input_ordinal)"
      " VALUES (?, ?, ?, ?, ?, ?)";
  return store_.Execute(
      sql,
      {core::ToString(edge.run_id), core::ToString(edge.output_fact_id),
       edge.witness_id, edge.input_kind, edge.input_id,
       std::to_string(edge.input_ordinal)});
}

StatusOr<fact_proto::ProvenanceGraph> ProvenanceStore::Explain(
    core::StableId run_id, core::StableId fact_id, const ExplainBudget& budget) {
  const std::string run = core::ToString(run_id);
  const std::string target = core::ToString(fact_id);

  fact_proto::ProvenanceGraph graph;
  graph.set_run_id(run);
  graph.set_fact_id(target);

  // The top fact.
  auto fact_rows = store_.Query(
      "SELECT cells_hex FROM analysis_facts WHERE fact_id = ?", {target});
  if (!fact_rows.ok()) {
    return fact_rows.status();
  }
  if (fact_rows->empty()) {
    return Status::NotFound("fact not found");
  }
  auto decoded = HexDecode((*fact_rows)[0][0]);
  if (!decoded.ok()) {
    return decoded.status();
  }
  fact_proto::Fact fact;
  if (!fact.ParseFromString(*decoded)) {
    return Status::Internal("failed to parse stored fact");
  }
  *graph.mutable_fact() = fact;

  // The top fact's binding.
  auto binding_rows = store_.Query(
      "SELECT confidence, producer_kind, analyzer_run_id, scope_kind, scope_id,"
      " selected_witness_id, is_current FROM run_fact_bindings"
      " WHERE run_id = ? AND fact_id = ?",
      {run, target});
  if (!binding_rows.ok()) {
    return binding_rows.status();
  }
  if (binding_rows->empty()) {
    return Status::NotFound("binding not found");
  }
  const auto& br = (*binding_rows)[0];
  auto* binding = graph.mutable_binding();
  binding->set_analysis_run_id(run);
  binding->set_fact_id(target);
  if (!br[0].empty()) {
    binding->set_confidence(std::strtod(br[0].c_str(), nullptr));
  }
  binding->set_producer_kind(ToProtoProducer(
      static_cast<ProducerKind>(std::strtol(br[1].c_str(), nullptr, 10))));
  binding->set_analyzer_run_id(br[2]);
  binding->set_scope_kind(br[3]);
  binding->set_scope_id(br[4]);
  binding->set_selected_witness_id(br[5]);
  binding->set_is_current(br[6] == "1");

  // Breadth-first walk of the witness DAG. Distances are non-decreasing in the
  // queue, so the first distance past max_depth marks every remaining task as
  // out of budget too.
  struct Task {
    std::string fact_id;
    std::uint32_t distance;
  };
  std::vector<Task> queue;
  queue.push_back(Task{target, 0});
  std::set<std::string> enqueued{target};

  std::uint32_t node_count = 0;
  bool truncated = false;
  std::string truncation_reason;

  for (std::size_t i = 0; i < queue.size(); ++i) {
    const Task task = queue[i];
    if (task.distance > budget.max_depth) {
      truncated = true;
      truncation_reason = "max_depth";
      break;
    }

    auto node_rows = store_.Query(
        "SELECT witness_id, selected, producer_kind, producer_id, rule_id,"
        " rule_version, analyzer_run_id, source_anchor_id, summary_id,"
        " description FROM provenance_nodes"
        " WHERE run_id = ? AND output_fact_id = ?",
        {run, task.fact_id});
    if (!node_rows.ok()) {
      return node_rows.status();
    }
    if (node_rows->empty()) {
      continue;  // rooted input: a leaf, no witness to expand
    }

    if (node_count >= budget.max_nodes) {
      truncated = true;
      truncation_reason = "max_nodes";
      break;
    }
    const auto& nr = (*node_rows)[0];
    auto* node = graph.add_nodes();
    node->set_analysis_run_id(run);
    node->set_output_fact_id(task.fact_id);
    node->set_witness_id(nr[0]);
    node->set_selected(nr[1] == "1");
    node->set_producer_kind(ToProtoProducer(
        static_cast<ProducerKind>(std::strtol(nr[2].c_str(), nullptr, 10))));
    node->set_producer_id(nr[3]);
    node->set_rule_id(nr[4]);
    node->set_rule_version(nr[5]);
    node->set_analyzer_run_id(nr[6]);
    node->set_source_anchor_id(nr[7]);
    node->set_summary_id(nr[8]);
    node->set_description(nr[9]);
    ++node_count;

    // Apply the budget's inclusion flags to the node we just emitted.
    if (!budget.include_source_anchors) {
      node->clear_source_anchor_id();
    }
    if (!budget.include_summary_ids) {
      node->clear_summary_id();
    }
    if (!budget.include_datalog_derivation) {
      node->clear_rule_id();
      node->clear_rule_version();
    }

    auto edge_rows = store_.Query(
        "SELECT witness_id, input_kind, input_id, input_ordinal"
        " FROM provenance_edges WHERE run_id = ? AND output_fact_id = ?"
        " ORDER BY input_ordinal",
        {run, task.fact_id});
    if (!edge_rows.ok()) {
      return edge_rows.status();
    }
    for (const auto& er : *edge_rows) {
      auto* edge = graph.add_edges();
      edge->set_analysis_run_id(run);
      edge->set_output_fact_id(task.fact_id);
      edge->set_witness_id(er[0]);
      edge->set_input_kind(er[1]);
      edge->set_input_id(er[2]);
      edge->set_input_ordinal(
          static_cast<std::uint32_t>(std::strtol(er[3].c_str(), nullptr, 10)));

      if (er[1] == "derived" && enqueued.insert(er[2]).second) {
        queue.push_back(Task{er[2], task.distance + 1});
      }
    }
  }

  graph.set_truncated(truncated);
  graph.set_truncation_reason(truncation_reason);
  return graph;
}

}  // namespace veritas::facts
