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

#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "veritas/wpa/RelationIo.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {
namespace {

// Runs `program` with `args`, waiting up to `timeout` (zero waits forever).
// Returns the exit code, or a non-OK status on fork/exec/wait failure or when
// the deadline elapses (in which case the child is killed and reaped).
StatusOr<int> RunWorker(const std::string& program,
                        const std::vector<std::string>& args,
                        const WpaExecutionLimits& limits) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const std::string& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t child = ::fork();
  if (child < 0) {
    return Status::Internal("failed to fork the Souffle worker");
  }
  if (child == 0) {
    if (limits.memory_mb != 0) {
      constexpr rlim_t kBytesPerMiB = 1024U * 1024U;
      rlimit address_space{};
      if (::getrlimit(RLIMIT_AS, &address_space) != 0) {
        ::_exit(126);
      }
      const rlim_t requested =
          static_cast<rlim_t>(limits.memory_mb) * kBytesPerMiB;
      if (address_space.rlim_max != RLIM_INFINITY &&
          requested > address_space.rlim_max) {
        ::_exit(126);
      }
      address_space.rlim_cur = requested;
      if (::setrlimit(RLIMIT_AS, &address_space) != 0) {
        ::_exit(126);
      }
    }
    ::execv(program.c_str(), argv.data());
    ::_exit(127);  // execv returns only on failure.
  }

  const auto deadline = std::chrono::steady_clock::now() + limits.timeout;
  for (;;) {
    int status = 0;
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      if (WIFSIGNALED(status)) {
        return Status::Internal("Souffle worker killed by signal " +
                                std::to_string(WTERMSIG(status)));
      }
      return Status::Internal("Souffle worker exited abnormally");
    }
    if (result < 0) {
      return Status::Internal("waitpid on the Souffle worker failed");
    }
    if (limits.timeout.count() > 0 &&
        std::chrono::steady_clock::now() >= deadline) {
      ::kill(child, SIGKILL);
      ::waitpid(child, &status, 0);
      return Status::DeadlineExceeded("Souffle worker exceeded its time limit");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

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

SouffleWpaExecutor::SouffleWpaExecutor(std::filesystem::path worker,
                                       std::string toolchain_identity)
    : worker_(std::move(worker)),
      toolchain_identity_(std::move(toolchain_identity)) {}

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
  constexpr std::uint64_t kBytesPerMiB = 1024U * 1024U;
  if (limits.memory_mb >
      std::numeric_limits<rlim_t>::max() / kBytesPerMiB) {
    return Status::InvalidArgument("WPA memory limit overflows RLIMIT_AS");
  }
#if defined(__APPLE__)
  // Darwin defines RLIMIT_AS as its advisory RLIMIT_RSS resource but rejects
  // finite values with EINVAL. Refuse the run instead of silently omitting an
  // enforcement contract that callers requested.
  if (limits.memory_mb != 0) {
    return Status::InvalidArgument(
        "WPA address-space limits are not supported on this platform");
  }
#endif

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
  std::vector<std::string> args = {
      worker_.string(),
      "--component=" + component,
      "-F",
      work_dir.string(),
      "-D",
      work_dir.string(),
      "--jobs",
      std::to_string(limits.threads),
  };
  StatusOr<int> run_result = RunWorker(worker_.string(), args, limits);
  if (!run_result.ok()) {
    return run_result.status();
  }
  if (*run_result != 0) {
    return Status::Internal("Souffle worker exited with code " +
                            std::to_string(*run_result));
  }

  return RelationIo::ReadOutput(work_dir, input.logical);
}

}  // namespace veritas::wpa
