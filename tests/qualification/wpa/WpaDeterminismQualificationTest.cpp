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

// WpaDeterminismQualificationTest.cpp — the same input yields the same
// identities across repeated in-process analyses.
//
// IDs are content-derived and insertion-order independent, so analyzing a
// fixture repeatedly must reproduce the same revision/build-variant context,
// the same published summary IDs, and the same WPA run ID. This is the
// relations-v2 qualification: dense/stable mapping and component ownership
// must not drift across runs.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::testing {
namespace {

namespace analysis = veritas::analysis;

// Runs the full M1->M4->M5->M3->WPA pipeline repeatedly on a stable fixture
// path and asserts every identity field is reproduced exactly.
void ExpectRepeatedAnalysisDeterministic(std::string_view fixture) {
  const std::filesystem::path root = FixtureProject(fixture);
  const analysis::ProjectAnalysisRequest request{
      .project_root = root,
      .output_root = root / ".veritas",
  };

  analysis::ProjectAnalyzer analyzer;
  auto first = analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default());
  ASSERT_TRUE(first.ok()) << first.status().message();

  constexpr int kRuns = 5;
  for (int i = 0; i < kRuns; ++i) {
    auto again = analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default());
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again->revision_id, first->revision_id);
    EXPECT_EQ(again->build_variant_id, first->build_variant_id);
    EXPECT_EQ(again->published_summary_ids, first->published_summary_ids);
    EXPECT_EQ(again->wpa_run_id, first->wpa_run_id);
    EXPECT_EQ(again->wpa_engine, first->wpa_engine);
    EXPECT_EQ(again->wpa_diagnostics, first->wpa_diagnostics);
  }

  // A completed production run must expose a durable identity.
  EXPECT_FALSE(first->wpa_run_id.empty());
  EXPECT_FALSE(first->published_summary_ids.empty());
}

TEST(WpaDeterminismQualificationTest, RecursiveCallsAnalysisIsDeterministic) {
  ExpectRepeatedAnalysisDeterministic("recursive_calls");
}

// semantic_zoo is intentionally not driven through this aggregate yet: its
// virtual-dispatch and callback MAY edges produce multiple alternative proofs
// that ResultCanonicalizer currently rejects as "witness binds two inputs at
// one ordinal". Re-enable once the canonicalizer selects a canonical proof for
// multi-target MAY edges instead of rejecting them.


}  // namespace
}  // namespace veritas::testing
