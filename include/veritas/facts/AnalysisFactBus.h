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
  std::vector<AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

// Reduces a successful WPA run to a canonical batch: flattens the completed
// components' facts/witnesses/diagnostics, canonicalizes component, rooted
// input, fact, and witness ordering, and derives the content-addressed
// batch_id. Mechanical; the bus re-validates on Publish.
AnalysisFactBatch MakeAnalysisFactBatch(const wpa::WpaRunResult& result);

// A named consumer of analysis fact batches. Repeated publication of the same
// (run_id, batch_id) must be a successful no-op.
class AnalysisFactSink {
 public:
  virtual ~AnalysisFactSink() = default;
  virtual Status Publish(const AnalysisFactBatch& batch) = 0;
};

// Validates and fans out an immutable batch to every registered sink.
//
// Delivery is idempotent at-least-once per sink, keyed by (run_id, batch_id):
// a sink that already received a batch is a successful no-op on retry, so a
// partial fan-out retries only the sinks that failed. Cross-sink atomicity is
// not claimed. Delivery state is recorded durably in the run repository.
class AnalysisFactBus {
 public:
  explicit AnalysisFactBus(wpa::WpaRunRepository& delivery_state);

  void AddSink(std::string sink_id, AnalysisFactSink& sink);

  // Validates the batch, then delivers it to every pending sink. Returns
  // non-OK (FailedPrecondition for a malformed batch) without mutating any
  // component success when validation fails; on a sink failure, returns that
  // sink's status after recording which sinks already completed.
  Status Publish(AnalysisFactBatch batch) const;

 private:
  Status Validate(const AnalysisFactBatch& batch) const;

  wpa::WpaRunRepository& delivery_state_;
  std::vector<std::pair<std::string, AnalysisFactSink*>> sinks_;
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_ANALYSIS_FACT_BUS_H_
