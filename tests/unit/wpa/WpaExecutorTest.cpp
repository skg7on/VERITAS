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

#include <gtest/gtest.h>

#include "veritas/facts/AnalysisRun.h"

namespace veritas::wpa {
namespace {

TEST(WpaExecutorTest, CppExecutorRequiresNonProductionIdentity) {
  // The C++ engine must never carry the production Souffle identity.
  EXPECT_FALSE(
      CppConformanceExecutor::Create(facts::EngineIdentity::kSouffle).ok());
  EXPECT_TRUE(CppConformanceExecutor::Create(
                  facts::EngineIdentity::kCppConformance)
                  .ok());
  EXPECT_TRUE(
      CppConformanceExecutor::Create(facts::EngineIdentity::kCppEmergency).ok());
}

TEST(WpaExecutorTest, CppExecutorReportsItsIdentity) {
  auto conformance =
      CppConformanceExecutor::Create(facts::EngineIdentity::kCppConformance);
  ASSERT_TRUE(conformance.ok());
  EXPECT_EQ(conformance->identity(), facts::EngineIdentity::kCppConformance);

  auto emergency =
      CppConformanceExecutor::Create(facts::EngineIdentity::kCppEmergency);
  ASSERT_TRUE(emergency.ok());
  EXPECT_EQ(emergency->identity(), facts::EngineIdentity::kCppEmergency);
}

}  // namespace
}  // namespace veritas::wpa
