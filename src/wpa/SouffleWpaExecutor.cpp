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

#include "veritas/wpa/SouffleWpaExecutor.h"

#include <string>
#include <system_error>

#include <unistd.h>

#include "veritas/wpa/RelationIo.h"
#include "veritas/wpa/SouffleRunner.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {
namespace {

// Removes `dir` when the scope exits, without throwing (VERITAS has no
// exceptions, so normal control flow is the only path that runs this).
struct DirCleanup {
  std::filesystem::path dir;
  ~DirCleanup() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

}  // namespace

SouffleWpaExecutor::SouffleWpaExecutor(std::filesystem::path /*worker*/,
                                       std::string toolchain_identity)
    : toolchain_identity_(std::move(toolchain_identity)) {}

facts::EngineIdentity SouffleWpaExecutor::identity() const {
  return facts::EngineIdentity::kSouffle;
}

std::string_view SouffleWpaExecutor::toolchain_identity() const {
  return toolchain_identity_;
}

StatusOr<facts::RawWpaEvaluation> SouffleWpaExecutor::Execute(
    const WpaExecutionEnvelope& input, const WpaExecutionLimits& limits) const {
  if (input.run.engine != facts::EngineIdentity::kSouffle) {
    return Status::InvalidArgument(
        "envelope engine identity does not match this executor");
  }
  if (input.run.engine_toolchain_identity != toolchain_identity_) {
    return Status::InvalidArgument(
        "envelope toolchain identity does not match this executor");
  }
  if (limits.threads != 1) {
    return Status::InvalidArgument(
        "the Souffle WPA executor requires exactly one worker thread");
  }
  // The per-component timeout and memory limits were enforced by the subprocess
  // worker; the in-process runner cannot kill or resource-limit the host, so
  // they are intentionally ignored here.

  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  if (made == nullptr) {
    return Status::Internal("failed to create a run-local temporary directory");
  }
  const std::filesystem::path work_dir(made);
  DirCleanup cleanup{work_dir};

  Status write_status = RelationIo::WriteInput(work_dir, input.logical);
  if (!write_status.ok()) {
    return write_status;
  }

  const std::string component(ComponentKindName(input.logical.component));
  const int run_status = veritas_souffle_run(
      component.c_str(), work_dir.string().c_str(), work_dir.string().c_str(),
      static_cast<unsigned>(limits.threads));
  if (run_status != 0) {
    return Status::Internal("Souffle evaluation failed with code " +
                            std::to_string(run_status));
  }

  return RelationIo::ReadOutput(work_dir, input.logical);
}

}  // namespace veritas::wpa
