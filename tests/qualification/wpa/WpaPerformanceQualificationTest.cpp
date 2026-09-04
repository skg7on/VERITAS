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

// WpaPerformanceQualificationTest.cpp — the recursive_calls fixture completes
// under the checked-in resource ceilings and reproduces its semantic hashes
// across warmed iterations.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <sys/resource.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::testing {
namespace {

namespace analysis = veritas::analysis;

// Checked-in performance ceilings (see the qualification design spec §10).
constexpr std::int64_t kMaximumWallTimeMs = 30000;
constexpr std::int64_t kMaximumPeakRssMb = 2048;

std::int64_t PeakRssMb() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  // Darwin reports ru_maxrss in bytes.
  return static_cast<std::int64_t>(usage.ru_maxrss) / (1024 * 1024);
#else
  // Linux reports ru_maxrss in kibibytes.
  return static_cast<std::int64_t>(usage.ru_maxrss) / 1024;
#endif
}

TEST(WpaPerformanceQualificationTest, RecursiveCallsWithinResourceCeilings) {
  const std::filesystem::path root = FixtureProject("recursive_calls");
  const analysis::ProjectAnalysisRequest request{
      .project_root = root,
      .output_root = root / ".veritas",
  };

  analysis::ProjectAnalyzer analyzer;

  // One warm-up run so the first measured iteration is not dominated by
  // one-time initialization (Souffle worker spawn, SVF registry setup).
  auto warm = analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default());
  ASSERT_TRUE(warm.ok()) << warm.status().message();

  const auto started = std::chrono::steady_clock::now();
  auto result = analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default());
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started);
  ASSERT_TRUE(result.ok()) << result.status().message();

  EXPECT_FALSE(result->wpa_run_id.empty());
  EXPECT_FALSE(result->published_summary_ids.empty());

  // Resource ceilings: the test fails only when a checked-in ceiling is
  // exceeded, never on absolute timing.
  EXPECT_LT(elapsed.count(), kMaximumWallTimeMs)
      << "wall time " << elapsed.count() << "ms exceeded "
      << kMaximumWallTimeMs << "ms";
  const auto peak_rss_mb = PeakRssMb();
  EXPECT_LT(peak_rss_mb, kMaximumPeakRssMb)
      << "peak RSS " << peak_rss_mb << "MB exceeded "
      << kMaximumPeakRssMb << "MB";

  // Warmed iterations must reproduce the same run identity.
  for (int i = 0; i < 3; ++i) {
    auto again = analyzer.AnalyzeProject(request, analysis::AnalysisConfig::Default());
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again->wpa_run_id, result->wpa_run_id);
  }
}

}  // namespace
}  // namespace veritas::testing
