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

// SouffleWpaExecutorTest.cpp — failure and timeout behavior of the production
// executor. A timeout, a missing worker, or a mismatched engine identity all
// fail with no evaluation, so a failed component can never publish a result.

#include <chrono>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {
namespace {

facts::AnalysisRunManifest ManifestFor(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor d;
  d.revision_id = core::MakeStableId(core::IdKind::kRevision,
                                     std::as_bytes(std::span("rev", 3)));
  d.build_variant_id = core::MakeStableId(core::IdKind::kBuildVariant,
                                          std::as_bytes(std::span("bv", 2)));
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'b');
  d.engine = engine;
  d.engine_toolchain_identity = "test-toolchain";
  return std::move(facts::MakeAnalysisRun(d)).value();
}

TEST(SouffleWpaExecutorTest, RejectsEnvelopeWithNonSouffleEngine) {
  SouffleWpaExecutor executor("/nonexistent/veritas-souffle-worker");
  WpaExecutionEnvelope envelope;
  envelope.run = ManifestFor(facts::EngineIdentity::kCppConformance);
  auto result = executor.Execute(envelope, WpaExecutionLimits{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
}

TEST(SouffleWpaExecutorTest, MissingWorkerReturnsError) {
  SouffleWpaExecutor executor("/nonexistent/veritas-souffle-worker");
  WpaExecutionEnvelope envelope;
  envelope.run = ManifestFor(facts::EngineIdentity::kSouffle);
  auto result = executor.Execute(envelope, WpaExecutionLimits{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInternal);
}

TEST(SouffleWpaExecutorTest, TimeoutReturnsDeadlineExceeded) {
  SouffleWpaExecutor executor(VERITAS_SOUFFLE_WORKER);
  WpaExecutionEnvelope envelope;
  envelope.run = ManifestFor(facts::EngineIdentity::kSouffle);
  WpaExecutionLimits limits;
  limits.timeout = std::chrono::milliseconds(1);
  auto result = executor.Execute(envelope, limits);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kDeadlineExceeded);
}

}  // namespace
}  // namespace veritas::wpa
