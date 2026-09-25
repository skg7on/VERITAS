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

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

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
  std::vector<AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

// Resolves one completed component's stored result during assembly. A caller
// that has released the in-memory payload supplies one of these; the batch is
// then assembled from the same bytes that were stored, so the published content
// is what the store holds rather than what this process happens to remember.
using ComponentReloader = std::function<StatusOr<wpa::WpaComponentResult>(
    const wpa::WpaComponentKey &)>;

// Reduces a successful WPA run to a canonical batch: flattens the completed
// components' facts/witnesses/diagnostics, canonicalizes component, rooted
// input, fact, and witness ordering, strips the component payload vectors while
// retaining their hashes and metadata, and derives the content-addressed
// batch_id. Lvalues are copied; production passes an rvalue to transfer
// ownership. Mechanical; the bus re-validates on Publish.
//
// The run must carry its completed components' payloads. A run from
// `WpaOrchestrator::Run` does not -- it releases them as it stores them -- and
// must be assembled with the loader overload below instead; assembling one here
// is a programming error, and is asserted against rather than producing an empty
// batch.
AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result);

// The same reduction for a run whose completed components carry no payload,
// which is how the orchestrator leaves them: each component's result is fetched
// through `reload` as it is keyed and released again before the next one is
// fetched, so assembly holds one component's payload rather than every
// component's. Every reloaded result must agree with the completed component it
// replaces -- key, logical input hash, fixpoint hash, and external hash -- and a
// disagreement is a `FailedPrecondition` rather than a silently accepted
// substitution. A reload failure is returned as it is, and is neither retried
// nor skipped.
StatusOr<AnalysisFactBatch> MakeAnalysisFactBatch(wpa::WpaRunResult result,
                                                  const ComponentReloader &reload);

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

  // Validates the batch, then delivers it to every pending sink. Returns
  // non-OK (FailedPrecondition for a malformed batch) without mutating any
  // component success when validation fails; on a sink failure, returns that
  // sink's status after recording which sinks already completed.
  Status Publish(const AnalysisFactBatch &batch) const;

private:
  Status Validate(const AnalysisFactBatch &batch) const;

  wpa::WpaRunRepository &delivery_state_;
  std::vector<std::pair<std::string, AnalysisFactSink *>> sinks_;
};

} // namespace veritas::facts

#endif // VERITAS_FACTS_ANALYSIS_FACT_BUS_H_
