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

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/RunMetrics.h"
#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SccStateRepository.h"
#include "veritas/wpa/WpaComponent.h"
#include "veritas/wpa/WpaCoordinator.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

// Gathers the facts completed for each successor SCC of `scc_id`, restricted to
// the component's relation domain, which become the successor support the
// materializer turns into support relations.
//
// The facts are read from the completed component results the batch is already
// keeping, not from a parallel copy. A second copy of every component's facts
// is a whole payload's worth of resident memory held for the entire run, and
// the completed results outlive the run's last materialization anyway.
std::vector<facts::AnalysisFact> SuccessorSupport(
    const SccGraph& scc_graph, core::StableId scc_id, WpaComponentKind component,
    const std::vector<WpaComponentCompletion>& completed,
    const std::map<WpaComponentKey, std::size_t>& completed_index) {
  std::set<facts::RelationId> expected;
  for (const auto& domain : ComponentDomains(component)) {
    if (domain.support.has_value()) {
      expected.insert(domain.derived);
    }
  }
  std::vector<facts::AnalysisFact> support;
  auto successors = scc_graph.Successors(scc_id);
  if (!successors.ok()) {
    return support;
  }
  for (const auto& successor : *successors) {
    const auto it = completed_index.find(WpaComponentKey{successor, component});
    if (it == completed_index.end()) {
      continue;
    }
    for (const auto& fact : completed[it->second].result.facts) {
      if (expected.contains(fact.row.relation)) {
        support.push_back(fact);
      }
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

// Maps the V2 component kind to the V1 protobuf enum the M7 scheduler uses.
summary::v1::ComponentKind V1Component(WpaComponentKind component) {
  switch (component) {
  case WpaComponentKind::kReachability:
    return summary::v1::COMPONENT_KIND_CALLS;
  case WpaComponentKind::kFlow:
    return summary::v1::COMPONENT_KIND_VALUE_FLOW;
  case WpaComponentKind::kMemoryEffects:
    return summary::v1::COMPONENT_KIND_MEMORY_EFFECTS;
  case WpaComponentKind::kEffects:
    return summary::v1::COMPONENT_KIND_UNKNOWNS;
  }
  return summary::v1::COMPONENT_KIND_UNSPECIFIED;
}

// Builds the minimal V1 SccResult the incremental scheduler needs to compare
// the externally visible hash; facts are carried by the V2 run repository, not
// here.
SccResult ToSccResult(const WpaComponentResult& result) {
  SccResult scc;
  scc.scc_id = result.scc_id;
  scc.component_kind = V1Component(result.component);
  scc.input_hash = result.logical_input_hash;
  scc.fixpoint_hash = result.fixpoint_hash;
  scc.externally_visible_hash = result.external_hash;
  scc.iteration_count = 1;
  scc.status = SccStatus::kConverged;
  return scc;
}

// The label a per-component span carries so a top-N entry names the component
// it came from: `<component-kind>/<scc-id>`. Minted only when a recorder is
// present, because a null recorder discards the label and the component loop
// must not allocate a string per occurrence for nothing.
std::string ComponentSpanLabel(core::RunMetrics* metrics,
                               const WpaComponentKey& key) {
  if (metrics == nullptr) {
    return {};
  }
  return std::string(ComponentKindName(key.component)) + "/" +
         core::ToString(key.scc_id);
}

}  // namespace

WpaOrchestrator::WpaOrchestrator(WpaExecutor& executor,
                                 WpaRunRepository& repository,
                                 SccStateRepository* scc_state)
    : executor_(executor), repository_(repository), scc_state_(scc_state) {}

StatusOr<WpaRunResult> WpaOrchestrator::Run(const WpaRunRequest& request) {
  // Read once: the recorder is optional, non-owning, and never read for control
  // flow, so every span and counter below tests this one pointer.
  core::RunMetrics* const metrics = request.metrics;

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
  auto summary_index = WpaSummaryIndex::Build(request.summaries);
  if (!summary_index.ok()) {
    repository_.MarkIncomplete(request.run);
    return summary_index.status();
  }

  WpaRunResult result;
  result.run = request.run;

  SccContext context;
  context.revision_id = core::ToString(request.run.revision_id);
  context.build_variant_id = core::ToString(request.run.build_variant_id);
  const auto scc_order = scc_graph->ReverseTopologicalOrder();

  // The persisted call/SCC graph and the frozen expected set are one measured
  // unit: the set is read off the same SCC order the graph state was built
  // from, so the two cannot describe different decompositions.
  {
    core::PhaseSpan span(metrics, "wpa.graph_build", core::SpanMode::kBearing);
    if (scc_state_ != nullptr) {
      Status published =
          scc_state_->PublishGraph(context, *call_graph, *scc_graph);
      if (!published.ok()) {
        repository_.MarkIncomplete(request.run);
        return published;
      }
    }

    for (const auto& scc_id : scc_order) {
      for (const auto component : request.components) {
        result.expected_components.push_back({scc_id, component});
      }
    }
  }

  // Where each completed component's result lives in `result.completed_components`,
  // which is the single owner of every component's facts and witnesses.
  std::map<WpaComponentKey, std::size_t> completed_index;

  // Per-component totals, accumulated here because AddCounter inside the loop
  // would append one counter per occurrence -- 13,716 of them on a real corpus
  // -- to a list the report reads back by name. They are added once, below.
  std::uint64_t reused_count = 0;
  std::uint64_t executed_count = 0;
  const std::uint64_t expected_count = result.expected_components.size();

  {
    core::PhaseSpan orchestrate_span(metrics, "wpa.orchestrate",
                                     core::SpanMode::kBearing);
    for (const auto& scc_id : scc_order) {
      for (const auto component : request.components) {
        const WpaComponentKey key{scc_id, component};

        // Materialize covers both the request build and the rooted-input
        // collection that reads its output, which is why the failure check
        // sits outside the span rather than between the two halves.
        auto logical = [&]() -> StatusOr<WpaLogicalComponentInput> {
          core::PhaseSpan span(metrics, "wpa.component.materialize",
                               core::SpanMode::kDistributed);
          span.SetLabel(ComponentSpanLabel(metrics, key));

          WpaMaterializationRequest materialization;
          materialization.semantics =
              static_cast<const facts::AnalysisRunSemanticDescriptor&>(
                  request.run);
          materialization.scc_id = scc_id;
          materialization.component = component;
          materialization.summaries = request.summaries;
          // Reuse the whole-program SCC decomposition built once above, rather
          // than rebuilding the call graph and SCC graph for every component.
          materialization.scc_graph = &*scc_graph;
          // Keep the successor support alive for the duration of Build: the
          // span stored in the request points into this vector.
          std::vector<facts::AnalysisFact> successor_support = SuccessorSupport(
              *scc_graph, scc_id, component, result.completed_components,
              completed_index);
          materialization.successor_support = successor_support;
          materialization.models = request.models;

          auto built =
              WpaInputMaterializer::Build(materialization, *summary_index);
          if (!built.ok()) {
            return built.status();
          }

          // Collect the rooted input fact IDs and their full evidence for the
          // batch.
          for (const auto& root : built->local_roots) {
            result.rooted_input_fact_ids.push_back(root.fact.fact_id);
            result.rooted_input_facts.push_back(root);
          }
          for (const auto& root : built->successor_roots) {
            result.rooted_input_fact_ids.push_back(root.fact.fact_id);
            result.rooted_input_facts.push_back(root);
          }
          return built;
        }();
        if (!logical.ok()) {
          repository_.RecordComponentFailure(
              request.run, key, std::string(logical.status().message()));
          repository_.MarkIncomplete(request.run);
          return logical.status();
        }

        const ResultCacheDescriptor cache_descriptor = MakeResultCacheDescriptor(
            request.run, key, logical->logical_input_hash);
        auto reusable = [&] {
          core::PhaseSpan span(metrics, "wpa.component.cache_lookup",
                               core::SpanMode::kDistributed);
          span.SetLabel(ComponentSpanLabel(metrics, key));
          return repository_.LoadReusableComponent(cache_descriptor);
        }();
        if (!reusable.ok()) {
          repository_.RecordComponentFailure(
              request.run, key, std::string(reusable.status().message()));
          repository_.MarkIncomplete(request.run);
          return reusable.status();
        }

        WpaComponentResult component_result;
        if (reusable->has_value()) {
          component_result = std::move(**reusable);
          ++reused_count;
        } else {
          WpaExecutionEnvelope envelope{request.run, std::move(*logical)};
          auto raw = [&] {
            core::PhaseSpan span(metrics, "wpa.component.execute",
                                 core::SpanMode::kDistributed);
            span.SetLabel(ComponentSpanLabel(metrics, key));
            return executor_.Execute(envelope, request.limits);
          }();
          if (!raw.ok()) {
            repository_.RecordComponentFailure(
                request.run, key, std::string(raw.status().message()));
            repository_.MarkIncomplete(request.run);
            return raw.status();
          }
          facts::CanonicalizationRequest canonicalization;
          canonicalization.local_roots = envelope.logical.local_roots;
          canonicalization.successor_roots = envelope.logical.successor_roots;
          canonicalization.evaluation = &*raw;
          auto canonical = [&] {
            core::PhaseSpan span(metrics, "wpa.component.canonicalize",
                                 core::SpanMode::kDistributed);
            span.SetLabel(ComponentSpanLabel(metrics, key));
            return facts::ResultCanonicalizer::Canonicalize(canonicalization);
          }();
          if (!canonical.ok()) {
            repository_.RecordComponentFailure(
                request.run, key, std::string(canonical.status().message()));
            repository_.MarkIncomplete(request.run);
            return canonical.status();
          }
          component_result = MakeResult(envelope.logical, *canonical);
          ++executed_count;
        }

        // The completion takes ownership of the payload, so the local result is
        // moved rather than copied and read through the completion afterwards.
        auto completion = repository_.StoreSuccessfulComponent(
            request.run, key, std::move(component_result));
        if (!completion.ok()) {
          repository_.MarkIncomplete(request.run);
          return completion.status();
        }

        // Incremental propagation: a changed externally visible hash schedules
        // the component's predecessors through the M7 scheduler.
        if (scc_state_ != nullptr) {
          auto change =
              scc_state_->StoreState(context, ToSccResult(completion->result));
          if (!change.ok()) {
            repository_.MarkIncomplete(request.run);
            return change.status();
          }
          if (*change == ExternalChange::kChanged) {
            runtime::WorklistScheduler scheduler;
            auto enqueue = WpaCoordinator::EnqueuePredecessorsIfChanged(
                *change, key.scc_id, V1Component(component), context, {},
                *scc_graph, &scheduler);
            if (!enqueue.ok()) {
              repository_.MarkIncomplete(request.run);
              return enqueue;
            }
            while (!scheduler.Empty()) {
              result.scheduled_predecessors.push_back(*scheduler.PopNext());
            }
          }
        }

        completed_index[key] = result.completed_components.size();
        result.completed_components.push_back(std::move(*completion));
      }
    }
  }

  // The per-component totals, added once now that the loop has finished. The
  // frozen expected set is summed two ways -- as a total and per kind -- and
  // both come from `result.expected_components`, so the report's two
  // expected-component totals cannot describe different sets.
  if (metrics != nullptr) {
    metrics->AddCounter("wpa.components.expected", expected_count, "count");
    metrics->AddCounter("wpa.components.reused", reused_count, "count");
    metrics->AddCounter("wpa.components.executed", executed_count, "count");

    std::map<WpaComponentKind, std::uint64_t> per_kind;
    for (const WpaComponentKey& expected : result.expected_components) {
      ++per_kind[expected.component];
    }
    for (const auto& entry : per_kind) {
      metrics->AddCounter("wpa.component." +
                              std::string(ComponentKindName(entry.first)) +
                              ".expected",
                          entry.second, "count");
    }
  }

  // The run's last batch of convergence state is committed here, before the run
  // is marked complete and before `Run` returns. A later run reads these rows to
  // decide whether a component's externally visible hash moved, so leaving the
  // tail of the run queued would let it compare against a stale row. A failure
  // takes the same path as every other store error in this run.
  if (scc_state_ != nullptr) {
    core::PhaseSpan span(metrics, "wpa.scc_state_flush",
                         core::SpanMode::kBearing);
    Status flushed = scc_state_->FlushStateCache();
    if (!flushed.ok()) {
      repository_.MarkIncomplete(request.run);
      return flushed;
    }
  }

  Status complete = repository_.CompleteRun(request.run);
  if (!complete.ok()) {
    // `CompleteRun` can now fail on its final batch flush as well as on the
    // status update, so it gets the same failure path as every other store
    // error in this run rather than returning with the row left `kInProgress`.
    repository_.MarkIncomplete(request.run);
    return complete;
  }
  return result;
}

}  // namespace veritas::wpa
