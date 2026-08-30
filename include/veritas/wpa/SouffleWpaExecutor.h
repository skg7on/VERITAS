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

// SouffleWpaExecutor.h — the compiled-Souffle production engine adapter.
//
// Writes the engine-neutral logical input with RelationIo, runs the compiled
// veritas-souffle-worker as a subprocess under the requested limits, and reads
// the derived relations and witness relation back into a raw evaluation. A
// timeout, signal, non-zero exit, missing output, schema mismatch, or witness
// parse failure returns a non-OK Status with no evaluation.

#ifndef VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_
#define VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_

#include <filesystem>

#include "veritas/wpa/WpaExecutor.h"

namespace veritas::wpa {

class SouffleWpaExecutor final : public WpaExecutor {
 public:
  explicit SouffleWpaExecutor(std::filesystem::path worker);

  facts::EngineIdentity identity() const override;
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& input,
      const WpaExecutionLimits& limits) const override;

 private:
  std::filesystem::path worker_;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_
