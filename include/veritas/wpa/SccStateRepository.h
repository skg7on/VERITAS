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

#ifndef VERITAS_WPA_SCC_STATE_REPOSITORY_H_
#define VERITAS_WPA_SCC_STATE_REPOSITORY_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SccResult.h"

namespace veritas::wpa {

struct SccContext {
  std::string revision_id;
  std::string build_variant_id;
};

struct StoredSccState {
  core::StableId scc_id;
  summary::v1::ComponentKind component_kind;
  std::string input_hash;
  std::string fixpoint_hash;
  std::string externally_visible_hash;
  std::size_t iteration_count;
  SccStatus status;
};

enum class ExternalChange {
  kUnchanged,
  kChanged,
};

class SccStateRepository {
public:
  explicit SccStateRepository(summarydb::MetadataStore &metadata_store)
      : metadata_store_(metadata_store) {}

  Status PublishGraph(const SccContext &context, const CallGraph &call_graph,
                      const SccGraph &scc_graph);

  // Reads the committed convergence state of one (SCC, component kind) pair.
  // Only committed rows are visible: a row `StoreState` has queued but no flush
  // has covered is not returned here.
  StatusOr<std::optional<StoredSccState>>
  LoadState(const SccContext &context, core::StableId scc_id,
            summary::v1::ComponentKind component_kind) const;

  // Stores one component's convergence state and reports whether the SCC's
  // externally visible hash moved. The row is queued rather than committed, so
  // it becomes readable once a commit covers it — see `FlushStateCache`, which
  // `StoreState` calls itself once a batch is full. The returned classification
  // is computed from the committed row, not from the queued one.
  //
  // That read is the reason a batch must not span two stores of the same
  // `(scc, component kind)` key: the second store would classify the component
  // against the previous run's row rather than the row the batch holds. This
  // repository's only caller cannot do that — `WpaOrchestrator::Run` visits each
  // key exactly once per run, because `SccGraph::ReverseTopologicalOrder` yields
  // each SCC exactly once and the run's component list is a set of distinct
  // component kinds. A caller that stores a key twice must flush in between.
  //
  // The per-`Run` flush is what extends that argument across runs. When the C++
  // conformance oracle is enabled, `ProjectAnalyzer` shares one repository
  // across two `Run`s over the same keys (`src/analysis/ProjectAnalyzer.cpp`
  // line 233, and the two `Run` calls at lines 264-276 and 309-311), and each
  // `Run` flushes its last batch before returning, so the second run's first
  // store of a key cannot land in the first run's still-open batch. The oracle
  // is off by default (`run_cpp_conformance_oracle = false`).
  StatusOr<ExternalChange> StoreState(const SccContext &context,
                                      const SccResult &result);

  // Commits every queued convergence-state row in one transaction. A no-op when
  // nothing is queued.
  //
  // Durability, stated plainly: rows queued since the last commit are lost if
  // the process dies before this returns. Every one of them is one component's
  // convergence state — the input, fixpoint, and externally visible hashes of
  // one (SCC, component kind) pair — and a later run recomputes it by
  // re-executing that component against the same inputs. No published fact, no
  // provenance edge, no component result, and not the run receipt are affected,
  // and a run that completes publishes exactly what it published before,
  // because `WpaOrchestrator::Run` flushes the run's last batch before it marks
  // the run complete.
  //
  // Losing a batch cannot change what a later run derives. The lost rows are
  // read as absent (or as an older run's row), which changes only the
  // `ExternalChange` this repository reports for that key on the next run; that
  // value is consumed by `WpaCoordinator::EnqueuePredecessorsIfChanged` and
  // lands in `WpaRunResult::scheduled_predecessors`, a reported list that no
  // code executes or persists. The absent case classifies the component as
  // changed, which schedules its predecessors — the conservative direction.
  //
  // A failed flush rolls the transaction back, clears the batch, and returns
  // the error, so the batch is abandoned whole rather than half-written or left
  // queued. The caller fails the run, exactly as it does for any other store
  // error.
  //
  // A run that fails before its final flush commits nothing of its unfinished
  // batch, so up to one batch of rows keeps the previous run's value — the same
  // thing that happens today for every component a failed run never reached.
  // The next run re-executes and re-stores them.
  Status FlushStateCache();

  // How many components one commit covers: the number whose convergence rows a
  // crash can cost. The batch is also flushed whenever it reaches this size, so
  // the window never grows past it.
  static constexpr std::size_t kStateBatchSize = 256;

private:
  // Queues one stored state row's bound values for the next commit. The row
  // cannot be rejected: it is built here, with the column count its statement
  // declares.
  void QueueStateRows(const SccContext &context, const SccResult &result);

  summarydb::MetadataStore &metadata_store_;

  // Bound parameter values of the state rows queued since the last commit, one
  // row per stored component. They are rows rather than a live
  // `BulkInsertBatcher` because the batcher's statement is a prefix plus value
  // tuples, and this row's statement carries an `ON CONFLICT ... DO UPDATE`
  // suffix that a prefix cannot express; a batch this size would take the
  // batcher's single-row path anyway.
  std::vector<std::vector<std::string>> pending_state_rows_;
};

} // namespace veritas::wpa

#endif // VERITAS_WPA_SCC_STATE_REPOSITORY_H_
