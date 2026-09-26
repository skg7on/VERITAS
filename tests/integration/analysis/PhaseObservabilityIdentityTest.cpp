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

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "ProjectFixture.h"

namespace veritas::analysis {
namespace {

namespace fs = std::filesystem;

TEST(PhaseObservabilityIdentityTest, RecordingDoesNotMoveAnyIdentity) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output_a = fs::temp_directory_path() /
                        ("veritas-metrics-off-" + std::to_string(std::rand()));
  const auto output_b = fs::temp_directory_path() /
                        ("veritas-metrics-on-" + std::to_string(std::rand()));

  auto request_a = veritas::analysis::ProjectAnalysisRequest{
      .project_root = project, .output_root = output_a};
  auto request_b = veritas::analysis::ProjectAnalysisRequest{
      .project_root = project, .output_root = output_b};
  const auto config = veritas::analysis::AnalysisConfig::Default();

  veritas::analysis::ProjectAnalyzer analyzer_a;
  auto result_a = analyzer_a.AnalyzeProject(request_a, config);
  ASSERT_TRUE(result_a.ok()) << result_a.status().message();

  veritas::core::RunMetricsOptions options;
  veritas::core::RunMetrics metrics(options);
  veritas::analysis::ProjectAnalyzer analyzer_b;
  auto result_b = analyzer_b.AnalyzeProject(request_b, config, &metrics);
  ASSERT_TRUE(result_b.ok()) << result_b.status().message();

  EXPECT_EQ(result_a->revision_id, result_b->revision_id);
  EXPECT_EQ(result_a->build_variant_id, result_b->build_variant_id);
  EXPECT_EQ(result_a->program_context_id, result_b->program_context_id);
  EXPECT_EQ(result_a->projection_id, result_b->projection_id);
  EXPECT_EQ(result_a->wpa_run_id, result_b->wpa_run_id);
  EXPECT_EQ(result_a->published_summary_ids, result_b->published_summary_ids);
  EXPECT_EQ(result_a->cpg_node_count, result_b->cpg_node_count);
  EXPECT_EQ(result_a->cpg_edge_count, result_b->cpg_edge_count);
  EXPECT_EQ(result_a->unknowns.size(), result_b->unknowns.size());
  // The recorder actually ran, so an empty tree cannot make this vacuous.
  EXPECT_FALSE(metrics.TakeStats().root.children.empty());
}

// Instrumentation can compile, run, and record nothing: a wrong null check, a
// name that does not resolve where the site assumed, a span opened in a scope
// that already returned. No analysis result moves either way, so this case
// asserts the recorded tree itself -- presence and counts, never durations, so
// it cannot flake.
TEST(PhaseObservabilityIdentityTest, RecordsTheExpectedSpanTree) {
  const auto project = testing::FixtureProject("multiple_tus");
  // A root unique to this process, as `FixtureProject` and the WPA test
  // fixtures both provide. A name derived from `std::rand()` is not enough:
  // the call is unseeded, so every run of this invocation shape resolves to
  // the same path. The component-result cache is keyed on the output root, so
  // a root left behind by an earlier run turns the per-component execute span
  // into a cache hit -- and this case would then fail on its second run for a
  // reason that has nothing to do with the instrumentation it exists to prove.
  std::string output_template =
      (fs::temp_directory_path() / "veritas-metrics-spans-XXXXXX").string();
  char* created = ::mkdtemp(output_template.data());
  ASSERT_NE(created, nullptr) << "cannot create an output root under "
                              << fs::temp_directory_path();
  const fs::path output = created;
  const ProjectAnalysisRequest request{.project_root = project,
                                       .output_root = output};

  core::RunMetricsOptions options;
  core::RunMetrics metrics(options);
  ProjectAnalyzer analyzer;
  auto result =
      analyzer.AnalyzeProject(request, AnalysisConfig::Default(), &metrics);
  ASSERT_TRUE(result.ok()) << result.status().message();

  const core::RunMetricsStats stats = metrics.TakeStats();

  std::map<std::string, std::uint64_t> counts;
  std::function<void(const core::SpanStats&)> walk =
      [&](const core::SpanStats& node) {
        counts[node.name] += node.count;
        for (const core::SpanStats& child : node.children) walk(child);
      };
  walk(stats.root);

  // A span that never opens shows up here as an absent key, not as a zero.
  for (const char* name : {"m1.ingest", "m4.local_analysis", "m5.svf",
                           "m5.model_bundle_load", "m5.merge_svf_facts",
                           "m6.cpg_projection", "m2m3.publish_summaries",
                           "wpa.orchestrate", "wpa.graph_build",
                           "facts.batch_assemble", "facts.store_open",
                           "facts.publish", "facts.publish.validate"}) {
    EXPECT_GT(counts[name], 0u) << name << " never opened";
  }
  // The SVF session runs its own five numbered steps.
  for (const char* name : {"m5.svf.module_set", "m5.svf.svfi",
                           "m5.svf.andersen", "m5.svf.svfg",
                           "m5.svf.map_facts"}) {
    EXPECT_GT(counts[name], 0u) << name << " never opened";
  }
  // The per-component spans are distributed.
  for (const char* name : {"wpa.component.materialize",
                           "wpa.component.cache_lookup",
                           "wpa.component.execute",
                           "wpa.component.canonicalize"}) {
    EXPECT_GT(counts[name], 0u) << name << " never opened";
  }

  std::map<std::string, std::uint64_t> counters;
  for (const core::Counter& counter : stats.counters) {
    counters[counter.name] = counter.value;
  }
  for (const char* name : {"svf.svfg_nodes", "wpa.components.expected",
                           "wpa.components.reused", "wpa.components.executed",
                           "facts.rooted_input", "facts.canonical"}) {
    EXPECT_EQ(counters.count(name), 1u) << name << " missing";
  }
  EXPECT_GT(counters["wpa.components.expected"], 0u);
  EXPECT_GT(counters["svf.svfg_nodes"], 0u);
}

}  // namespace
}  // namespace veritas::analysis
