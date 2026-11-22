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

// QueryExecutor.h — executes a planned query one admitted SCC at a time.
//
// `QueryExecutor` is the kernel engine boundary: one call evaluates one
// admitted SCC's logical input and returns a raw evaluation that is not yet
// trusted. Grounding, proof selection, budgets and coverage belong to
// `ExecuteQuery`, which stays engine-neutral. `WpaQueryExecutor` adapts the
// existing WPA/SVF executor interface to that boundary: it materializes the
// SCC from its own summary snapshot and runs the same compiled rule bundles
// the baseline WPA run uses. No baseline bundle is replaced.
//
// A query executor returns `StatusCode::kDeadlineExceeded` when its bounded
// evaluation did not terminate; every other non-OK status is a failure that
// publishes no artifact.

#ifndef VERITAS_QUERY_QUERY_EXECUTOR_H_
#define VERITAS_QUERY_QUERY_EXECUTOR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/query/QueryPlanner.h"
#include "veritas/query/QueryTypes.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/WpaComponent.h"
#include "veritas/wpa/WpaExecutor.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::query {

struct QueryKernelRequest {
  facts::AnalysisRunManifest run;
  core::StableId scc_id{};
  QueryPredicate predicate = QueryPredicate::kReachableCall;
  // Successor results or admitted boundary facts, in the derived-relation
  // vocabulary the materializer expects.
  std::vector<facts::AnalysisFact> successor_support;
  // The row allowance still available to this kernel. A kernel whose logical
  // input exceeds it is not executed.
  std::uint64_t remaining_examined_rows = 0;
};

enum class QueryKernelOutcome : std::uint8_t {
  kExecuted,
  kRowBudgetExhausted,
};

struct QueryKernelResult {
  QueryKernelOutcome outcome = QueryKernelOutcome::kExecuted;
  wpa::WpaLogicalComponentInput logical;
  facts::RawWpaEvaluation evaluation;
};

class QueryExecutor {
public:
  virtual ~QueryExecutor() = default;

  virtual facts::EngineIdentity identity() const = 0;
  virtual std::string_view toolchain_identity() const = 0;
  // Binds the executor to the exact dependency snapshot it materializes from.
  virtual std::string_view snapshot_digest() const = 0;
  virtual StatusOr<QueryKernelResult>
  Execute(const QueryKernelRequest &request,
          const wpa::WpaExecutionLimits &limits) const = 0;
};

// The production adapter. It owns the summary copy its materializations read,
// builds the call/SCC graph once, and never introduces a second rule bundle.
class WpaQueryExecutor final : public QueryExecutor {
public:
  static StatusOr<std::unique_ptr<WpaQueryExecutor>>
  Create(wpa::WpaExecutor &engine,
         std::vector<summary::SummaryArtifact> summaries,
         facts::AnalysisRunSemanticDescriptor semantics,
         std::string snapshot_digest);

  facts::EngineIdentity identity() const override;
  std::string_view toolchain_identity() const override;
  std::string_view snapshot_digest() const override;
  StatusOr<QueryKernelResult>
  Execute(const QueryKernelRequest &request,
          const wpa::WpaExecutionLimits &limits) const override;

private:
  WpaQueryExecutor(wpa::WpaExecutor &engine,
                   std::vector<summary::SummaryArtifact> summaries,
                   facts::AnalysisRunSemanticDescriptor semantics,
                   std::string snapshot_digest, wpa::SccGraph scc_graph,
                   wpa::WpaSummaryIndex summary_index);

  wpa::WpaExecutor &engine_;
  std::vector<summary::SummaryArtifact> summaries_;
  facts::AnalysisRunSemanticDescriptor semantics_;
  std::string snapshot_digest_;
  wpa::SccGraph scc_graph_;
  wpa::WpaSummaryIndex summary_index_;
};

// Executes every admitted SCC of `plan` in its recorded order, then selects the
// endpoint-matching results and a finite rooted proof for each.
//
// Failure contract:
//   * a plan whose QueryId no longer re-derives, or whose snapshot digest
//     differs from the executor's, is FailedPrecondition;
//   * a kernel failure other than a timeout returns its status and produces no
//     artifact (a dead worker publishes nothing);
//   * a timeout, row-budget exhaustion, unproven result, or exhausted proof
//     budget produces an inspectable INCOMPLETE artifact with the matching
//     frontier entry. No selected result ever loses a required premise.
StatusOr<QueryArtifact> ExecuteQuery(const QueryPlan &plan,
                                     QueryExecutor &executor);

} // namespace veritas::query

#endif // VERITAS_QUERY_QUERY_EXECUTOR_H_
