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

// The renderer contract of spec section 8.3: rendering the same report twice is
// byte-identical, the artifact carries no floats and no absolute paths, and it
// parses against the versioned schema. The fixture's numbers are arbitrary —
// these cases pin the format rules, not the measurements.

#include "veritas/observability/RunReport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <llvm/Support/JSON.h>

namespace veritas::observability {
namespace {

// Counts terminal columns: the tree's box-drawing characters are one column
// each whatever their byte length, so this is the measure a padded label column
// has to use.
std::size_t DisplayColumns(const std::string& text) {
  std::size_t columns = 0;
  for (const char c : text) {
    // 0b10xxxxxx is a UTF-8 continuation byte; every other byte opens a
    // character, and every character in a report line is one column wide.
    if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++columns;
  }
  return columns;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\n') continue;
    lines.push_back(text.substr(start, i - start));
    start = i + 1;
  }
  if (start < text.size()) lines.push_back(text.substr(start));
  return lines;
}

RunReport MakeFixtureReport() {
  RunReport report;
  report.metrics_options.top_n = 10;
  report.identity.run_id = "run:sha256:0000";
  report.environment.os = "test";
  report.inventory.output.cpg_nodes = 12;
  // Reversed on purpose: "z" precedes "a" here, and "reachability" precedes
  // "effects". A renderer that emits insertion order fails the sort test.
  report.metrics.counters = {
      core::Counter{"z.counter", 3, "count"},
      core::Counter{"a.counter", 1, "count"},
  };
  report.inventory.output.components_by_kind = {{"reachability", 2},
                                                {"effects", 1}};
  report.store.tables = {{"analysis_facts", 1249792}};

  core::SpanStats parent;
  parent.name = "run";
  parent.count = 1;
  parent.wall_inclusive = std::chrono::seconds(2);
  parent.wall_self = std::chrono::seconds(1);

  core::SpanStats child;
  child.name = "m5.svf";
  child.count = 1;
  child.wall_inclusive = std::chrono::seconds(1);
  child.wall_self = std::chrono::seconds(1);
  parent.children.push_back(child);

  core::SpanStats distributed;
  distributed.name = "wpa.component.execute";
  distributed.count = 3;
  core::Distribution distribution;
  distribution.p50 = std::chrono::milliseconds(2);
  distribution.p95 = std::chrono::milliseconds(9);
  distribution.p99 = std::chrono::milliseconds(9);
  distribution.max = std::chrono::milliseconds(9);
  distributed.distribution = distribution;
  distributed.top_n.push_back(
      core::TopEntry{"flow/scc:sha256:aaaa", std::chrono::milliseconds(9)});
  parent.children.push_back(distributed);

  report.metrics.root = parent;
  report.metrics.series = {{std::chrono::milliseconds(0), 100, 90},
                           {std::chrono::milliseconds(100), 200, 180}};
  // A producer that collected samples says so, and that is what the run-level
  // memory block keys on rather than the presence of the published series.
  report.metrics.memory_measured = true;
  return report;
}

TEST(RunReportTest, RendersSameBytesTwice) {
  RunReport report = MakeFixtureReport();
  EXPECT_EQ(RenderRunReportJson(report), RenderRunReportJson(report));
}

TEST(RunReportTest, EmitsOnlyIntegerNumbersOutsideStrings) {
  const std::string json = RenderRunReportJson(MakeFixtureReport());
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    ASSERT_NE(c, '.') << "float token near offset " << i << ": "
                      << json.substr(i > 12 ? i - 12 : 0, 25);
  }
}

TEST(RunReportTest, ContainsNoAbsolutePath) {
  const std::string json = RenderRunReportJson(MakeFixtureReport());
  EXPECT_EQ(json.find("/tmp/"), std::string::npos);
  EXPECT_EQ(json.find("/Users/"), std::string::npos);
}

