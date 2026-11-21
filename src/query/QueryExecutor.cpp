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

#include "veritas/query/QueryExecutor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ProofSelector.h"
#include "veritas/facts/ResultCanonicalizer.h"

namespace veritas::query {
namespace {

using core::StableId;
using facts::AnalysisFact;
using facts::SemanticRow;

bool MatchesAny(const SemanticRow &row, std::size_t column,
                const std::vector<TypedRef> &refs) {
  if (row.cells.size() <= column) {
    return false;
  }
  const auto *id = std::get_if<StableId>(&row.cells[column]);
  if (id == nullptr) {
    return false;
  }
  for (const auto &ref : refs) {
    if (ref.id == *id) {
      return true;
    }
  }
  return false;
}

bool MatchesSubjects(const SemanticRow &row,
                     const std::vector<TypedRef> &subjects) {
  if (subjects.empty()) {
    return true;
  }
  // GlobalFlow has no function column; subjects are scope anchors only.
  if (row.relation == facts::RelationId::kGlobalFlow) {
    return true;
  }
  return MatchesAny(row, 0, subjects);
}

bool MatchesDescriptor(const SemanticRow &row,
                       const QueryDescriptor &descriptor) {
  if (row.relation != PredicateRelation(descriptor.predicate)) {
    return false;
  }
  if (!MatchesAny(row, 0, descriptor.sources)) {
    return false;
  }
  if (!descriptor.sinks.empty() && !MatchesAny(row, 1, descriptor.sinks)) {
    return false;
  }
  return MatchesSubjects(row, descriptor.subjects);
}

std::vector<AnalysisFact> FilterResults(const std::vector<AnalysisFact> &facts,
                                        const QueryDescriptor &descriptor) {
  std::vector<AnalysisFact> results;
  for (const auto &fact : facts) {
    if (MatchesDescriptor(fact.row, descriptor)) {
      results.push_back(fact);
    }
  }
  std::sort(results.begin(), results.end(),
            [](const AnalysisFact &a, const AnalysisFact &b) {
              return a.fact_id < b.fact_id;
            });
  results.erase(std::unique(results.begin(), results.end(),
                            [](const AnalysisFact &a, const AnalysisFact &b) {
                              return a.fact_id == b.fact_id;
                            }),
                results.end());
  return results;
}

std::vector<std::string> BuildAssumptions(const QueryPlan &plan) {
  std::vector<std::string> assumptions;
  assumptions.push_back(
      "query.relevance-filter:v1:admitted-sccs=" +
      std::to_string(plan.sccs.size()) +
      ";relevant-functions=" + std::to_string(plan.relevant_functions.size()));
  for (const auto &boundary : plan.boundary_summaries) {
    const std::string function = core::ToString(boundary.function_variant_id);
    assumptions.push_back("boundary:" + function +
                          ":producer=" + boundary.producer_version);
    for (const auto &assumption : boundary.assumptions) {
      assumptions.push_back("boundary:" + function + ":" + assumption);
    }
    for (const auto &site : boundary.covered_call_sites) {
      assumptions.push_back("boundary:" + function +
                            ":covers-callsite:" + core::ToString(site));
    }
  }
  std::sort(assumptions.begin(), assumptions.end());
  assumptions.erase(std::unique(assumptions.begin(), assumptions.end()),
                    assumptions.end());
  return assumptions;
}

} // namespace

// --- WpaQueryExecutor -------------------------------------------------------

WpaQueryExecutor::WpaQueryExecutor(
    wpa::WpaExecutor &engine, std::vector<summary::SummaryArtifact> summaries,
    facts::AnalysisRunSemanticDescriptor semantics, std::string snapshot_digest,
    wpa::CallGraph call_graph, wpa::SccGraph scc_graph,
    wpa::WpaSummaryIndex summary_index)
    : engine_(engine), summaries_(std::move(summaries)),
      semantics_(std::move(semantics)),
      snapshot_digest_(std::move(snapshot_digest)),
      call_graph_(std::move(call_graph)), scc_graph_(std::move(scc_graph)),
      summary_index_(std::move(summary_index)) {}

StatusOr<std::unique_ptr<WpaQueryExecutor>>
WpaQueryExecutor::Create(wpa::WpaExecutor &engine,
                         std::vector<summary::SummaryArtifact> summaries,
                         facts::AnalysisRunSemanticDescriptor semantics,
                         std::string snapshot_digest) {
  if (snapshot_digest.empty()) {
    return Status::InvalidArgument("query executor needs a snapshot digest");
  }
  auto call_graph = wpa::CallGraph::FromSummaries(summaries);
  if (!call_graph.ok()) {
    return call_graph.status();
  }
  auto scc_graph = wpa::SccGraph::Build(*call_graph);
  if (!scc_graph.ok()) {
    return scc_graph.status();
  }
  auto index = wpa::WpaSummaryIndex::Build(summaries);
  if (!index.ok()) {
    return index.status();
  }
  // The summary index points into the summaries vector's buffer. A vector move
  // preserves that allocation, so the index stays valid after the move into
  // the executor's member.
  return std::unique_ptr<WpaQueryExecutor>(
      new WpaQueryExecutor(engine, std::move(summaries), std::move(semantics),
                           std::move(snapshot_digest), std::move(*call_graph),
                           std::move(*scc_graph), std::move(*index)));
}

facts::EngineIdentity WpaQueryExecutor::identity() const {
  return engine_.identity();
}

std::string_view WpaQueryExecutor::toolchain_identity() const {
  return engine_.toolchain_identity();
}

std::string_view WpaQueryExecutor::snapshot_digest() const {
  return snapshot_digest_;
}

StatusOr<QueryKernelResult>
WpaQueryExecutor::Execute(const QueryKernelRequest &request,
                          const wpa::WpaExecutionLimits &limits) const {
  auto members = scc_graph_.Members(request.scc_id);
  if (!members.ok()) {
    return Status::NotFound("query kernel SCC is not in this snapshot");
  }

  wpa::WpaMaterializationRequest materialization;
  materialization.semantics = semantics_;
  materialization.scc_id = request.scc_id;
  materialization.component = PredicateComponent(request.predicate);
  materialization.summaries = summaries_;
  materialization.successor_support = request.successor_support;
  materialization.scc_graph = &scc_graph_;
  auto logical =
      wpa::WpaInputMaterializer::Build(materialization, summary_index_);
  if (!logical.ok()) {
    return logical.status();
  }

  QueryKernelResult result;
  if (logical->edb.size() > request.remaining_examined_rows) {
    result.outcome = QueryKernelOutcome::kRowBudgetExhausted;
    result.logical = std::move(*logical);
    return result;
  }

  wpa::WpaExecutionEnvelope envelope{request.run, std::move(*logical)};
  auto raw = engine_.Execute(envelope, limits);
  if (!raw.ok()) {
    return raw.status();
  }
  result.evaluation = std::move(*raw);
  result.logical = std::move(envelope.logical);
  return result;
}

// --- ExecuteQuery -----------------------------------------------------------

StatusOr<QueryArtifact> ExecuteQuery(const QueryPlan &plan,
                                     QueryExecutor &executor) {
  auto query_id = ComputeQueryId(plan.descriptor);
  if (!query_id.ok()) {
    return query_id.status();
  }
  if (!(*query_id == plan.query_id)) {
    return Status::FailedPrecondition(
        "query plan identity does not re-derive from its descriptor");
  }
  if (plan.snapshot_digest != executor.snapshot_digest()) {
    return Status::FailedPrecondition(
        "query plan was planned against a different dependency snapshot");
  }
  Status limits_valid = ValidateQueryLimits(plan.descriptor.limits);
  if (!limits_valid.ok()) {
    return limits_valid;
  }

  facts::AnalysisRunDescriptor run_descriptor;
  static_cast<facts::AnalysisRunSemanticDescriptor &>(run_descriptor) =
      plan.semantics;
  run_descriptor.revision_id = plan.revision_id;
  run_descriptor.engine = executor.identity();
  run_descriptor.engine_toolchain_identity =
      std::string(executor.toolchain_identity());
  auto manifest = facts::MakeAnalysisRun(run_descriptor);
  if (!manifest.ok()) {
    return manifest.status();
  }

  const auto started = std::chrono::steady_clock::now();

  std::vector<QueryFrontierEntry> frontier = plan.frontier;
  std::map<StableId, std::vector<AnalysisFact>> completed;
  std::set<StableId> admitted;
  std::map<StableId, std::set<StableId>> members_by_scc;
  for (const auto &scc : plan.sccs) {
    admitted.insert(scc.scc_id);
    members_by_scc[scc.scc_id] =
        std::set<StableId>(scc.members.begin(), scc.members.end());
  }
  for (const auto &scc : plan.excluded_sccs) {
    members_by_scc[scc.scc_id] =
        std::set<StableId>(scc.members.begin(), scc.members.end());
  }

  std::vector<AnalysisFact> all_facts;
  std::vector<facts::WitnessEdge> all_witnesses;
  std::map<StableId, facts::RootedInputFact> roots;
  std::uint64_t examined = 0;
  std::uint64_t kernels = 0;
  std::uint64_t support_rows = 0;
  std::uint64_t boundary_rows = 0;

  auto add_roots = [&roots](const wpa::WpaLogicalComponentInput &logical) {
    for (const auto &root : logical.local_roots) {
      roots.emplace(root.fact.fact_id, root);
    }
    for (const auto &root : logical.successor_roots) {
      roots.emplace(root.fact.fact_id, root);
    }
  };

  for (const auto &scc : plan.sccs) {
    if (examined >= plan.descriptor.limits.max_examined_rows) {
      frontier.push_back(
          QueryFrontierEntry{QueryFrontierKind::kRowBudget, scc.scc_id,
                             "examined-row budget exhausted before this SCC"});
      break;
    }

    std::vector<AnalysisFact> support;
    for (const auto &successor : scc.successors) {
      const auto done = completed.find(successor);
      if (done != completed.end()) {
        support_rows += done->second.size();
        support.insert(support.end(), done->second.begin(), done->second.end());
        continue;
      }
      if (admitted.contains(successor)) {
        return Status::Internal(
            "query plan order does not compute a successor before its caller");
      }
      // An unadmitted successor may be covered by an admitted boundary
      // summary. The planner recorded the frontier when it was not.
      const auto members = members_by_scc.find(successor);
      if (members == members_by_scc.end()) {
        continue;
      }
      for (const auto &boundary : plan.boundary_summaries) {
        if (!members->second.contains(boundary.function_variant_id) ||
            !BoundarySupports(boundary, plan.descriptor.predicate)) {
          continue;
        }
        boundary_rows += boundary.facts.size();
        support.insert(support.end(), boundary.facts.begin(),
                       boundary.facts.end());
      }
    }

    QueryKernelRequest request;
    request.run = *manifest;
    request.scc_id = scc.scc_id;
    request.predicate = plan.descriptor.predicate;
    request.successor_support = std::move(support);
    request.remaining_examined_rows =
        plan.descriptor.limits.max_examined_rows - examined;

    wpa::WpaExecutionLimits execution_limits;
    execution_limits.timeout =
        std::chrono::milliseconds(plan.descriptor.runtime.timeout_ms);
    execution_limits.memory_mb = plan.descriptor.runtime.memory_mb;
    execution_limits.threads = plan.descriptor.runtime.threads;

    auto kernel = executor.Execute(request, execution_limits);
    if (!kernel.ok()) {
      if (kernel.status().code() == StatusCode::kDeadlineExceeded) {
        frontier.push_back(
            QueryFrontierEntry{QueryFrontierKind::kTimeout, scc.scc_id,
                               "bounded evaluation did not terminate"});
        break;
      }
      // Any other failure publishes nothing: no partial artifact is minted.
      return kernel.status();
    }
    ++kernels;
    if (kernel->outcome == QueryKernelOutcome::kRowBudgetExhausted) {
      frontier.push_back(QueryFrontierEntry{
          QueryFrontierKind::kRowBudget, scc.scc_id,
          "logical input exceeds the remaining examined-row budget"});
      break;
    }
    examined += kernel->logical.edb.size();

    facts::CanonicalizationRequest canonicalization;
    canonicalization.local_roots = kernel->logical.local_roots;
    canonicalization.successor_roots = kernel->logical.successor_roots;
    canonicalization.evaluation = &kernel->evaluation;
    auto canonical = facts::ResultCanonicalizer::Canonicalize(canonicalization);
    if (!canonical.ok()) {
      if (canonical.status().code() == StatusCode::kFailedPrecondition) {
        frontier.push_back(QueryFrontierEntry{
            QueryFrontierKind::kMissingPremise, scc.scc_id,
            "a kernel result has no finite proof rooted in its inputs"});
        break;
      }
      return canonical.status();
    }

    add_roots(kernel->logical);
    all_facts.insert(all_facts.end(), canonical->facts.begin(),
                     canonical->facts.end());
    all_witnesses.insert(all_witnesses.end(), canonical->witnesses.begin(),
                         canonical->witnesses.end());
    completed.emplace(scc.scc_id, canonical->facts);
  }

  const auto candidates = FilterResults(all_facts, plan.descriptor);
  std::vector<facts::RootedInputFact> root_facts;
  root_facts.reserve(roots.size());
  for (const auto &[id, root] : roots) {
    root_facts.push_back(root);
  }
  auto proof = internal::SelectProofs(candidates, all_witnesses, root_facts,
                                      plan.descriptor.limits.max_proof_edges);
  if (!proof.ok()) {
    return proof.status();
  }
  if (proof->missing_premise) {
    frontier.push_back(QueryFrontierEntry{
        QueryFrontierKind::kMissingPremise, std::nullopt,
        "a selected result could not be rooted; it stays unsealed"});
  }
  if (proof->truncated) {
    frontier.push_back(QueryFrontierEntry{
        QueryFrontierKind::kProofBudget, std::nullopt,
        "proof-edge budget admits only a canonical prefix of the results"});
  }
  CanonicalizeQueryFrontier(&frontier);

  QueryCoverage coverage;
  coverage.outcome = (plan.scope_closed && frontier.empty())
                         ? QueryOutcome::kComplete
                         : QueryOutcome::kIncomplete;
  coverage.predicate_supported = true;
  coverage.scope_closed = plan.scope_closed;
  coverage.frontier = std::move(frontier);
  coverage.assumptions = BuildAssumptions(plan);
  coverage.examined_rows = examined;
  coverage.proof_edges = proof->edges.size();
  coverage.proof_truncated = proof->truncated;

  const auto finished = std::chrono::steady_clock::now();
  QueryCost cost;
  cost.kernels_executed = kernels;
  cost.worker_setup_count = kernels;
  cost.materialized_rows = examined;
  cost.successor_support_rows = support_rows;
  cost.boundary_rows = boundary_rows;
  cost.wall_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(finished - started)
          .count());
  cost.global_svf_bootstrap = false;
  cost.global_svf_bootstrap_identity = "not-run:local-summary-input";

  auto artifact_id = ComputeQueryArtifactId(
      plan.query_id, coverage, proof->results, proof->premises, proof->edges);
  if (!artifact_id.ok()) {
    return artifact_id.status();
  }
  return QueryArtifact{plan.query_id,
                       *artifact_id,
                       std::move(proof->results),
                       std::move(proof->premises),
                       std::move(proof->edges),
                       std::move(coverage),
                       std::move(cost)};
}

} // namespace veritas::query
