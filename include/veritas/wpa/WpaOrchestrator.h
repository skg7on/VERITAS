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

// WpaOrchestrator.h — bottom-up, reverse-topological WPA run orchestration.
//
// Builds one call/SCC graph, freezes the expected (SccId, ComponentKind) set,
// and iterates reverse-topological SCCs: materialize one logical input, reuse
// a matching content-addressed result, execute and canonicalize otherwise, and
// transactionally store success. A failed or incomplete component publishes no
// replacement result; the prior success remains stale history.

#ifndef VERITAS_WPA_WPA_ORCHESTRATOR_H_
#define VERITAS_WPA_WPA_ORCHESTRATOR_H_

#include <span>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/runtime/WorklistScheduler.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/WpaExecutor.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::analysis::semantic {
class ModelBundle;
}

namespace veritas::wpa {

struct WpaRunRequest {
  facts::AnalysisRunManifest run;
  std::span<const summary::SummaryArtifact> summaries;
  const analysis::semantic::ModelBundle* models = nullptr;
  std::span<const WpaComponentKind> components;
  WpaExecutionLimits limits;
};

struct WpaRunResult {
  facts::AnalysisRunManifest run;
  std::vector<WpaComponentKey> expected_components;
  std::vector<WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  std::vector<runtime::WorkItem> scheduled_predecessors;
  // The flattened, canonical handoff across every completed component: the
  // exact facts, witnesses, and diagnostics the AnalysisFactBus consumes.
  std::vector<facts::AnalysisFact> facts;
  std::vector<facts::WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

class SccStateRepository;

class WpaOrchestrator {
 public:
  // The optional scc_state enables incremental predecessor scheduling: when a
  // component's externally visible hash changes, its predecessors are enqueued
  // through the M7 scheduler and surfaced in scheduled_predecessors.
  WpaOrchestrator(WpaExecutor& executor, WpaRunRepository& repository,
                  SccStateRepository* scc_state = nullptr);

  StatusOr<WpaRunResult> Run(const WpaRunRequest& request);

 private:
  WpaExecutor& executor_;
  WpaRunRepository& repository_;
  SccStateRepository* scc_state_;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_ORCHESTRATOR_H_
