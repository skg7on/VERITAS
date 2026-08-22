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

#include "veritas/facts/LegacyFactAdapter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::facts;

namespace {

core::StableId Stable(core::IdKind kind, std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(kind, bytes);
}

AnalysisRunManifest ValidRun() {
  AnalysisRunDescriptor d;
  d.revision_id = Stable(core::IdKind::kRevision, 0x01);
  d.build_variant_id = Stable(core::IdKind::kBuildVariant, 0x02);
  d.summary_schema_version = "summary.v1-compat-v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "wpa.rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'a');
  d.engine = EngineIdentity::kSouffle;
  d.engine_toolchain_identity = "souffle-2.5+canonical-build";
  return *MakeAnalysisRun(d);
}

summary::v1::FunctionSummary LegacySummaryWithCall(
    summary::v1::EpistemicState state) {
  summary::v1::FunctionSummary summary;
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(Stable(core::IdKind::kFunctionVariant, 0x10)));
  auto* call = summary.add_calls();
  call->set_call_site_anchor_id("anchor");
  call->set_resolved_callee_function_variant_id(
      core::ToString(Stable(core::IdKind::kFunctionVariant, 0x20)));
  call->set_epistemic(state);
  return summary;
}

}  // namespace

TEST(LegacyFactAdapterTest, PreservesNegativeAndUnknownEpistemicStates) {
  for (auto state : {summary::v1::EPISTEMIC_STATE_MUST_NOT,
                     summary::v1::EPISTEMIC_STATE_INFERRED,
                     summary::v1::EPISTEMIC_STATE_ASSUMED,
                     summary::v1::EPISTEMIC_STATE_UNKNOWN}) {
    std::vector<summary::v1::FunctionSummary> summaries{
        LegacySummaryWithCall(state)};
    auto projection = ProjectLegacySummaries(ValidRun(), summaries);
    ASSERT_TRUE(projection.ok());
    EXPECT_FALSE(projection->rows.empty());
  }
}