TEST(RunReportTest, SortsCountersAndComponentKindsByName) {
  // The fixture supplies these out of order on purpose, so a renderer that
  // emits insertion order fails here. Spec section 6.5 rule 1.
  const std::string json = RenderRunReportJson(MakeFixtureReport());
  const std::size_t first = json.find("a.counter");
  const std::size_t last = json.find("z.counter");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(last, std::string::npos);
  EXPECT_LT(first, last);

  const std::size_t effects = json.find("effects");
  const std::size_t reachability = json.find("reachability");
  ASSERT_NE(effects, std::string::npos);
  ASSERT_NE(reachability, std::string::npos);
  EXPECT_LT(effects, reachability);
}

TEST(RunReportTest, JsonParsesAndCarriesTheSchemaVersion) {
  const std::string json = RenderRunReportJson(MakeFixtureReport());
  auto parsed = llvm::json::parse(json);
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(*root->getString("schema"), "veritas.run-metrics.v1");
}

TEST(RunReportTest, JsonCarriesEverySchemaBlockWithItsExpectedType) {
  // The versioned-schema contract of spec section 8.3: a reader that asks for a
  // block by name and type must find it. A missing block is the failure this
  // case exists for.
  const std::string json = RenderRunReportJson(MakeFixtureReport());
  auto parsed = llvm::json::parse(json);
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);

  EXPECT_TRUE(root->getBoolean("complete").has_value());
  EXPECT_NE(root->getArray("counters"), nullptr);
  EXPECT_NE(root->getArray("diagnostics"), nullptr);
  EXPECT_NE(root->getArray("phases"), nullptr);
  EXPECT_NE(root->getObject("config"), nullptr);
  EXPECT_NE(root->getObject("environment"), nullptr);
  EXPECT_NE(root->getObject("identity"), nullptr);
  EXPECT_NE(root->getObject("memory"), nullptr);
  EXPECT_NE(root->getObject("store"), nullptr);

  const llvm::json::Object* inventory = root->getObject("inventory");
  ASSERT_NE(inventory, nullptr);
  EXPECT_NE(inventory->getObject("input"), nullptr);
  EXPECT_NE(inventory->getObject("output"), nullptr);
  EXPECT_NE(inventory->getObject("incrementality"), nullptr);

  // The tree is emitted as a one-element forest, and `counters` carries both
  // counters the fixture supplied, sorted by name.
  ASSERT_EQ(root->getArray("phases")->size(), 1u);
  ASSERT_EQ(root->getArray("counters")->size(), 2u);
  const llvm::json::Value* first_counter = &root->getArray("counters")->front();
  const llvm::json::Object* first_counter_object = first_counter->getAsObject();
  ASSERT_NE(first_counter_object, nullptr);
  EXPECT_EQ(*first_counter_object->getString("name"), "a.counter");

  const llvm::json::Object* root_phase =
      root->getArray("phases")->front().getAsObject();
  ASSERT_NE(root_phase, nullptr);
  EXPECT_EQ(*root_phase->getString("name"), "run");
  EXPECT_EQ(*root_phase->getInteger("wall_inclusive_ns"), 2000000000);
}

TEST(RunReportTest, FormatDurationRendersTheDocumentedUnits) {
  EXPECT_EQ(FormatDuration(std::chrono::nanoseconds(0)), "0ns");
  EXPECT_EQ(FormatDuration(std::chrono::nanoseconds(1234567890)), "1.235s");
  EXPECT_EQ(FormatDuration(std::chrono::nanoseconds(1000000000)), "1.000s");
  // Below one second the unit switches to milliseconds, still three decimals.
  EXPECT_EQ(FormatDuration(std::chrono::nanoseconds(1500000)), "1.500ms");
  EXPECT_EQ(FormatDuration(std::chrono::nanoseconds(999999)), "1.000ms");
}

