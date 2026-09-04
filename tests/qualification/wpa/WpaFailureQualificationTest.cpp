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

// WpaFailureQualificationTest.cpp — failure atomicity of the production WPA
// run. A failed component publishes no replacement; the last successful
// component remains queryable as stale history. Executor-level failure
// injection (mismatched provenance, missing worker, timeout) fails closed
// without producing an evaluation.

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::wpa {
namespace {

namespace facts = veritas::facts;

core::StableId StableId(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(kind, std::as_bytes(std::span(text.data(), text.size())));
}

facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine,
                                        std::string toolchain_identity,
                                        std::string_view revision) {
  facts::AnalysisRunDescriptor d;
  d.revision_id = StableId(core::IdKind::kRevision, revision);
  d.build_variant_id = StableId(core::IdKind::kBuildVariant, "bv");
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'b');
  d.engine = engine;
  d.engine_toolchain_identity = std::move(toolchain_identity);
  return std::move(facts::MakeAnalysisRun(d)).value();
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-qual-XXXXXX")
          .string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// --- Executor-level failure injection (fails closed, no evaluation) ---

TEST(WpaFailureQualificationTest, RejectsMismatchedEngineIdentity) {
  SouffleWpaExecutor executor("/nonexistent/veritas-souffle-worker",
                              "souffle-toolchain");
  WpaExecutionEnvelope envelope;
  envelope.run = MakeManifest(facts::EngineIdentity::kCppConformance,
                              "cpp-toolchain", "rev");
  auto result = executor.Execute(envelope, WpaExecutionLimits{});
  ASSERT_FALSE(result.ok());
}

TEST(WpaFailureQualificationTest, MissingWorkerFailsClosed) {
  SouffleWpaExecutor executor("/nonexistent/veritas-souffle-worker",
                              "souffle-toolchain");
  WpaExecutionEnvelope envelope;
  envelope.run =
      MakeManifest(facts::EngineIdentity::kSouffle, "souffle-toolchain", "rev");
  auto result = executor.Execute(envelope, WpaExecutionLimits{});
  ASSERT_FALSE(result.ok());
}

TEST(WpaFailureQualificationTest, TimeoutFailsClosed) {
  SouffleWpaExecutor executor(VERITAS_SOUFFLE_WORKER, "souffle-toolchain");
  WpaExecutionEnvelope envelope;
  envelope.run =
      MakeManifest(facts::EngineIdentity::kSouffle, "souffle-toolchain", "rev");
  WpaExecutionLimits limits;
  limits.timeout = std::chrono::milliseconds(1);
  auto result = executor.Execute(envelope, limits);
  ASSERT_FALSE(result.ok());
}

// --- Failure atomicity aggregate: a failed run never replaces prior success ---

TEST(WpaFailureQualificationTest, FailedRunDoesNotReplacePriorSuccess) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  const auto run_a =
      MakeManifest(facts::EngineIdentity::kSouffle, "souffle-toolchain", "rev-a");
  const auto run_b =
      MakeManifest(facts::EngineIdentity::kSouffle, "souffle-toolchain", "rev-b");

  const WpaComponentKey key{StableId(core::IdKind::kScc, "scc"),
                            WpaComponentKind::kReachability};

  // Run A succeeds and publishes a component result.
  ASSERT_TRUE(repo->BeginRun(run_a).ok());
  WpaComponentResult result;
  result.scc_id = key.scc_id;
  result.component = key.component;
  result.logical_input_hash = "logical-a";
  result.fixpoint_hash = std::string(64, 'a');
  result.external_hash = std::string(64, 'b');
  auto stored = repo->StoreSuccessfulComponent(run_a, key, result);
  ASSERT_TRUE(stored.ok()) << stored.status().message();
  ASSERT_TRUE(repo->CompleteRun(run_a).ok());

  // Run B fails the same component and marks the run incomplete.
  ASSERT_TRUE(repo->BeginRun(run_b).ok());
  ASSERT_TRUE(repo->RecordComponentFailure(run_b, key, "injected failure").ok());
  ASSERT_TRUE(repo->MarkIncomplete(run_b).ok());

  // Run B is incomplete; run A's success is retained and still queryable.
  auto status_b = repo->RunStatus(run_b.run_id);
  ASSERT_TRUE(status_b.ok());
  EXPECT_EQ(*status_b, WpaRunStatus::kIncomplete);

  auto status_a = repo->RunStatus(run_a.run_id);
  ASSERT_TRUE(status_a.ok());
  EXPECT_EQ(*status_a, WpaRunStatus::kComplete);

  auto object_key = repo->ResultObjectKey(run_a.run_id, key);
  ASSERT_TRUE(object_key.ok());
  EXPECT_TRUE(object_key->has_value());

  std::filesystem::remove_all(db);
}

}  // namespace
}  // namespace veritas::wpa
