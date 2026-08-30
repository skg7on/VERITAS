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

#include "veritas/wpa/WpaOrchestrator.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/WpaComponent.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

// Gathers the facts completed for each successor SCC of `scc_id`, which become
// the successor support the materializer turns into support relations.
std::vector<facts::AnalysisFact> SuccessorSupport(
    const SccGraph& scc_graph, core::StableId scc_id,
    const std::map<core::StableId, std::vector<facts::AnalysisFact>>&
        completed_facts) {
  std::vector<facts::AnalysisFact> support;
  auto successors = scc_graph.Successors(scc_id);
  if (!successors.ok()) {
    return support;
  }
  for (const auto& successor : *successors) {
    auto it = completed_facts.find(successor);
    if (it != completed_facts.end()) {
      support.insert(support.end(), it->second.begin(), it->second.end());
    }
  }
  return support;
}

// Builds a WpaComponentResult from a canonicalized result and its input.
WpaComponentResult MakeResult(const WpaLogicalComponentInput& logical,
                              const facts::CanonicalizedResult& canonical) {
  WpaComponentResult result;
  result.scc_id = logical.scc_id;
  result.component = logical.component;
  result.logical_input_hash = logical.logical_input_hash;
  result.fixpoint_hash = canonical.fixpoint_hash;
  result.external_hash = canonical.external_hash;
  result.facts = canonical.facts;
  result.witnesses = canonical.witnesses;
  result.diagnostics = canonical.diagnostics;
  return result;
}

}  // namespace

WpaOrchestrator::WpaOrchestrator(WpaExecutor& executor,
                                 WpaRunRepository& repository)
    : executor_(executor), repository_(repository) {}

StatusOr<WpaRunResult> WpaOrchestrator::Run(const WpaRunRequest& request) {
  Status begin = repository_.BeginRun(request.run);
  if (!begin.ok()) {
    return begin;
  }

  auto call_graph = CallGraph::FromSummaries(request.summaries);
  if (!call_graph.ok()) {
    repository_.MarkIncomplete(request.run);
    return call_graph.status();
  }
  auto scc_graph = SccGraph::Build(*call_graph);
  if (!scc_graph.ok()) {
    repository_.MarkIncomplete(request.run);
    return scc_graph.status();
  }

  WpaRunResult result;
  result.run = request.run;

  const auto scc_order = scc_graph->ReverseTopologicalOrder();
  for (const auto& scc_id : scc_order) {
    for (const auto component : request.components) {
      result.expected_components.push_back({scc_id, component});
    }
  }

  std::map<core::StableId, std::vector<facts::AnalysisFact>> completed_facts;

  for (const auto& scc_id : scc_order) {
    for (const auto component : request.components) {
      const WpaComponentKey key{scc_id, component};

      WpaMaterializationRequest materialization;
      materialization.semantics =
          static_cast<const facts::AnalysisRunSemanticDescriptor&>(request.run);
      materialization.scc_id = scc_id;
      materialization.component = component;
      materialization.summaries = request.summaries;
      // Keep the successor support alive for the duration of Build: the span
      // stored in the request points into this vector.
      std::vector<facts::AnalysisFact> successor_support =
          SuccessorSupport(*scc_graph, scc_id, completed_facts);
      materialization.successor_support = successor_support;
      materialization.models = request.models;

      auto logical = WpaInputMaterializer::Build(materialization);
      if (!logical.ok()) {
        repository_.RecordComponentFailure(request.run, key,
                                           std::string(logical.status().message()));
        repository_.MarkIncomplete(request.run);
        return logical.status();
      }

      // Collect the rooted input fact IDs for the batch.
      for (const auto& root : logical->local_roots) {
        result.rooted_input_fact_ids.push_back(root.fact.fact_id);
      }
      for (const auto& root : logical->successor_roots) {
        result.rooted_input_fact_ids.push_back(root.fact.fact_id);
      }

      const std::string cache_key = DeriveResultCacheKey(
          request.run, key, logical->logical_input_hash);
      auto reusable = repository_.LoadReusableComponent(cache_key);
      if (!reusable.ok()) {
        repository_.RecordComponentFailure(request.run, key,
                                           std::string(reusable.status().message()));
        repository_.MarkIncomplete(request.run);
        return reusable.status();
      }

      WpaComponentResult component_result;
      if (reusable->has_value()) {
        component_result = std::move(**reusable);
      } else {
        WpaExecutionEnvelope envelope{request.run, std::move(*logical)};
        auto raw = executor_.Execute(envelope, request.limits);
        if (!raw.ok()) {
          repository_.RecordComponentFailure(request.run, key,
                                             std::string(raw.status().message()));
          repository_.MarkIncomplete(request.run);
          return raw.status();
        }
        facts::CanonicalizationRequest canonicalization;
        canonicalization.local_roots = envelope.logical.local_roots;
        canonicalization.successor_roots = envelope.logical.successor_roots;
        canonicalization.evaluation = &*raw;
        auto canonical = facts::ResultCanonicalizer::Canonicalize(canonicalization);
        if (!canonical.ok()) {
          repository_.RecordComponentFailure(
              request.run, key, std::string(canonical.status().message()));
          repository_.MarkIncomplete(request.run);
          return canonical.status();
        }
        component_result = MakeResult(envelope.logical, *canonical);
      }

      auto completion = repository_.StoreSuccessfulComponent(
          request.run, key, component_result);
      if (!completion.ok()) {
        repository_.MarkIncomplete(request.run);
        return completion.status();
      }
      result.completed_components.push_back(std::move(*completion));
      completed_facts[key.scc_id] = std::move(component_result.facts);
    }
  }

  Status complete = repository_.CompleteRun(request.run);
  if (!complete.ok()) {
    return complete;
  }
  return result;
}

}  // namespace veritas::wpa
