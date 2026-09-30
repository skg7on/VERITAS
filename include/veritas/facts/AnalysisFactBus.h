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

// AnalysisFactBus.h — the M9 ingestion seam.
//
// A successful WPA run is reduced to one immutable, content-addressed
// AnalysisFactBatch: the frozen expected component set, the completed
// components and their hashes, the rooted input fact IDs, and the flattened
// facts/witnesses/diagnostics. The bus validates that batch (one manifest,
// stable fact identity, rooted witness closure, exact expected/completed
// component equality) and delivers it to registered sinks idempotently at
// least once under the canonical (run_id, batch_id). This is the stable seam
// M9 builds a durable, transactional sink and explainFact on; it adds no
// durable M9 store itself.

#ifndef VERITAS_FACTS_ANALYSIS_FACT_BUS_H_
#define VERITAS_FACTS_ANALYSIS_FACT_BUS_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/RowArena.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::core {
class RunMetrics;
}  // namespace veritas::core

namespace veritas::facts {

// The immutable, canonical handoff of one successful WPA run.
struct AnalysisFactBatch {
  // Content-addressed over the canonical batch content, independent of the
  // order facts, witnesses, or components were discovered in.
  core::StableId batch_id;
  AnalysisRunManifest run;
  std::vector<wpa::WpaComponentKey> expected_components;
  std::vector<wpa::WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  // Full rooted-input evidence carried alongside the canonical ID set.
  std::vector<RootedInputFact> rooted_input_facts;
  std::vector<std::string> diagnostics;

  // Canonical facts and witnesses, held compactly. Iterate, do not copy.
  AnalysisFactRange facts() const { return AnalysisFactRange(&facts_); }
  WitnessRange witnesses() const { return WitnessRange(&witnesses_); }
  std::size_t fact_count() const { return facts_.size(); }
  std::size_t witness_count() const { return witnesses_.size(); }

  // Row-level handles by position, forwarded to the owning arena. Each
  // accessor names its arena, so the returned handles are unambiguous and a
  // comparison needs no arena argument. Validation uses these to compare stored
  // bytes instead of decoding rows.
  StatusOr<RowHandle> fact_row_handle_at(std::size_t index) const {
    return facts_.fact_row_handle_at(index);
  }
  StatusOr<RowHandle> witness_result_row_handle_at(std::size_t index) const {
    return witnesses_.witness_result_row_handle_at(index);
  }
  StatusOr<RowHandle> witness_input_row_handle_at(std::size_t index) const {
    return witnesses_.witness_input_row_handle_at(index);
  }

  // The index-th witness, decoded. A position in an append-only arena is a
  // stable identity for a witness; the address of a decoded one is not, because
  // the ranges yield values and a value dies at the end of the step that
  // produced it. A consumer that groups witnesses and reaches them again after
  // the grouping -- the fact store does -- therefore holds positions and asks
  // for the rows back by position. Fails with InvalidArgument past the end.
  StatusOr<WitnessEdge> witness_at(std::size_t index) const {
    auto entry = witnesses_.handle_at(index);
    if (!entry.ok()) {
      return entry.status();
    }
    return witnesses_.DecodeWitness(*entry);
  }

  // Compares a published fact's row against a witness's result or input row.
  // The argument order is the arena order, so a caller cannot silently compare
  // two rows of the same kind.
  bool RowsEqual(RowHandle fact_row, RowHandle witness_row) const {
    return RowArena::RowsEqual(facts_, fact_row, witnesses_, witness_row);
  }

  // Builder side. The arena is append-only, so these replace the whole payload,
  // and a row the arena rejects is left out rather than stored half-encoded.
  // Assembly fills the arenas directly instead: it appends in canonical order
  // and never rebuilds a payload it has already published.
  void SetFacts(const std::vector<AnalysisFact> &facts);
  void SetWitnesses(const std::vector<WitnessEdge> &witnesses);
  void ClearPayload();

private:
  RowArena facts_;
  RowArena witnesses_;

  // The assembler appends into both arenas in canonical order. It is a friend
  // rather than a member because `MakeAnalysisFactBatch` is the pipeline's
  // entry point and takes the run result by value; the builders above are for
  // tests and small callers, which can afford to hand over a whole vector.
  friend AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result);
};

// Reduces a successful WPA run to a canonical batch: flattens the completed
// components' facts/witnesses/diagnostics, canonicalizes component, rooted
// input, fact, and witness ordering, strips the component payload vectors while
// retaining their hashes and metadata, and derives the content-addressed
// batch_id. Lvalues are copied; production passes an rvalue to transfer
// ownership. Mechanical; the bus re-validates on Publish.
AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result);

// Recomputes the canonical content-addressed batch id over every immutable
// field. Exposed so the bus and its callers share one derivation.
core::StableId DeriveBatchId(const AnalysisFactBatch &batch);

// A named consumer of analysis fact batches. Repeated publication of the same
// (run_id, batch_id) must be a successful no-op.
class AnalysisFactSink {
public:
  virtual ~AnalysisFactSink() = default;
  virtual Status Publish(const AnalysisFactBatch &batch) = 0;
};

// Validates and fans out an immutable batch to every registered sink.
//
// Delivery is idempotent at-least-once per sink, keyed by (run_id, batch_id):
// a sink that already received a batch is a successful no-op on retry, so a
// partial fan-out retries only the sinks that failed. Cross-sink atomicity is
// not claimed. Delivery state is recorded durably in the run repository.
class AnalysisFactBus {
public:
  explicit AnalysisFactBus(wpa::WpaRunRepository &delivery_state);

  void AddSink(std::string sink_id, AnalysisFactSink &sink);

  // Optional recorder. Non-owning; never read for control flow.
  void SetMetrics(core::RunMetrics *metrics) { metrics_ = metrics; }

  // Validates the batch, then delivers it to every pending sink. Returns
  // non-OK (FailedPrecondition for a malformed batch) without mutating any
  // component success when validation fails; on a sink failure, returns that
  // sink's status after recording which sinks already completed.
  Status Publish(const AnalysisFactBatch &batch) const;

private:
  Status Validate(const AnalysisFactBatch &batch) const;

  wpa::WpaRunRepository &delivery_state_;
  std::vector<std::pair<std::string, AnalysisFactSink *>> sinks_;
  core::RunMetrics *metrics_ = nullptr;
};

} // namespace veritas::facts

#endif // VERITAS_FACTS_ANALYSIS_FACT_BUS_H_
