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

// RunMetricsTest.cpp — unit tests for the span recorder.

#include "veritas/core/RunMetrics.h"

#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "gtest/gtest.h"

namespace veritas::core {
namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

// A recorder whose clocks are driven by two variables the test mutates
// between BeginSpan and EndSpan. This makes every duration an exact integer
// and removes all sleeping from the suite.
//
// RunMetrics is non-movable, so the recorder holds it through a
// std::unique_ptr rather than by value.
struct ScriptedRecorder {
  milliseconds wall_tick{0};
  nanoseconds cpu_tick{0};
  std::unique_ptr<RunMetrics> metrics;

  explicit ScriptedRecorder(RunMetricsOptions opts = {}) {
    metrics = std::make_unique<RunMetrics>(
        opts,
        [this] {
          return std::chrono::steady_clock::time_point{} + wall_tick;
        },
        [this] { return cpu_tick; });
  }
};

// Appends one memory sample at time t, with resident set and physical
// footprint both in bytes. This is the same SeriesBuffer::Append the sampler
// thread calls, driven directly so the join is testable without a thread and
// without a clock.
void AppendSample(RunMetrics& metrics, std::chrono::milliseconds t,
                  std::uint64_t rss, std::uint64_t footprint) {
  metrics.series().Append(MemorySample{t, rss, footprint});
}

TEST(RunMetricsTest, RecordsCountAndWallTimeForOneSpan) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t token = r.metrics->BeginSpan("a");
  r.wall_tick = milliseconds(7);
  r.metrics->EndSpan(token);

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_EQ(stats.root.children.size(), 1u);
  const SpanStats& a = stats.root.children.front();
  EXPECT_EQ(a.name, "a");
  EXPECT_EQ(a.count, 1u);
  EXPECT_EQ(a.wall_inclusive, milliseconds(7));
  EXPECT_EQ(a.wall_self, milliseconds(7));
  EXPECT_EQ(a.min, milliseconds(7));
  EXPECT_EQ(a.max, milliseconds(7));
}

TEST(RunMetricsTest, SubtractsChildrenFromParentSelfTime) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t parent = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(2);
  const std::size_t child = r.metrics->BeginSpan("c");
  r.wall_tick = milliseconds(5);
  r.metrics->EndSpan(child);
  r.wall_tick = milliseconds(10);
  r.metrics->EndSpan(parent);

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& p = stats.root.children.front();
  EXPECT_EQ(p.name, "p");
  EXPECT_EQ(p.wall_inclusive, milliseconds(10));
  // Self time subtracts the children from the parent's inclusive time. The
  // parent ran 0 ms -> 10 ms and its only child ran 2 ms -> 5 ms, so the
  // parent's self time is 10 ms - 3 ms = 7 ms. (The brief this test came from
  // asserted 3 ms here, the child's inclusive time, which no implementation
  // can satisfy alongside the three assertions around it.)
  EXPECT_EQ(p.wall_self, milliseconds(7));
  ASSERT_EQ(p.children.size(), 1u);
  EXPECT_EQ(p.children.front().name, "c");
  EXPECT_EQ(p.children.front().wall_inclusive, milliseconds(3));
}

TEST(RunMetricsTest, MeasuresCpuOnlyOnBearingSpans) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  r.cpu_tick = nanoseconds(1000);
  const std::size_t bearing = r.metrics->BeginSpan("b", SpanMode::kBearing);
  r.wall_tick = milliseconds(4);
  r.cpu_tick = nanoseconds(2500);
  r.metrics->EndSpan(bearing);

  r.wall_tick = milliseconds(100);
  const std::size_t plain = r.metrics->BeginSpan("q");
  r.wall_tick = milliseconds(110);
  r.cpu_tick = nanoseconds(9000);  // must be ignored for a plain span
  r.metrics->EndSpan(plain);

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& b = stats.root.children.at(0);
  EXPECT_TRUE(b.cpu_measured);
  EXPECT_EQ(b.cpu_inclusive, nanoseconds(1500));
  const SpanStats& q = stats.root.children.at(1);
  EXPECT_FALSE(q.cpu_measured);
  EXPECT_EQ(q.cpu_inclusive, nanoseconds(0));
}

TEST(RunMetricsTest, AggregatesRepeatedSpansByName) {
  ScriptedRecorder r;
  const milliseconds durations[] = {milliseconds(2), milliseconds(3)};
  for (const auto d : durations) {
    r.wall_tick = milliseconds(0);
    const std::size_t token = r.metrics->BeginSpan("x");
    r.wall_tick = d;
    r.metrics->EndSpan(token);
  }

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_EQ(stats.root.children.size(), 1u);
  const SpanStats& x = stats.root.children.front();
  EXPECT_EQ(x.count, 2u);
  EXPECT_EQ(x.wall_inclusive, milliseconds(5));
  EXPECT_EQ(x.min, milliseconds(2));
  EXPECT_EQ(x.max, milliseconds(3));
}