TEST(RunReportTest, FormatBytesRendersTheDocumentedUnits) {
  EXPECT_EQ(FormatBytes(9223372036), "8.59 GiB");
  EXPECT_EQ(FormatBytes(1073741824), "1.00 GiB");
  // Below one GiB the unit is MiB, with two decimals, not a rounded-up GiB.
  EXPECT_EQ(FormatBytes(1073741823), "1024.00 MiB");
  EXPECT_EQ(FormatBytes(5242880), "5.00 MiB");
  EXPECT_EQ(FormatBytes(1048576), "1.00 MiB");
  // Below one MiB the unit is KiB, with one decimal. This is the tier that
  // keeps a phase that allocates a few hundred kilobytes from reading as
  // "0.00 MiB" — nothing measured — rather than as small.
  EXPECT_EQ(FormatBytes(1048575), "1024.0 KiB");
  EXPECT_EQ(FormatBytes(2048), "2.0 KiB");
  EXPECT_EQ(FormatBytes(1024), "1.0 KiB");
  // Below one KiB the byte count is printed as it is.
  EXPECT_EQ(FormatBytes(1023), "1023 B");
  EXPECT_EQ(FormatBytes(1), "1 B");
  EXPECT_EQ(FormatBytes(0), "0 B");
}

TEST(RunReportTest, TextReportKeepsSmallMemoryDeltasVisible) {
  // The delta column scales like the peak column, so a MiB-scale release is not
  // rounded away to a bare "+0.00" beside a GiB-scale peak.
  RunReport report;
  report.metrics.root.name = "run";
  report.metrics.root.count = 1;
  report.metrics.root.memory =
      core::SpanMemory{0, 0, 3 * 1024 * 1024, 0, 3 * 1024 * 1024};
  const std::string grown = RenderRunReportText(report);
  // Both columns, so the reader can compare them in the same units.
  EXPECT_NE(grown.find("3.00 MiB"), std::string::npos) << grown;
  EXPECT_NE(grown.find("+3.00 MiB"), std::string::npos) << grown;

  report.metrics.root.memory = core::SpanMemory{0, 0, 512, 0, -3145728};
  const std::string shrunk = RenderRunReportText(report);
  EXPECT_NE(shrunk.find("-3.00 MiB"), std::string::npos) << shrunk;

  // A release smaller than a byte count can express is still signed and spelled.
  report.metrics.root.memory = core::SpanMemory{0, 0, 512, 0, 512};
  EXPECT_NE(RenderRunReportText(report).find("+512 B"), std::string::npos);
  report.metrics.root.memory = core::SpanMemory{0, 0, 512, 0, 0};
  EXPECT_NE(RenderRunReportText(report).find("+0 B"), std::string::npos);
}

TEST(RunReportTest, EmitsRunMemoryOnlyWhenMemoryWasMeasured) {
  // The run-level memory block follows the same rule as a span's, and its
  // presence test is "was it measured", never "was the series emitted". No
  // measurement at all is the one case with no block; a zeroed peak or an
  // empty `series` array would both read as "measured".
  RunReport not_measured = MakeFixtureReport();
  not_measured.metrics.memory_measured = false;
  not_measured.metrics.series.clear();
  auto parsed = llvm::json::parse(RenderRunReportJson(not_measured));
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(root->getObject("memory"), nullptr);

  // The positive control: the same fixture having measured, and publishing,
  // still emits the block with the peak the producer set, so the assertion
  // above cannot pass by the block never being emitted at all.
  RunReport measured = MakeFixtureReport();
  measured.metrics.peak_rss_bytes = 200;
  measured.metrics.peak_footprint_bytes = 180;
  measured.metrics.peak_at = std::chrono::milliseconds(100);
  auto parsed_full = llvm::json::parse(RenderRunReportJson(measured));
  ASSERT_TRUE(static_cast<bool>(parsed_full));
  const llvm::json::Object* full_root = parsed_full->getAsObject();
  ASSERT_NE(full_root, nullptr);
  const llvm::json::Object* memory = full_root->getObject("memory");
  ASSERT_NE(memory, nullptr);
  const llvm::json::Object* peak = memory->getObject("peak");
  ASSERT_NE(peak, nullptr);
  const std::optional<std::int64_t> rss = peak->getInteger("rss_bytes");
  const std::optional<std::int64_t> footprint =
      peak->getInteger("footprint_bytes");
  const std::optional<std::int64_t> at_ms = peak->getInteger("at_ms");
  ASSERT_TRUE(rss.has_value());
  ASSERT_TRUE(footprint.has_value());
  ASSERT_TRUE(at_ms.has_value());
  EXPECT_EQ(*rss, 200);
  EXPECT_EQ(*footprint, 180);
  EXPECT_EQ(*at_ms, 100);
  const llvm::json::Array* series = memory->getArray("series");
  ASSERT_NE(series, nullptr);
  EXPECT_EQ(series->size(), 2u);
}

