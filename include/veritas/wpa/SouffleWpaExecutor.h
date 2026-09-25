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
// Hands the engine-neutral logical input to an in-memory session (the C ABI in
// SouffleRunner.h), evaluates the compiled Souffle program in-process, and
// scans the derived relations and the witness relation back into a raw
// evaluation. Nothing on this path touches the filesystem. A session that will
// not open, a rejected insert, a non-zero run status, an unscannable relation,
// a schema mismatch, or a witness key that does not decode returns a non-OK
// Status with no evaluation.

#ifndef VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_
#define VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "veritas/wpa/WpaExecutor.h"

namespace veritas::wpa {

class SouffleWpaExecutor final : public WpaExecutor {
 public:
  // The `worker` path is accepted for API compatibility but unused: the engine
  // is linked in-process, not spawned as a subprocess.
  SouffleWpaExecutor(std::filesystem::path worker,
                     std::string toolchain_identity);

  facts::EngineIdentity identity() const override;
  std::string_view toolchain_identity() const override;
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& input,
      const WpaExecutionLimits& limits) const override;

 private:
  std::string toolchain_identity_;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_SOUFFLE_WPA_EXECUTOR_H_