TEST(RunMetricsTest, NearestRankPercentilesOverTenSamples) {
  RunMetricsOptions options;
  options.span_sample_cap = 64;
  ScriptedRecorder r(options);
  for (int i = 1; i <= 10; ++i) {
    r.wall_tick = milliseconds(0);
    const std::size_t token =
        r.metrics->BeginSpan("d", SpanMode::kDistributed);
    r.wall_tick = milliseconds(i);
    r.metrics->EndSpan(token);
  }

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& d = stats.root.children.front();
  ASSERT_TRUE(d.distribution.has_value());
  // nearest-rank: p_k = samples[ceil(k/100 * n) - 1], n = 10
  EXPECT_EQ(d.distribution->p50, milliseconds(5));
  EXPECT_EQ(d.distribution->p95, milliseconds(10));
  EXPECT_EQ(d.distribution->p99, milliseconds(10));
  EXPECT_EQ(d.distribution->max, milliseconds(10));
  EXPECT_FALSE(d.distribution->samples_truncated);
}

TEST(RunMetricsTest, TopNOrdersByDurationThenLabel) {
  RunMetricsOptions options;
  options.top_n = 2;
  ScriptedRecorder r(options);
  const std::pair<const char*, int> cases[] = {
      {"b", 5}, {"a", 5}, {"c", 9}};
  for (const auto& [label, ms] : cases) {
    r.wall_tick = milliseconds(0);
    const std::size_t token =
        r.metrics->BeginSpan("t", SpanMode::kDistributed);
    r.metrics->SetLabel(token, label);
    r.wall_tick = milliseconds(ms);
    r.metrics->EndSpan(token);
  }

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& t = stats.root.children.front();
  ASSERT_EQ(t.top_n.size(), 2u);
  EXPECT_EQ(t.top_n.at(0).label, "c");   // 9 ms
  EXPECT_EQ(t.top_n.at(1).label, "a");   // 5 ms, tie broken by label ascending
}

TEST(RunMetricsTest, CapsSamplesAndFlagsTruncation) {
  RunMetricsOptions options;
  options.span_sample_cap = 3;
  ScriptedRecorder r(options);
  for (int i = 0; i < 5; ++i) {
    r.wall_tick = milliseconds(0);
    const std::size_t token =
        r.metrics->BeginSpan("d", SpanMode::kDistributed);
    r.wall_tick = milliseconds(i + 1);
    r.metrics->EndSpan(token);
  }

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& d = stats.root.children.front();
  ASSERT_TRUE(d.distribution.has_value());
  // The first cap samples are kept; the rest are dropped and flagged.
  EXPECT_TRUE(d.distribution->samples_truncated);
  // distribution.max describes the RETAINED samples, so it agrees with the
  // percentiles beside it: max([1ms, 2ms, 3ms]) = 3ms.
  EXPECT_EQ(d.distribution->max, milliseconds(3));
  // SpanStats.max is the true max over every occurrence, which is a different
  // quantity once truncation has occurred: 5ms.
  EXPECT_EQ(d.max, milliseconds(5));

  // Truncation is a degradation, and design section 7.1's preamble makes every
  // degradation loud: a diagnostic and `complete: false`, not just a flag in
  // the artifact. The flag alone would leave the document asserting a
  // completeness it does not have — the partial-summary failure this design
  // exists to prevent — and the writer must be able to see it on stderr.
  EXPECT_FALSE(stats.complete);
  ASSERT_EQ(stats.diagnostics.size(), 1u) << "one diagnostic per span, not one "
                                             "per dropped occurrence";
  EXPECT_NE(stats.diagnostics.front().find("per-span sample cap"),
            std::string::npos)
      << stats.diagnostics.front();
  EXPECT_NE(stats.diagnostics.front().find("'d'"), std::string::npos)
      << "the diagnostic names the span it is about: "
      << stats.diagnostics.front();
}

