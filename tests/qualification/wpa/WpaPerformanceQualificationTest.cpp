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

// WpaPerformanceQualificationTest.cpp — repeated production Souffle analysis
// of a recursive fixture stays within the checked-in resource ceilings and
// produces the same WPA run identity every time.

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::analysis {
namespace {

// Mirrors tests/qualification/wpa/performance-ceilings.json. The ceiling is
// deliberately generous: it must fail on an order-of-magnitude regression, not
// on ordinary machine noise.
constexpr long long kMaxMedianWallMs = 30000;
constexpr long long kMaxPeakRssMb = 2048;

long long CurrentPeakRssMb() {
  struct rusage usage {};
  getrusage(RUSAGE_SELF, &usage);
  // ru_maxrss is bytes on macOS (darwin), kilobytes on Linux.
#if defined(__APPLE__)
  return static_cast<long long>(usage.ru_maxrss) / (1024 * 1024);
#else
  return static_cast<long long>(usage.ru_maxrss) / 1024;
#endif
}

ProjectAnalysisRequest FixtureRequest() {
  const auto root = testing::FixtureProject("recursive_calls");
  return ProjectAnalysisRequest{.project_root = root,
                                .output_root = root / ".veritas"};
}

TEST(WpaPerformanceQualificationTest, WithinCeilingsAndDeterministic) {
  constexpr int kIterations = 5;

  std::vector<long long> wall_ms;
  std::vector<std::string> run_ids;

  for (int i = 0; i < kIterations; ++i) {
    ProjectAnalyzer analyzer;
    const auto start = std::chrono::steady_clock::now();
    auto result = analyzer.AnalyzeProject(FixtureRequest(),
                                          AnalysisConfig::Default());
    const auto end = std::chrono::steady_clock::now();
    ASSERT_TRUE(result.ok()) << result.status().message();

    wall_ms.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - start)
                          .count());
    run_ids.push_back(result->wpa_run_id);

    // Production Souffle is the engine for every run.
    EXPECT_EQ(result->wpa_engine, WpaEngineMode::kSouffle);
    // The same semantic input yields the same WPA run identity every time.
    if (i > 0) {
      EXPECT_EQ(run_ids[i], run_ids[0]);
    }
  }

  std::sort(wall_ms.begin(), wall_ms.end());
  const long long median_wall = wall_ms[wall_ms.size() / 2];
  EXPECT_LT(median_wall, kMaxMedianWallMs);

  const long long peak_rss = CurrentPeakRssMb();
  EXPECT_LT(peak_rss, kMaxPeakRssMb);

  std::printf("WpaPerformanceQualificationTest: median wall %lld ms, peak RSS "
              "%lld MB\n",
              median_wall, peak_rss);
}

}  // namespace
}  // namespace veritas::analysis