TEST(RunReportTest, EmitsRunMemoryPeakWhenTheSeriesIsSuppressed) {
  // --metrics-series false suppresses the samples, not the measurement: the
  // peak was measured and must survive, and the samples must be absent rather
  // than rendered as an empty array, which is a third thing again.
  RunReport report = MakeFixtureReport();
  report.metrics_options.emit_series = false;
  report.metrics.peak_rss_bytes = 200;
  report.metrics.peak_footprint_bytes = 180;
  report.metrics.peak_at = std::chrono::milliseconds(100);
  // What TakeStats leaves behind when it collected samples but suppressed
  // them: the buffer's policy still stands, the samples are not copied out.
  report.metrics.series.clear();
  report.metrics.series_decimation = 4;

  auto parsed = llvm::json::parse(RenderRunReportJson(report));
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const llvm::json::Object* memory = root->getObject("memory");
  ASSERT_NE(memory, nullptr);
  const llvm::json::Object* peak = memory->getObject("peak");
  ASSERT_NE(peak, nullptr);
  const std::optional<std::int64_t> rss = peak->getInteger("rss_bytes");
  const std::optional<std::int64_t> footprint =
      peak->getInteger("footprint_bytes");
  const std::optional<std::int64_t> at_ms = peak->getInteger("at_ms");
  ASSERT_TRUE(rss.has_value());
  ASSERT_TRUE(footprint.has_value());
  ASSERT_TRUE(at_ms.has_value());
  EXPECT_EQ(*rss, 200);
  EXPECT_EQ(*footprint, 180);
  EXPECT_EQ(*at_ms, 100);
  EXPECT_EQ(memory->getArray("series"), nullptr);
  const std::optional<std::int64_t> decimation =
      memory->getInteger("series_decimation");
  ASSERT_TRUE(decimation.has_value());
  EXPECT_EQ(*decimation, 4);
}

TEST(RunReportTest, EmitsTheConformanceOracleFlag) {
  // The block is a boolean, and it carries the field rather than a constant.
  RunReport report = MakeFixtureReport();
  report.conformance_oracle = true;
  auto parsed = llvm::json::parse(RenderRunReportJson(report));
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const llvm::json::Object* config = root->getObject("config");
  ASSERT_NE(config, nullptr);
  const std::optional<bool> oracle = config->getBoolean("conformance_oracle");
  ASSERT_TRUE(oracle.has_value());
  EXPECT_TRUE(*oracle);
}

TEST(RunReportTest, ConfigBlockCarriesNoAnalysisObject) {
  // RunReport does not depend on the analysis library, so the analysis
  // configuration cannot be emitted; the two configuration hashes cover every
  // AnalysisConfig field but `conformance_oracle`, which sits beside them.
  auto parsed = llvm::json::parse(RenderRunReportJson(MakeFixtureReport()));
  ASSERT_TRUE(static_cast<bool>(parsed));
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const llvm::json::Object* config = root->getObject("config");
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(config->getObject("analysis"), nullptr);
  EXPECT_NE(config->getObject("metrics"), nullptr);
  const std::optional<bool> oracle = config->getBoolean("conformance_oracle");
  ASSERT_TRUE(oracle.has_value());
  EXPECT_FALSE(*oracle);
}