TEST(RunMetricsTest, DoesNotDiagnoseASpanThatStaysUnderTheSampleCap) {
  // The direction the case above leaves unpinned. A degradation that fires
  // whenever a distributed span is used at all would put `complete: false` and
  // a diagnostic on every healthy run, which devalues both for the case that
  // matters. Five occurrences against a cap of five is the boundary the
  // comparison is written on.
  RunMetricsOptions options;
  options.span_sample_cap = 5;
  ScriptedRecorder r(options);
  for (int i = 0; i < 5; ++i) {
    r.wall_tick = milliseconds(0);
    const std::size_t token =
        r.metrics->BeginSpan("d", SpanMode::kDistributed);
    r.wall_tick = milliseconds(i + 1);
    r.metrics->EndSpan(token);
  }

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& d = stats.root.children.front();
  ASSERT_TRUE(d.distribution.has_value());
  EXPECT_FALSE(d.distribution->samples_truncated);
  EXPECT_TRUE(stats.complete);
  EXPECT_TRUE(stats.diagnostics.empty()) << stats.diagnostics.front();
}

TEST(RunMetricsTest, SortsCountersByNameOnTake) {
  ScriptedRecorder r;
  r.metrics->AddCounter("z.count", 1, "count");
  r.metrics->AddCounter("a.something", 2, "rows");

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_EQ(stats.counters.size(), 2u);
  EXPECT_EQ(stats.counters.at(0).name, "a.something");
  EXPECT_EQ(stats.counters.at(1).name, "z.count");
}

TEST(RunMetricsTest, MismatchedEndSpanRecordsDiagnosticAndDoesNotFold) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t outer = r.metrics->BeginSpan("outer");
  r.wall_tick = milliseconds(1);
  const std::size_t inner = r.metrics->BeginSpan("inner");
  r.wall_tick = milliseconds(2);

  // Closing the outer span first is a caller error. It must not fold a
  // bogus duration into either span.
  r.metrics->EndSpan(outer);
  r.metrics->EndSpan(inner);

  const RunMetricsStats stats = r.metrics->TakeStats();
  EXPECT_FALSE(stats.diagnostics.empty());
  EXPECT_FALSE(stats.complete);
}

TEST(RunMetricsTest, ClampsNegativeDurationsAndRecordsDiagnostic) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(10);
  const std::size_t token = r.metrics->BeginSpan("backwards");
  r.wall_tick = milliseconds(4);  // a misbehaving injected clock
  r.metrics->EndSpan(token);

  const RunMetricsStats stats = r.metrics->TakeStats();
  EXPECT_EQ(stats.root.children.front().wall_inclusive, milliseconds(0));
  EXPECT_FALSE(stats.diagnostics.empty());
  EXPECT_FALSE(stats.complete);
}

TEST(RunMetricsTest, NullRecorderIsInert) {
  // A null recorder must be a harmless no-op. Asserting "zero clock reads"
  // here would assert nothing: with no recorder there is no clock to read.
  // What is observable is that a live recorder nearby is left untouched.
  ScriptedRecorder r;
  {
    RunMetrics* null_metrics = nullptr;
    PhaseSpan span(null_metrics, "never");
    span.SetLabel("x");
    span.AddCounter("c", 1, "count");
  }
  const RunMetricsStats stats = r.metrics->TakeStats();
  EXPECT_TRUE(stats.root.children.empty());
  EXPECT_TRUE(stats.counters.empty());
  EXPECT_TRUE(stats.diagnostics.empty());
}

TEST(RunMetricsTest, JoinsSeriesToBearingSpanInterval) {
  ScriptedRecorder r;
  // Wall ticks are 0 and 10 ms, so the span's window is [0, 10] ms.
  r.wall_tick = milliseconds(0);
  const std::size_t token = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(10);
  r.metrics->EndSpan(token);

  AppendSample(*r.metrics, milliseconds(0), 100, 90);
  AppendSample(*r.metrics, milliseconds(5), 300, 250);
  AppendSample(*r.metrics, milliseconds(10), 200, 180);
  AppendSample(*r.metrics, milliseconds(20), 999, 999);  // outside the window

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& p = stats.root.children.front();
  ASSERT_TRUE(p.memory.has_value());
  EXPECT_EQ(p.memory->rss_start, 100u);
  EXPECT_EQ(p.memory->rss_end, 200u);
  EXPECT_EQ(p.memory->rss_delta, 100);
  EXPECT_EQ(p.memory->peak_within, 300u);  // 999 is outside and must not count
  EXPECT_EQ(p.memory->footprint_peak, 250u);
}

TEST(RunMetricsTest, BothSpanBoundariesAreInclusive) {
  // The boundary rule is start-inclusive and end-inclusive: a sample at
  // exactly t_start belongs to the span, and so does one at exactly t_end.
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t token = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(10);
  r.metrics->EndSpan(token);

  AppendSample(*r.metrics, milliseconds(0), 100, 10);
  AppendSample(*r.metrics, milliseconds(10), 500, 50);
  AppendSample(*r.metrics, milliseconds(11), 999, 999);

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& p = stats.root.children.front();
  ASSERT_TRUE(p.memory.has_value());
  EXPECT_EQ(p.memory->rss_start, 100u);  // t == t_start is inside
  EXPECT_EQ(p.memory->rss_end, 500u);    // t == t_end is inside
  EXPECT_EQ(p.memory->peak_within, 500u);
}

