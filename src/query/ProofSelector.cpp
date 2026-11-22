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

#include "ProofSelector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace veritas::query::internal {
namespace {

struct ProofGraph {
  std::map<std::string, std::vector<facts::WitnessEdge>> edges_by_result;
  std::map<std::string, facts::AnalysisFact> roots;
};

Status CollectClosure(const std::string &key, const ProofGraph &graph,
                      std::set<std::string> *visited,
                      std::set<facts::WitnessEdge> *edges,
                      std::map<std::string, facts::AnalysisFact> *facts) {
  if (!visited->insert(key).second) {
    return Status::Ok();
  }
  const auto root = graph.roots.find(key);
  if (root != graph.roots.end()) {
    facts->emplace(key, root->second);
    return Status::Ok();
  }
  const auto found = graph.edges_by_result.find(key);
  if (found == graph.edges_by_result.end()) {
    return Status::FailedPrecondition(
        "a selected query result has no finite derivation rooted in a "
        "declared input");
  }
  for (const auto &edge : found->second) {
    auto result_fact = facts::MakeFact(edge.result.row);
    if (!result_fact.ok()) {
      return result_fact.status();
    }
    auto input_fact = facts::MakeFact(edge.input.row);
    if (!input_fact.ok()) {
      return input_fact.status();
    }
    edges->insert(edge);
    facts->emplace(facts::EncodeSemanticKey(edge.result.row),
                   std::move(*result_fact));
    const std::string input_key = facts::EncodeSemanticKey(edge.input.row);
    facts->emplace(input_key, std::move(*input_fact));
    Status nested = CollectClosure(input_key, graph, visited, edges, facts);
    if (!nested.ok()) {
      return nested;
    }
  }
  return Status::Ok();
}

} // namespace

StatusOr<SelectedProof>
SelectProofs(std::span<const facts::AnalysisFact> candidates,
             std::span<const facts::WitnessEdge> witnesses,
             std::span<const facts::RootedInputFact> roots,
             std::uint64_t max_edges) {
  ProofGraph graph;
  for (const auto &root : roots) {
    graph.roots.emplace(facts::EncodeSemanticKey(root.fact.row), root.fact);
  }
  for (const auto &edge : witnesses) {
    graph.edges_by_result[facts::EncodeSemanticKey(edge.result.row)].push_back(
        edge);
  }
  for (auto &[key, edges] : graph.edges_by_result) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  }

  SelectedProof selected;
  std::set<facts::WitnessEdge> selected_edges;
  std::map<std::string, facts::AnalysisFact> selected_facts;
  for (const auto &candidate : candidates) {
    std::set<std::string> visited;
    std::set<facts::WitnessEdge> candidate_edges;
    std::map<std::string, facts::AnalysisFact> candidate_facts;
    const std::string key = facts::EncodeSemanticKey(candidate.row);
    Status collected = CollectClosure(key, graph, &visited, &candidate_edges,
                                      &candidate_facts);
    if (!collected.ok()) {
      if (collected.code() == StatusCode::kFailedPrecondition) {
        selected.missing_premise = true;
        continue;
      }
      return collected;
    }
    std::uint64_t new_edges = 0;
    for (const auto &edge : candidate_edges) {
      if (!selected_edges.contains(edge)) {
        ++new_edges;
      }
    }
    if (selected_edges.size() + new_edges > max_edges) {
      selected.truncated = true;
      break;
    }
    selected.results.push_back(candidate);
    selected_edges.insert(candidate_edges.begin(), candidate_edges.end());
    for (auto &[fact_key, fact] : candidate_facts) {
      selected_facts.emplace(fact_key, std::move(fact));
    }
  }

  std::set<core::StableId> result_ids;
  for (const auto &result : selected.results) {
    result_ids.insert(result.fact_id);
  }
  for (auto &[key, fact] : selected_facts) {
    if (!result_ids.contains(fact.fact_id)) {
      selected.premises.push_back(std::move(fact));
    }
  }
  std::sort(selected.premises.begin(), selected.premises.end(),
            [](const facts::AnalysisFact &a, const facts::AnalysisFact &b) {
              return a.fact_id < b.fact_id;
            });
  selected.premises.erase(std::unique(selected.premises.begin(),
                                      selected.premises.end(),
                                      [](const facts::AnalysisFact &a,
                                         const facts::AnalysisFact &b) {
                                        return a.fact_id == b.fact_id;
                                      }),
                          selected.premises.end());
  selected.edges.assign(selected_edges.begin(), selected_edges.end());
  std::sort(selected.edges.begin(), selected.edges.end());
  return selected;
}

} // namespace veritas::query::internal
