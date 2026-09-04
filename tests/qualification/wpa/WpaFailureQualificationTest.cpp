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

// WpaFailureQualificationTest.cpp — a failed or incomplete component publishes
// no replacement result, and a later failed run never disturbs a prior
// successful run. Every executor failure mode (missing worker, incompatible
// bundle, timeout, crash, malformed witness, schema mismatch) marks its run
// incomplete and leaves the earlier success as stale history only.

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "WpaQualificationSupport.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::wpa::qualification {
namespace {

enum class FailureKind {
  kMissingWorker,
  kIncompatibleBundle,
  kTimeout,
  kCrash,
  kMalformedWitness,
  kSchemaMismatch,
};

Status FailureStatus(FailureKind kind) {
  switch (kind) {
  case FailureKind::kMissingWorker:
    return Status::NotFound("worker executable missing");
  case FailureKind::kIncompatibleBundle:
    return Status::InvalidArgument("incompatible rule bundle");
  case FailureKind::kTimeout:
    return Status::DeadlineExceeded("component timed out");
  case FailureKind::kCrash:
    return Status::Internal("worker crashed");
  case FailureKind::kMalformedWitness:
    return Status::FailedPrecondition("malformed witness edge");
  case FailureKind::kSchemaMismatch:
    return Status::FailedPrecondition("relation schema mismatch");
  }
  return Status::Internal("unknown failure");
}

std::vector<summary::SummaryArtifact> ChainProgram() {
  auto a = V2Summary("a");
  AddDirectCall(&a, "a", "b");
  auto b = V2Summary("b");
  AddDirectCall(&b, "b", "c");
  return {a, b, V2Summary("c")};
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

class WorkingExecutor : public WpaExecutor {
 public:
  facts::EngineIdentity identity() const override {
    return facts::EngineIdentity::kSouffle;
  }
  std::string_view toolchain_identity() const override {
    return "working-toolchain";
  }
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope&, const WpaExecutionLimits&) const override {
    return facts::RawWpaEvaluation{};
  }
};

class FailingExecutor : public WpaExecutor {
 public:
  explicit FailingExecutor(Status status) : status_(std::move(status)) {}
  facts::EngineIdentity identity() const override {
    return facts::EngineIdentity::kSouffle;
  }
  std::string_view toolchain_identity() const override {
    return "failing-toolchain";
  }
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope&, const WpaExecutionLimits&) const override {
    return status_;
  }

 private:
  Status status_;
};

// Runs one successful orchestration, then a distinct failing orchestration,
// and reports whether the failure published nothing while the prior success
// stayed complete. Returns bool (not ASSERT) so it composes under EXPECT_TRUE.
bool VerifyPriorSuccessRetainedAndNewRunIncomplete(FailureKind failure) {
  const auto program = ChainProgram();
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  if (!repo.ok())
    return false;

  const std::array<WpaComponentKind, 1> components = {
      WpaComponentKind::kReachability};

  WorkingExecutor working;
  WpaOrchestrator ok_orchestrator(working, *repo);
  WpaRunRequest ok_request;
  ok_request.run = MakeManifest(facts::EngineIdentity::kSouffle,
                                "ok-toolchain");
  ok_request.summaries = program;
  ok_request.components = components;
  auto ok = ok_orchestrator.Run(ok_request);
  if (!ok.ok())
    return false;

  FailingExecutor failing(FailureStatus(failure));
  WpaOrchestrator fail_orchestrator(failing, *repo);
  WpaRunRequest fail_request;
  fail_request.run = MakeManifest(facts::EngineIdentity::kSouffle,
                                  "fail-toolchain");
  fail_request.summaries = program;
  fail_request.components = components;
  auto failed = fail_orchestrator.Run(fail_request);
  if (failed.ok())
    return false;

  auto failed_status = repo->RunStatus(fail_request.run.run_id);
  if (!failed_status.ok() || *failed_status != WpaRunStatus::kIncomplete)
    return false;

  auto ok_status = repo->RunStatus(ok_request.run.run_id);
  if (!ok_status.ok() || *ok_status != WpaRunStatus::kComplete)
    return false;

  std::filesystem::remove_all(db);
  return true;
}

TEST(WpaFailureQualificationTest, EveryFailureRetainsPriorSuccess) {
  for (const auto failure :
       {FailureKind::kMissingWorker, FailureKind::kIncompatibleBundle,
        FailureKind::kTimeout, FailureKind::kCrash,
        FailureKind::kMalformedWitness, FailureKind::kSchemaMismatch}) {
    SCOPED_TRACE(static_cast<int>(failure));
    EXPECT_TRUE(VerifyPriorSuccessRetainedAndNewRunIncomplete(failure));
  }
}

}  // namespace
}  // namespace veritas::wpa::qualification