TEST(RunMetricsTest, PlainSpansGetNoMemoryButTheParentWindowStillCoversThem) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t parent = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(5);
  const std::size_t child = r.metrics->BeginSpan("c", SpanMode::kPlain);
  r.wall_tick = milliseconds(15);
  r.metrics->EndSpan(child);
  r.wall_tick = milliseconds(20);
  r.metrics->EndSpan(parent);

  AppendSample(*r.metrics, milliseconds(10), 700, 70);

  const RunMetricsStats stats = r.metrics->TakeStats();
  const SpanStats& p = stats.root.children.front();
  ASSERT_TRUE(p.memory.has_value());
  ASSERT_EQ(p.children.size(), 1u);
  // A kPlain span records no memory block at all...
  EXPECT_FALSE(p.children.front().memory.has_value());
  // ...but the sample taken while it ran is still inside the parent's window.
  EXPECT_EQ(p.memory->peak_within, 700u);
}

TEST(RunMetricsTest, BearingSpanWithNoSeriesHasNoMemoryBlock) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t token = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(10);
  r.metrics->EndSpan(token);

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_TRUE(stats.series.empty());
  EXPECT_FALSE(stats.memory_measured);
  // An absent measurement must read as absent rather than as a measured zero,
  // and this is the library default: interval zero means no sampler, so no
  // series, for every bearing span.
  EXPECT_FALSE(stats.root.children.front().memory.has_value());
}

TEST(RunMetricsTest, BearingSpanWhoseWindowHoldsNoSampleHasNoMemoryBlock) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t early = r.metrics->BeginSpan("early", SpanMode::kBearing);
  r.wall_tick = milliseconds(10);
  r.metrics->EndSpan(early);
  r.wall_tick = milliseconds(20);
  const std::size_t late = r.metrics->BeginSpan("late", SpanMode::kBearing);
  r.wall_tick = milliseconds(30);
  r.metrics->EndSpan(late);

  // A real series, but both samples fall inside `late`'s [20, 30] ms window
  // and outside `early`'s [0, 10] ms one.
  AppendSample(*r.metrics, milliseconds(20), 500, 50);
  AppendSample(*r.metrics, milliseconds(30), 600, 60);

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_EQ(stats.root.children.size(), 2u);
  const SpanStats& early_span = stats.root.children.at(0);
  const SpanStats& late_span = stats.root.children.at(1);
  ASSERT_EQ(early_span.name, "early");
  ASSERT_EQ(late_span.name, "late");
  EXPECT_FALSE(early_span.memory.has_value());
  // The positive control: the same series does fill a window it lands in, so
  // the absence above is a fact about the window, not about the series.
  ASSERT_TRUE(late_span.memory.has_value());
  EXPECT_EQ(late_span.memory->peak_within, 600u);
}

TEST(RunMetricsTest, FailedMemoryProbeAppendsNoSampleAndDiagnosesOnce) {
  RunMetricsOptions options;
  // A reading with no resident set is a failed measurement: a running process
  // never has zero resident bytes, so zero is the failure signal.
  options.memory_probe = [] { return MemoryReading{0, 0}; };
  ScriptedRecorder r(options);
  r.metrics->RecordMemorySample();
  r.metrics->RecordMemorySample();
  r.metrics->RecordMemorySample();

  const RunMetricsStats stats = r.metrics->TakeStats();
  // Nothing was measured, so nothing is claimed to have been.
  EXPECT_TRUE(stats.series.empty());
  // Once, not once per tick: a failing reader would otherwise flood the
  // diagnostics at the sampling rate.
  ASSERT_EQ(stats.diagnostics.size(), 1u);
  EXPECT_FALSE(stats.complete);
}

TEST(RunMetricsTest, WorkingMemoryProbeAppendsItsReading) {
  RunMetricsOptions options;
  options.memory_probe = [] { return MemoryReading{4096u, 2048u}; };
  ScriptedRecorder r(options);
  r.wall_tick = milliseconds(7);
  r.metrics->RecordMemorySample();

  const RunMetricsStats stats = r.metrics->TakeStats();
  ASSERT_EQ(stats.series.size(), 1u);
  EXPECT_EQ(stats.series.front().t, milliseconds(7));
  EXPECT_EQ(stats.series.front().rss_bytes, 4096u);
  EXPECT_EQ(stats.series.front().footprint_bytes, 2048u);
  EXPECT_EQ(stats.peak_rss_bytes, 4096u);
  EXPECT_TRUE(stats.memory_measured);
  EXPECT_TRUE(stats.diagnostics.empty());
}

