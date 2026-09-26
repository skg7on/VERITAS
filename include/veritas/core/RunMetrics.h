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

// RunMetrics.h — the span recorder for `veritas-build analyze`.
//
// A run opens and closes named spans; the recorder folds them into
// per-name aggregates and hands the caller a tree at the end. A span name may
// contain '.' — `m5.svf.andersen` and `wpa.component.execute` are real ones —
// because the tree is assembled from parent links rather than by splitting
// names, and counters carry names of their own.

#ifndef VERITAS_CORE_RUNMETRICS_H_
#define VERITAS_CORE_RUNMETRICS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veritas::core {

// SpanMode selects how much a span records. Every span records count,
// inclusive and self wall time, and min/max. kBearing additionally records
// process CPU time and its interval, so the report can join the memory
// series to it. kDistributed additionally retains per-occurrence samples for
// percentiles and the N slowest occurrences. cpu_inclusive is measured only
// on kBearing spans: getrusage costs orders of magnitude more than a
// steady_clock read, and the 13,716-iteration component loop must not pay it.
enum class SpanMode : std::uint8_t {
  kPlain,
  kBearing,
  kDistributed,
};

// WallClock returns monotonic wall time. Injected so tests can script it.
using WallClock = std::function<std::chrono::steady_clock::time_point()>;

// CpuClock returns *cumulative* process CPU time (user + sys), monotonic.
// A span's CPU cost is the difference between its end and start readings.
using CpuClock = std::function<std::chrono::nanoseconds()>;

// ProcessCpuNow is the default CpuClock: getrusage(RUSAGE_SELF).
std::chrono::nanoseconds ProcessCpuNow();

struct RunMetricsOptions {
  // The sampler thread runs only when this is positive. Zero means no thread
  // and no series; the recorder still produces per-span wall, self and CPU
  // time. The library default is zero on purpose: a test that constructs a
  // recorder must not silently acquire a thread. The CLI passes 250 ms.
  std::chrono::milliseconds sample_interval{};
  std::size_t top_n = 10;
  bool emit_series = true;
  std::size_t span_sample_cap = 65536;
  std::size_t series_capacity = 65536;
};

struct Counter {
  std::string name;
  std::uint64_t value = 0;
  std::string unit;
};

struct TopEntry {
  std::string label;
  std::chrono::nanoseconds wall{};
};

struct Distribution {
  std::chrono::nanoseconds p50{};
  std::chrono::nanoseconds p95{};
  std::chrono::nanoseconds p99{};
  std::chrono::nanoseconds max{};
  bool samples_truncated = false;
};

struct SpanMemory {
  std::uint64_t rss_start = 0;
  std::uint64_t rss_end = 0;
  std::uint64_t peak_within = 0;
  std::uint64_t footprint_peak = 0;
  // Signed: a phase may release more than it allocates.
  std::int64_t rss_delta = 0;
};

struct SpanStats {
  std::string name;
  std::uint64_t count = 0;
  std::chrono::nanoseconds wall_inclusive{};
  std::chrono::nanoseconds wall_self{};
  std::chrono::nanoseconds cpu_inclusive{};
  bool cpu_measured = false;
  std::chrono::nanoseconds min{};
  std::chrono::nanoseconds max{};
  std::optional<Distribution> distribution;
  std::vector<TopEntry> top_n;
  std::optional<SpanMemory> memory;
  std::vector<SpanStats> children;
};

struct MemorySample {
  std::chrono::milliseconds t{};
  std::uint64_t rss_bytes = 0;
  std::uint64_t footprint_bytes = 0;
};

struct RunMetricsStats {
  SpanStats root;
  std::vector<Counter> counters;
  std::vector<MemorySample> series;
  std::uint64_t series_decimation = 1;
  std::uint64_t peak_rss_bytes = 0;
  std::uint64_t peak_footprint_bytes = 0;
  std::chrono::milliseconds peak_at{};
  std::vector<std::string> diagnostics;
  bool complete = true;
};

// RunMetrics accumulates one run's measurements.
//
// Single-threaded by design: VERITAS takes spans on one thread, so the fold
// path takes no lock. Only the optional sampler thread shares the object, and
// it writes solely into its own pre-allocated buffer, which is read after it
// is joined. See the design spec, sections 4.5 and 7.4.
class RunMetrics {
 public:
  explicit RunMetrics(
      RunMetricsOptions options = {},
      WallClock wall = std::chrono::steady_clock::now,
      CpuClock cpu = &ProcessCpuNow);
  ~RunMetrics();

  RunMetrics(const RunMetrics&) = delete;
  RunMetrics& operator=(const RunMetrics&) = delete;
  RunMetrics(RunMetrics&&) = delete;
  RunMetrics& operator=(RunMetrics&&) = delete;

  // BeginSpan opens a span and returns a token. Spans are strictly nested:
  // EndSpan must receive the innermost open token. A mismatched token records
  // a diagnostic and returns without folding rather than aborting.
  std::size_t BeginSpan(std::string_view name,
                        SpanMode mode = SpanMode::kPlain);

  // EndSpan closes the span identified by token and folds its measurements.
  void EndSpan(std::size_t token);

  // SetLabel names the occurrence, for top-N attribution.
  void SetLabel(std::size_t token, std::string label);

  void AddCounter(std::string_view name, std::uint64_t value,
                  std::string_view unit);
  void AddDiagnostic(std::string message);

  // TakeStats builds the span tree and consumes the recorder: per-occurrence
  // samples are dropped here, so the result carries percentiles only. Not
  // const, and not repeatable with the same fidelity.
  RunMetricsStats TakeStats();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// PhaseSpan is the RAII form of BeginSpan/EndSpan. A null recorder makes it a
// no-op that never reads a clock, so an uninstrumented build pays one pointer
// test per span.
class PhaseSpan {
 public:
  PhaseSpan(RunMetrics* metrics, std::string_view name,
            SpanMode mode = SpanMode::kPlain);
  ~PhaseSpan();

  PhaseSpan(const PhaseSpan&) = delete;
  PhaseSpan& operator=(const PhaseSpan&) = delete;
  PhaseSpan(PhaseSpan&& other) = delete;
  PhaseSpan& operator=(PhaseSpan&& other) = delete;

  void SetLabel(std::string label);
  void AddCounter(std::string_view name, std::uint64_t value,
                  std::string_view unit);

 private:
  RunMetrics* metrics_ = nullptr;
  std::size_t token_ = 0;
};

}  // namespace veritas::core

#endif  // VERITAS_CORE_RUNMETRICS_H_