TEST(RunReportTest, TextReportIsPristineAndNamesThePhases) {
  const std::string text = RenderRunReportText(MakeFixtureReport());
  EXPECT_NE(text.find("Analysis phase report"), std::string::npos);
  EXPECT_NE(text.find("run"), std::string::npos);
  EXPECT_NE(text.find("m5.svf"), std::string::npos);
  EXPECT_NE(text.find("wpa.component.execute"), std::string::npos);
  EXPECT_NE(text.find("analysis_facts"), std::string::npos);
  EXPECT_EQ(text.find("/tmp/"), std::string::npos);
  EXPECT_EQ(text.find("/Users/"), std::string::npos);

  // A trailing newline, and no line padded with trailing whitespace: the report
  // is pasted into terminals and diffs, where both are visible.
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(text.back(), '\n');
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\n') continue;
    if (i > line_start) {
      EXPECT_NE(text[i - 1], ' ') << "trailing space at offset " << i - 1;
    }
    line_start = i + 1;
  }
}

TEST(RunReportTest, AlignsEveryTreeRowDespiteMultibyteConnectors) {
  // A fixture with a nested row on purpose: "├─ " and "│  " are three bytes per
  // box-drawing character and one column each, so a renderer that padded the
  // label column on std::string::size() would narrow every level and pull its
  // numbers to the left of its siblings'.
  RunReport report;
  report.metrics.root.name = "run";
  report.metrics.root.count = 1;
  core::SpanStats first;
  first.name = "m5.svf";
  first.count = 1;
  core::SpanStats grandchild;
  grandchild.name = "m5.svf.andersen";
  grandchild.count = 1;
  first.children.push_back(grandchild);
  core::SpanStats last;
  last.name = "wpa.orchestrate";
  last.count = 1;
  report.metrics.root.children.push_back(first);
  report.metrics.root.children.push_back(last);

  const std::vector<std::string> lines = SplitLines(RenderRunReportText(report));
  ASSERT_EQ(lines.size(), 5u) << RenderRunReportText(report);
  EXPECT_EQ(lines[0], "Analysis phase report");
  // Depth-first order, connectors nested: the not-last child's subtree hangs
  // off a vertical rule, and the last child's off a blank column.
  EXPECT_EQ(lines[1].find("  run"), 0u) << lines[1];
  EXPECT_EQ(lines[2].find("  ├─ m5.svf"), 0u) << lines[2];
  EXPECT_EQ(lines[3].find("  │  └─ m5.svf.andersen"), 0u) << lines[3];
  EXPECT_EQ(lines[4].find("  └─ wpa.orchestrate"), 0u) << lines[4];

  // Alignment is what the columns' display widths have in common.
  const std::size_t width = DisplayColumns(lines[1]);
  for (std::size_t i = 1; i < lines.size(); ++i) {
    EXPECT_EQ(DisplayColumns(lines[i]), width)
        << "row " << i << " is ragged: " << lines[i];
  }
}

TEST(RunReportTest, RenderersDoNotMutateTheReportTheyAreGiven) {
  RunReport report = MakeFixtureReport();
  const std::string json_first = RenderRunReportJson(report);
  const std::string text_first = RenderRunReportText(report);

  // The renderers sort name-keyed lists on a copy. A renderer that sorted in
  // place would leave the fixture's deliberate insertion order gone — the only
  // observable form of the mutation the design forbids, since a caller renders
  // the same report twice and an in-place sort is idempotent.
  ASSERT_EQ(report.metrics.counters.size(), 2u);
  EXPECT_EQ(report.metrics.counters.front().name, "z.counter");
  ASSERT_EQ(report.inventory.output.components_by_kind.size(), 2u);
  EXPECT_EQ(report.inventory.output.components_by_kind.front().first,
            "reachability");
  EXPECT_EQ(report.metrics.series.size(), 2u);

  EXPECT_EQ(json_first, RenderRunReportJson(report));
  EXPECT_EQ(text_first, RenderRunReportText(report));
}

}  // namespace
}  // namespace veritas::observability
