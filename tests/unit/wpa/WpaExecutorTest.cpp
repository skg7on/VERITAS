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

#include "veritas/wpa/CppConformanceExecutor.h"

#include <span>
#include <string>

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisRun.h"

namespace veritas::wpa {
namespace {

facts::AnalysisRunManifest ManifestFor(facts::EngineIdentity engine,
                                       std::string toolchain_identity) {
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
  d.engine_toolchain_identity = std::move(toolchain_identity);
  return std::move(facts::MakeAnalysisRun(d)).value();
}

TEST(WpaExecutorTest, CppExecutorRequiresNonProductionIdentity) {
  // The C++ engine must never carry the production Souffle identity.
  EXPECT_FALSE(CppConformanceExecutor::Create(
                   facts::EngineIdentity::kSouffle, "souffle-toolchain")
                   .ok());
  EXPECT_TRUE(CppConformanceExecutor::Create(
                  facts::EngineIdentity::kCppConformance, "cpp-conformance")
                  .ok());
  EXPECT_TRUE(CppConformanceExecutor::Create(
                  facts::EngineIdentity::kCppEmergency, "cpp-emergency")
                  .ok());
}

TEST(WpaExecutorTest, CppExecutorReportsItsIdentity) {
  auto conformance = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppConformance, "cpp-conformance");
  ASSERT_TRUE(conformance.ok());
  EXPECT_EQ(conformance->identity(), facts::EngineIdentity::kCppConformance);
  EXPECT_EQ(conformance->toolchain_identity(), "cpp-conformance");

  auto emergency = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppEmergency, "cpp-emergency");
  ASSERT_TRUE(emergency.ok());
  EXPECT_EQ(emergency->identity(), facts::EngineIdentity::kCppEmergency);
  EXPECT_EQ(emergency->toolchain_identity(), "cpp-emergency");
}

TEST(WpaExecutorTest, CppExecutorRejectsDifferentToolchainIdentity) {
  auto executor = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppConformance, "expected-toolchain");
  ASSERT_TRUE(executor.ok());
  WpaExecutionEnvelope envelope;
  envelope.run = ManifestFor(facts::EngineIdentity::kCppConformance,
                             "different-toolchain");

  auto result = executor->Execute(envelope, WpaExecutionLimits{});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace veritas::wpa
