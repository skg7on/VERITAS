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

#include "veritas/facts/AnalysisRun.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

using namespace veritas;
using veritas::core::IdKind;

namespace {

core::StableId Stable(core::IdKind kind, std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(kind, bytes);
}

facts::AnalysisRunDescriptor ValidDescriptor() {
  facts::AnalysisRunDescriptor d;
  d.revision_id = Stable(IdKind::kRevision, 0x01);
  d.build_variant_id = Stable(IdKind::kBuildVariant, 0x02);
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "wpa.rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'a');
  d.engine = facts::EngineIdentity::kSouffle;
  d.engine_toolchain_identity = "souffle-2.5+canonical-build";
  return d;
}

}  // namespace

TEST(AnalysisRunTest, EveryDescriptorFieldChangesRunId) {
  auto base = ValidDescriptor();
  ASSERT_TRUE(MakeAnalysisRun(base).ok());
  const auto base_id = MakeAnalysisRun(base)->run_id;

  auto changed = base;
  changed.engine = facts::EngineIdentity::kCppEmergency;
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.rule_bundle_version = "wpa.rules.v2.1";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.model_bundle_version = "models.v2";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.engine_toolchain_identity = "souffle-2.5+other-build";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);
}

TEST(AnalysisRunTest, RejectsEmptyVersionOrConfigurationFields) {
  auto descriptor = ValidDescriptor();
  descriptor.relation_schema_version.clear();
  EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
            StatusCode::kInvalidArgument);
}

TEST(AnalysisRunTest, RejectsWrongIdKinds) {
  auto descriptor = ValidDescriptor();
  descriptor.revision_id = Stable(IdKind::kBuildVariant, 0x03);
  EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
            StatusCode::kInvalidArgument);

  descriptor = ValidDescriptor();
  descriptor.build_variant_id = Stable(IdKind::kRevision, 0x04);
  EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
            StatusCode::kInvalidArgument);
}

TEST(AnalysisRunTest, RejectsNonCanonicalConfigurationHashes) {
  for (std::string bad : {
           std::string(63, 'a'),          // too short
           std::string(64, 'g'),          // non-hex
           std::string(64, 'A'),          // uppercase
           std::string(64, 'a') + "a",    // too long
       }) {
    auto descriptor = ValidDescriptor();
    descriptor.svf_configuration_hash = bad;
    EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
              StatusCode::kInvalidArgument);
  }
}

TEST(AnalysisRunTest, RejectsUnrecognizedEngine) {
  auto descriptor = ValidDescriptor();
  descriptor.engine = static_cast<facts::EngineIdentity>(99);
  EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
            StatusCode::kInvalidArgument);
}
