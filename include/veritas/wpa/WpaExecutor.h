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

// WpaExecutor.h — the abstract recursive-WPA execution interface.
//
// One executor evaluates one component's engine-neutral logical input under a
// specific execution envelope. The production engine is compiled Souffle; C++
// is a conformance oracle or an explicitly selected emergency engine, never an
// automatic fallback. Execute returns a raw evaluation (asserted results and
// the witness edges backing them) that is not yet trusted: grounding it in
// declared roots and selecting a canonical proof is the canonicalizer's job,
// and stays VERITAS-owned.

#ifndef VERITAS_WPA_WPA_EXECUTOR_H_
#define VERITAS_WPA_WPA_EXECUTOR_H_

#include <chrono>
#include <cstdint>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {

// Resource limits for one component execution.
struct WpaExecutionLimits {
  std::chrono::milliseconds timeout{0};
  std::uint64_t memory_mb = 0;
  std::uint32_t threads = 1;
};

// An execution envelope: the executor-specific run manifest (which carries the
// engine and the exact engine/toolchain identity) plus the immutable,
// engine-neutral logical component input. Both engines consume byte-identical
// logical input under two distinct envelopes.
struct WpaExecutionEnvelope {
  facts::AnalysisRunManifest run;
  WpaLogicalComponentInput logical;
};

// The abstract engine. Each adapter rejects an envelope whose manifest engine
// identity differs from its own, so a run can never silently switch engines.
class WpaExecutor {
 public:
  virtual ~WpaExecutor() = default;
  virtual facts::EngineIdentity identity() const = 0;
  virtual StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& input,
      const WpaExecutionLimits& limits) const = 0;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_WPA_EXECUTOR_H_