TEST(RunMetricsTest, MeasuresThePeakEvenWhenTheSeriesIsNotEmitted) {
  RunMetricsOptions options;
  options.emit_series = false;
  options.memory_probe = [] { return MemoryReading{4096u, 2048u}; };
  ScriptedRecorder r(options);
  r.wall_tick = milliseconds(7);
  r.metrics->RecordMemorySample();

  const RunMetricsStats stats = r.metrics->TakeStats();
  // The measurement happened even though the samples are not published.
  // Whether to publish them is a different question from whether they were
  // collected, so the peak stands and `memory_measured` says so — an empty
  // `series` alone would read as "nothing was measured" and lose the peak.
  EXPECT_TRUE(stats.memory_measured);
  EXPECT_TRUE(stats.series.empty());
  EXPECT_EQ(stats.peak_rss_bytes, 4096u);
  EXPECT_EQ(stats.peak_footprint_bytes, 2048u);
  EXPECT_EQ(stats.peak_at, milliseconds(7));
}

TEST(RunMetricsTest, SeriesBufferThinningDoublesDecimationKeepingFirstAndNewest) {
  SeriesBuffer buffer(4);
  for (int i = 0; i < 4; ++i) {
    buffer.Append(MemorySample{milliseconds(i * 10), 100u + i, 10u});
  }
  EXPECT_EQ(buffer.decimation(), 1u);
  EXPECT_EQ(buffer.samples().size(), 4u);

  // The fifth sample overflows the capacity and triggers thinning.
  buffer.Append(MemorySample{milliseconds(40), 104u, 14u});

  EXPECT_EQ(buffer.decimation(), 2u);
  ASSERT_LE(buffer.samples().size(), 4u);
  EXPECT_EQ(buffer.samples().front().t, milliseconds(0));  // the first survives
  EXPECT_EQ(buffer.samples().back().t, milliseconds(40));  // the newest survives
}

TEST(RunMetricsTest, SeriesDecimationSaturatesRatherThanWrapping) {
  // The factor doubles on every overflow, so the 64th doubling reaches 2^64 and
  // wraps to 0. Unclamped, the artifact would carry `series_decimation: 0` —
  // which the reader's guide defines as "the series was never thinned", the
  // exact opposite of the truth, and a plausible value for a different
  // measurement. Reaching 64 overflows by thinning takes about six days of
  // continuous sampling at the CLI's interval; a capacity of one overflows on
  // every append after the first, so 65 appends reach it here. (Measured, not
  // reasoned: 64 appends leave the factor at 2^63, because the first append
  // fills the buffer rather than overflowing it.)
  SeriesBuffer buffer(1);
  for (int i = 0; i < 65; ++i) {
    buffer.Append(MemorySample{milliseconds(i), 100u, 10u});
  }
  EXPECT_EQ(buffer.decimation(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_NE(buffer.decimation(), 0u);

  // And it stays clamped rather than wrapping on a later overflow.
  for (int i = 65; i < 256; ++i) {
    buffer.Append(MemorySample{milliseconds(i), 100u, 10u});
  }
  EXPECT_EQ(buffer.decimation(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_NE(buffer.decimation(), 0u);
}

TEST(RunMetricsTest, PeakIsTheMaximumOfTheWholeSeries) {
  ScriptedRecorder r;
  r.wall_tick = milliseconds(0);
  const std::size_t token = r.metrics->BeginSpan("p", SpanMode::kBearing);
  r.wall_tick = milliseconds(30);
  r.metrics->EndSpan(token);

  AppendSample(*r.metrics, milliseconds(0), 100, 90);
  AppendSample(*r.metrics, milliseconds(10), 900, 850);
  AppendSample(*r.metrics, milliseconds(20), 200, 180);

  const RunMetricsStats stats = r.metrics->TakeStats();
  // The peak is a property of the run, not of any one span: the maximum over
  // the whole series, carrying the time it occurred at.
  EXPECT_EQ(stats.peak_rss_bytes, 900u);
  EXPECT_EQ(stats.peak_footprint_bytes, 850u);
  EXPECT_EQ(stats.peak_at, milliseconds(10));
  EXPECT_EQ(stats.series_decimation, 1u);
  EXPECT_EQ(stats.series.size(), 3u);
}

}  // namespace
}  // namespace veritas::core
