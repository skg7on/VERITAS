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

#include "veritas/analysis/ProjectAnalyzer.h"

#include <gtest/gtest.h>

namespace veritas::analysis {
namespace {

// The default profile is `baseline`, which is today's behaviour. This is the
// whole safety argument for adding the field at all: an existing caller that
// constructs `AnalysisConfig::Default()` and knows nothing about scale profiles
// gets exactly the run it got before this milestone.
TEST(AnalysisConfigTest, DefaultProfileIsBaseline) {
  const AnalysisConfig config = AnalysisConfig::Default();
  EXPECT_EQ(config.scale_profile, ScaleProfile::kBaseline);
}

// `kBaseline` must be the first enumerator so that a zero-initialised
// `AnalysisConfig` is also `baseline`. Any other order silently makes
// `AnalysisConfig{}` mean `scaled`.
TEST(AnalysisConfigTest, BaselineIsTheZeroEnumerator) {
  EXPECT_EQ(static_cast<int>(ScaleProfile::kBaseline), 0);
}

TEST(AnalysisConfigTest, ProfileIsAssignable) {
  AnalysisConfig config = AnalysisConfig::Default();
  config.scale_profile = ScaleProfile::kScaled;
  EXPECT_EQ(config.scale_profile, ScaleProfile::kScaled);
}

}  // namespace
}  // namespace veritas::analysis
