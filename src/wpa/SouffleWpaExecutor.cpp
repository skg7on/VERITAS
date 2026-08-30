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
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <signal.h>
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
                        std::chrono::milliseconds timeout) {
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
    ::execv(program.c_str(), argv.data());
    ::_exit(127);  // execv returns only on failure.
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
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
    if (timeout.count() > 0 &&
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

SouffleWpaExecutor::SouffleWpaExecutor(std::filesystem::path worker)
    : worker_(std::move(worker)) {}

facts::EngineIdentity SouffleWpaExecutor::identity() const {
  return facts::EngineIdentity::kSouffle;
}

StatusOr<facts::RawWpaEvaluation> SouffleWpaExecutor::Execute(
    const WpaExecutionEnvelope& input, const WpaExecutionLimits& limits) const {
  if (input.run.engine != facts::EngineIdentity::kSouffle) {
    return Status::InvalidArgument(
        "envelope engine identity does not match this executor");
  }

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
      "1",
  };
  StatusOr<int> run_result = RunWorker(worker_.string(), args, limits.timeout);
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
