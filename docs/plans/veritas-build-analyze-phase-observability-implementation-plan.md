# `veritas-build analyze` Phase Observability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put an in-process span recorder inside `veritas-build analyze` so that per-phase wall time, self time, process CPU time, resident set and physical footprint become fields in a versioned artifact, instead of arguments reconstructed from external sampling.

**Architecture:** A dependency-free `veritas::observability::RunMetrics` recorder lives in `veritas_core` and is threaded into the analyzer, the WPA orchestrator, the SVF session, and the fact bus by explicit pointer. A leaf `observability` library turns a `RunReport` aggregate into a diffable JSON artifact and a stdout report. Recording is off unless a caller passes a recorder, so all existing direct-`AnalyzeProject` tests are untouched.

**Tech Stack:** C++20, CMake 3.23+ with Ninja, LLVM 24 (`llvm::json::OStream` for rendering), GoogleTest, POSIX `pthread`/`getrusage`/`mach`.

**Spec:** [`docs/specs/veritas-build-analyze-phase-observability-design-spec.md`](../specs/veritas-build-analyze-phase-observability-design-spec.md)

**Spec deviations found while planning** (both already corrected in the spec, recorded here so the implementer does not read them as drift):

1. **The recorder lives in `veritas_core`, not in the new `observability` subtree** (spec section 4.1). `veritas_facts` includes `veritas/wpa/WpaOrchestrator.h`, so `facts` depends on `wpa`; a recorder inside a library that also depends on `facts` would close the cycle `wpa → observability → facts → wpa`. The dependency-free recorder goes in `core`; `observability` holds only the renderers and the store read-back.
2. **A negative span duration is clamped unconditionally, with no `assert`** (spec section 7.1). An `assert` would abort the Debug test binary, making the case untestable exactly where it matters, and would be unreachable in Release.

**Mapping to spec section 9.2's five stages:**

| Spec stage | Plan tasks |
| --- | --- |
| 1 — recorder and renderer, standalone | T1, T2 |
| 2 — threading and the CLI | T3, T6 |
| 3 — sub-spans and per-component aggregates | T7 |
| 4 — publication, sampler, inventory, store read-back | T4, T5 |
| 5 — measurement and documentation | T8 |

## Global Constraints

- **Worktree.** All work happens in `.claude/worktrees/analyze-phase-observability-design` on branch `claude/analyze-phase-observability-design`, per `.claude/rules/git-worktree-policy.md`. Never commit to `main`.
- **Configure once, in the worktree.** The worktree has no build tree. Run, from the worktree root:
  ```bash
  cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
  ```
  Build with `cmake --build build --target <target> -j 8` for iteration, and `cmake --build build` for the full tree. The host compiler is auto-detected (`/opt/homebrew/opt/llvm@17/bin/clang++`, clang 17.0.6) against LLVM 24.x libraries. That skew is intentional; do not "fix" it.
- **Run tests** with `ctest --test-dir build -R "^<Suite>\." --output-on-failure`. `gtest_discover_tests` makes every case its own CTest entry named `<Suite>.<Case>`, so filter on the suite. A filter matching zero tests reports success, so confirm the expected case count in the output before believing a pass.
- **License header.** Every new `.h`, `.cpp`, and `CMakeLists.txt` in `include/`, `src/`, and `tests/` begins with exactly this, before any other content:
  ```cpp
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
  ```
  For CMake files use the same text with `#` instead of `//`.
- **No RTTI, no exceptions** (`.claude/rules/cpp-compilation-policy.md`). No `dynamic_cast`, `typeid`, `throw`, `try`, or `catch`. Never use throwing standard facilities: no `std::stoi`, `std::stoull`, `std::stod`, `std::map::at`, `std::vector::at`, or `std::thread`. Parse integers with `std::strtoull`, and use `std::filesystem`'s `std::error_code` overloads. Fallible functions return `veritas::Status` or `veritas::StatusOr<T>`.
- **Span names are dotted paths** — `m5.svf.andersen`, `wpa.component.execute` — emitted as-is, and they are the artifact's node keys. They are **not parsed**: the span tree is assembled by parent index, not by splitting on `.`, so a dot inside a name is ordinary text. An earlier draft of this plan forbade dots in span names because that draft's tree assembly split on them; that assembly was replaced during pre-flight review and the constraint was left behind. The stale wording still sits in `include/veritas/core/RunMetrics.h`'s comment and is corrected in Task 2's fix round (ruling R19).
- **Metrics never touch identity.** Spec section 5.2 is binding: metrics options are not fields of `AnalysisConfig`; the recorder is a parameter, never a field of a hashed struct; no span duration may drive a budget, timeout, or retry.
- **Commit trailer.** Every commit ends with `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
- **Two clock reads per span maximum.** `getrusage` is captured only on `SpanMode::kBearing` spans. Do not add it to `kPlain` or `kDistributed`.

---

### Task 1: The span recorder

The foundation. Everything else consumes this API, so it is specified in full here.

**Files:**
- Create: `include/veritas/core/RunMetrics.h`
- Create: `src/core/RunMetrics.cpp`
- Modify: `src/core/CMakeLists.txt` (add `RunMetrics.cpp` to the library; add `find_package(Threads REQUIRED)` and link `Threads::Threads`)
- Create: `tests/unit/core/RunMetricsTest.cpp`
- Modify: `tests/unit/core/CMakeLists.txt` (append one `add_executable` block)

**Interfaces:**
- Consumes: `veritas::Status`, `veritas::StatusOr` from `include/veritas/core/Status.h`.
- Produces: everything below. Later tasks use these names and signatures verbatim.
  - `enum class SpanMode : std::uint8_t { kPlain, kBearing, kDistributed };`
  - `using WallClock = std::function<std::chrono::steady_clock::time_point()>;`
  - `using CpuClock = std::function<std::chrono::nanoseconds()>;`
  - `std::chrono::nanoseconds ProcessCpuNow();`
  - `struct RunMetricsOptions { std::chrono::milliseconds sample_interval{}; std::size_t top_n; bool emit_series; std::size_t span_sample_cap; std::size_t series_capacity; };`
  - `struct Counter { std::string name; std::uint64_t value; std::string unit; };`
  - `struct TopEntry { std::string label; std::chrono::nanoseconds wall{}; };`
  - `struct Distribution { std::chrono::nanoseconds p50, p95, p99, max; bool samples_truncated; };`
  - `struct SpanMemory { std::uint64_t rss_start, rss_end, peak_within, footprint_peak; std::int64_t rss_delta; };`
  - `struct SpanStats { std::string name; std::uint64_t count; std::chrono::nanoseconds wall_inclusive, wall_self, cpu_inclusive, min, max; bool cpu_measured; std::optional<Distribution> distribution; std::vector<TopEntry> top_n; std::optional<SpanMemory> memory; std::vector<SpanStats> children; };`
  - `struct MemorySample { std::chrono::milliseconds t{}; std::uint64_t rss_bytes, footprint_bytes; };`
  - `struct RunMetricsStats { SpanStats root; std::vector<Counter> counters; std::vector<MemorySample> series; std::uint64_t series_decimation; std::uint64_t peak_rss_bytes, peak_footprint_bytes; std::chrono::milliseconds peak_at{}; std::vector<std::string> diagnostics; bool complete; };`
  - `class RunMetrics` with `RunMetrics(RunMetricsOptions = {}, WallClock = std::chrono::steady_clock::now, CpuClock = &ProcessCpuNow)`, `std::size_t BeginSpan(std::string_view, SpanMode = SpanMode::kPlain)`, `void EndSpan(std::size_t)`, `void SetLabel(std::size_t, std::string)`, `void AddCounter(std::string_view, std::uint64_t, std::string_view)`, `void AddDiagnostic(std::string)`, `RunMetricsStats TakeStats()`. Non-copyable.
  - `class PhaseSpan` with `PhaseSpan(RunMetrics*, std::string_view, SpanMode = SpanMode::kPlain)`, `~PhaseSpan()`, `void SetLabel(std::string)`, `void AddCounter(std::string_view, std::uint64_t, std::string_view)`. A `nullptr` recorder makes every method a no-op that reads no clock.

- [ ] **Step 1: Write the header**

Create `include/veritas/core/RunMetrics.h` with the license header, then:

```cpp
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
```

- [ ] **Step 2: Write a stub implementation and register both targets**

Create `src/core/RunMetrics.cpp` with the license header and **real signatures returning wrong values**, so the tests compile and fail rather than failing to build:

```cpp
#include "veritas/core/RunMetrics.h"

#include <sys/resource.h>

namespace veritas::core {

std::chrono::nanoseconds ProcessCpuNow() { return std::chrono::nanoseconds{0}; }

struct RunMetrics::Impl {};

RunMetrics::RunMetrics(RunMetricsOptions, WallClock, CpuClock)
    : impl_(std::make_unique<Impl>()) {}
RunMetrics::~RunMetrics() = default;

std::size_t RunMetrics::BeginSpan(std::string_view, SpanMode) { return 0; }
void RunMetrics::EndSpan(std::size_t) {}
void RunMetrics::SetLabel(std::size_t, std::string) {}
void RunMetrics::AddCounter(std::string_view, std::uint64_t, std::string_view) {}
void RunMetrics::AddDiagnostic(std::string) {}
RunMetricsStats RunMetrics::TakeStats() { return RunMetricsStats{}; }

PhaseSpan::PhaseSpan(RunMetrics* metrics, std::string_view name, SpanMode mode)
    : metrics_(metrics) {
  if (metrics_ != nullptr) token_ = metrics_->BeginSpan(name, mode);
}
PhaseSpan::~PhaseSpan() {
  if (metrics_ != nullptr) metrics_->EndSpan(token_);
}
void PhaseSpan::SetLabel(std::string label) {
  if (metrics_ != nullptr) metrics_->SetLabel(token_, std::move(label));
}
void PhaseSpan::AddCounter(std::string_view name, std::uint64_t value,
                           std::string_view unit) {
  if (metrics_ != nullptr) metrics_->AddCounter(name, value, unit);
}

}  // namespace veritas::core
```

In `src/core/CMakeLists.txt`, add `RunMetrics.cpp` to the `add_library(veritas_core ...)` source list, and after the existing target setup add:

```cmake
find_package(Threads REQUIRED)
target_link_libraries(veritas_core PRIVATE Threads::Threads)
```

- [ ] **Step 3: Write the failing tests**

Create `tests/unit/core/RunMetricsTest.cpp` with the license header, then:

```cpp
#include "veritas/core/RunMetrics.h"

#include <chrono>
#include <string>

#include "gtest/gtest.h"

namespace veritas::core {
namespace {

using std::chrono::milliseconds;
using std::chrono::nanoseconds;

// A recorder whose clocks are driven by two variables the test mutates
// between BeginSpan and EndSpan. This makes every duration an exact integer
// and removes all sleeping from the suite.
struct ScriptedRecorder {
  milliseconds wall_tick{0};
  nanoseconds cpu_tick{0};
  RunMetricsOptions options;
  RunMetrics metrics;

  explicit ScriptedRecorder(RunMetricsOptions opts = {}) : options(opts) {
    metrics = RunMetrics(
        options,
        [this] {
          return std::chrono::steady_clock::time_point{} + wall_tick;
        },
        [this] { return cpu_tick; });
  }
};
```

`RunMetrics` is non-movable, so `metrics = RunMetrics(...)` cannot compile. Use a `std::unique_ptr` member instead:

```cpp
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
```

Then the cases:

```cpp
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
  // Self time is inclusive minus children: 10 ms - 3 ms = 7 ms. (An earlier
  // draft of this plan said 3 ms here, which is the CHILD's self time.)
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
```

- [ ] **Step 4: Register the test target and run it to confirm failure**

Append to `tests/unit/core/CMakeLists.txt`, matching the surrounding blocks:

```cmake
add_executable(RunMetricsTest RunMetricsTest.cpp)
target_link_libraries(RunMetricsTest PRIVATE
  veritas_core
  GTest::gtest_main
)
veritas_add_warnings(RunMetricsTest)
gtest_discover_tests(RunMetricsTest DISCOVERY_TIMEOUT 60)
```

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build build --target RunMetricsTest -j 8
ctest --test-dir build -R "^RunMetricsTest\." --output-on-failure
```

Expected: the build succeeds and **every case fails** — `TakeStats()` returns an empty `RunMetricsStats`, so `root.children.size()` is 0. Confirm the case count in the output matches the 11 cases above before proceeding; a zero-test match reports success.

- [ ] **Step 5: Implement the recorder**

Replace the body of `src/core/RunMetrics.cpp`:

```cpp
#include "veritas/core/RunMetrics.h"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace veritas::core {

namespace {

constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);
constexpr std::size_t kNoToken = static_cast<std::size_t>(-1);

nanoseconds ClampNonNegative(nanoseconds value) {
  return value < nanoseconds::zero() ? nanoseconds::zero() : value;
}

// Nearest-rank percentile: samples[ceil(k/100 * n) - 1] over a sorted vector.
nanoseconds NearestRank(std::vector<nanoseconds> sorted, int percentile) {
  if (sorted.empty()) return nanoseconds::zero();
  std::sort(sorted.begin(), sorted.end());
  const std::size_t n = sorted.size();
  std::size_t rank =
      static_cast<std::size_t>(std::ceil(percentile / 100.0 * n));
  if (rank < 1) rank = 1;
  if (rank > n) rank = n;
  return sorted[rank - 1];
}

}  // namespace

nanoseconds ProcessCpuNow() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return nanoseconds::zero();
  const auto secs = std::chrono::seconds(usage.ru_utime.tv_sec +
                                         usage.ru_stime.tv_sec);
  const auto usecs = std::chrono::microseconds(usage.ru_utime.tv_usec +
                                               usage.ru_stime.tv_usec);
  return std::chrono::duration_cast<nanoseconds>(secs + usecs);
}

struct RunMetrics::Impl {
  struct Open {
    std::string name;
    SpanMode mode = SpanMode::kPlain;
    std::chrono::steady_clock::time_point start_wall{};
    nanoseconds start_cpu{};
    std::size_t accum_index = kNoParent;
    std::string label;
  };

  struct Accum {
    std::string name;
    std::size_t parent_index = kNoParent;
    SpanMode mode = SpanMode::kPlain;
    std::uint64_t count = 0;
    nanoseconds inclusive{};
    nanoseconds children_inclusive{};
    nanoseconds cpu{};
    nanoseconds min{};
    nanoseconds max{};
    bool min_max_set = false;
    std::vector<nanoseconds> samples;
    std::vector<TopEntry> top;
    bool samples_truncated = false;
    bool has_interval = false;
    std::chrono::steady_clock::time_point first_start{};
    std::chrono::steady_clock::time_point last_end{};
  };

  RunMetricsOptions options;
  WallClock wall;
  CpuClock cpu;
  std::vector<Open> open;
  std::vector<Accum> accums;
  // Keyed by (parent accum index, span name) so one node exists per name per
  // parent, and no name has to be parsed out of a dotted path.
  std::map<std::pair<std::size_t, std::string>, std::size_t> by_parent_name;
  std::vector<Counter> counters;
  std::vector<std::string> diagnostics;
  bool complete = true;

  Impl(RunMetricsOptions opts, WallClock w, CpuClock c)
      : options(opts), wall(std::move(w)), cpu(std::move(c)) {}

  void Diagnose(std::string message) {
    diagnostics.push_back(std::move(message));
    complete = false;
  }

  std::size_t FindOrCreateAccum(std::size_t parent, std::string_view name,
                                SpanMode mode) {
    const auto key = std::make_pair(parent, std::string(name));
    const auto it = by_parent_name.find(key);
    if (it != by_parent_name.end()) return it->second;
    Accum accum;
    accum.name = std::string(name);
    accum.parent_index = parent;
    accum.mode = mode;
    const std::size_t index = accums.size();
    accums.push_back(std::move(accum));
    by_parent_name.emplace(key, index);
    return index;
  }

  static void SortTop(std::vector<TopEntry>* top, std::size_t limit) {
    std::sort(top->begin(), top->end(),
              [](const TopEntry& left, const TopEntry& right) {
                if (left.wall != right.wall) return left.wall > right.wall;
                return left.label < right.label;
              });
    if (top->size() > limit) top->resize(limit);
  }

  void AttachChildren(const std::vector<std::size_t>& accum_to_stats) {
    (void)accum_to_stats;
  }
};

RunMetrics::RunMetrics(RunMetricsOptions options, WallClock wall, CpuClock cpu)
    : impl_(std::make_unique<Impl>(options, std::move(wall), std::move(cpu))) {}

RunMetrics::~RunMetrics() = default;

std::size_t RunMetrics::BeginSpan(std::string_view name, SpanMode mode) {
  Impl::Open span;
  span.name = std::string(name);
  span.mode = mode;
  span.start_wall = impl_->wall();
  span.start_cpu =
      mode == SpanMode::kBearing ? impl_->cpu() : nanoseconds::zero();
  const std::size_t parent =
      impl_->open.empty() ? kNoParent : impl_->open.back().accum_index;
  span.accum_index = impl_->FindOrCreateAccum(parent, name, mode);
  impl_->open.push_back(std::move(span));
  return impl_->open.size() - 1;
}

void RunMetrics::EndSpan(std::size_t token) {
  if (impl_->open.empty() || token != impl_->open.size() - 1) {
    impl_->Diagnose("RunMetrics: EndSpan token is not the innermost open span");
    return;
  }
  Impl::Open span = std::move(impl_->open.back());
  impl_->open.pop_back();

  const auto end_wall = impl_->wall();
  const nanoseconds raw_wall = end_wall - span.start_wall;
  if (raw_wall < nanoseconds::zero()) {
    impl_->Diagnose("RunMetrics: negative span duration clamped to zero");
  }
  const nanoseconds wall = ClampNonNegative(raw_wall);

  Impl::Accum& accum = impl_->accums.at(span.accum_index);
  accum.count += 1;
  accum.inclusive += wall;
  if (span.mode == SpanMode::kBearing) {
    const nanoseconds cpu = ClampNonNegative(impl_->cpu() - span.start_cpu);
    accum.cpu += cpu;
    accum.has_interval = true;
    if (accum.count == 1 || span.start_wall < accum.first_start) {
      accum.first_start = span.start_wall;
    }
    if (accum.count == 1 || end_wall > accum.last_end) {
      accum.last_end = end_wall;
    }
  }
  if (!accum.min_max_set || wall < accum.min) accum.min = wall;
  if (!accum.min_max_set || wall > accum.max) accum.max = wall;
  accum.min_max_set = true;

  if (span.mode == SpanMode::kDistributed) {
    if (accum.samples.size() < impl_->options.span_sample_cap) {
      accum.samples.push_back(wall);
    } else {
      accum.samples_truncated = true;
    }
    accum.top.push_back(TopEntry{span.label, wall});
    Impl::SortTop(&accum.top, impl_->options.top_n);
  }

  if (accum.parent_index != kNoParent) {
    impl_->accums.at(accum.parent_index).children_inclusive += wall;
  }
}

void RunMetrics::SetLabel(std::size_t token, std::string label) {
  if (impl_->open.empty() || token != impl_->open.size() - 1) {
    impl_->Diagnose("RunMetrics: SetLabel on a span that is not open");
    return;
  }
  impl_->open.back().label = std::move(label);
}

void RunMetrics::AddCounter(std::string_view name, std::uint64_t value,
                            std::string_view unit) {
  impl_->counters.push_back(
      Counter{std::string(name), value, std::string(unit)});
}

void RunMetrics::AddDiagnostic(std::string message) {
  impl_->Diagnose(std::move(message));
}

RunMetricsStats RunMetrics::TakeStats() {
  RunMetricsStats stats;
  stats.diagnostics = impl_->diagnostics;
  stats.complete = impl_->complete;

  std::sort(impl_->counters.begin(), impl_->counters.end(),
            [](const Counter& left, const Counter& right) {
              return left.name < right.name;
            });
  stats.counters = std::move(impl_->counters);

  // Classify each accumulator by its parent BEFORE building anything, then
  // build top-down by index.
  //
  // Do NOT fold children into parents in a single forward pass: a parent's
  // accumulator is always created before its children's, so by the time the
  // loop reaches a child, its parent has already been moved into the
  // grandparent. Pushing into a moved-from SpanStats silently corrupts the
  // tree. Recursive construction by index has no such ordering hazard.
  std::vector<std::vector<std::size_t>> child_indices(impl_->accums.size());
  std::vector<std::size_t> roots;
  for (std::size_t i = 0; i < impl_->accums.size(); ++i) {
    const std::size_t parent = impl_->accums.at(i).parent_index;
    if (parent == kNoParent) {
      roots.push_back(i);
    } else {
      child_indices.at(parent).push_back(i);
    }
  }
  const auto by_name = [this](std::size_t left, std::size_t right) {
    return impl_->accums.at(left).name < impl_->accums.at(right).name;
  };

  std::function<SpanStats(std::size_t)> build = [&](std::size_t index) {
    const Impl::Accum& accum = impl_->accums.at(index);
    SpanStats out;
    out.name = accum.name;
    out.count = accum.count;
    out.wall_inclusive = accum.inclusive;
    out.wall_self =
        ClampNonNegative(accum.inclusive - accum.children_inclusive);
    out.cpu_inclusive = accum.cpu;
    out.cpu_measured = accum.mode == SpanMode::kBearing;
    out.min = accum.min_max_set ? accum.min : nanoseconds::zero();
    out.max = accum.max;
    if (accum.mode == SpanMode::kDistributed) {
      Distribution distribution;
      distribution.samples_truncated = accum.samples_truncated;
      distribution.p50 = NearestRank(accum.samples, 50);
      distribution.p95 = NearestRank(accum.samples, 95);
      distribution.p99 = NearestRank(accum.samples, 99);
      // The max of the RETAINED samples, so it is consistent with the
      // percentiles beside it. SpanStats::max is the true max over every
      // occurrence and may be larger once samples_truncated is set.
      distribution.max = NearestRank(accum.samples, 100);
      out.distribution = distribution;
      out.top_n = accum.top;
    }
    std::vector<std::size_t> children = child_indices.at(index);
    std::sort(children.begin(), children.end(), by_name);
    for (const std::size_t child : children) {
      out.children.push_back(build(child));
    }
    return out;
  };

  if (roots.size() == 1 && impl_->accums.at(roots.front()).name == "run") {
    // The CLI opens a span named "run", so that accumulator is the root.
    stats.root = build(roots.front());
  } else {
    // Zero roots (nothing recorded), or spans taken outside any root. Wrap
    // them in a synthetic "run" so the tree always has exactly one root, in
    // name order. Its wall and CPU are the sum of its disjoint children and
    // its self time is therefore zero by construction.
    std::sort(roots.begin(), roots.end(), by_name);
    stats.root.name = "run";
    for (const std::size_t index : roots) {
      stats.root.children.push_back(build(index));
    }
    for (const SpanStats& child : stats.root.children) {
      stats.root.wall_inclusive += child.wall_inclusive;
      if (child.cpu_measured) {
        stats.root.cpu_measured = true;
        stats.root.cpu_inclusive += child.cpu_inclusive;
      }
      stats.root.min = stats.root.min == nanoseconds::zero()
                           ? child.min
                           : std::min(stats.root.min, child.min);
      stats.root.max = std::max(stats.root.max, child.max);
    }
    stats.root.wall_self = nanoseconds::zero();
  }
  return stats;
}

PhaseSpan::PhaseSpan(RunMetrics* metrics, std::string_view name, SpanMode mode)
    : metrics_(metrics) {
  if (metrics_ != nullptr) token_ = metrics_->BeginSpan(name, mode);
}

PhaseSpan::~PhaseSpan() {
  if (metrics_ != nullptr) metrics_->EndSpan(token_);
}

void PhaseSpan::SetLabel(std::string label) {
  if (metrics_ != nullptr) metrics_->SetLabel(token_, std::move(label));
}

void PhaseSpan::AddCounter(std::string_view name, std::uint64_t value,
                           std::string_view unit) {
  if (metrics_ != nullptr) metrics_->AddCounter(name, value, unit);
}

}  // namespace veritas::core
```

Two mechanical fixes this code needs before it compiles:

- `src/core/RunMetrics.cpp` uses `std::map` and `nanoseconds` unqualified. Add `#include <map>` and, inside `namespace veritas::core`, `using std::chrono::nanoseconds;` alongside `using std::chrono::seconds;`/`microseconds` only where used — or fully qualify every use as `std::chrono::nanoseconds`. Prefer fully qualifying in the header-facing code and adding `using std::chrono::nanoseconds;` at the top of the anonymous namespace in the `.cpp`.
- The `Impl::AttachChildren` declaration is dead. Delete it; the tree is built in `TakeStats`.

- [ ] **Step 6: Run the tests and confirm they pass**

```bash
cmake --build build --target RunMetricsTest -j 8
ctest --test-dir build -R "^RunMetricsTest\." --output-on-failure
```

Expected: 11/11 pass. If `CapsSamplesAndFlagsTruncation` fails on `max`, check that `max` is computed from the folded durations (3 ms) rather than from the retained samples.

- [ ] **Step 7: Commit**

```bash
git add include/veritas/core/RunMetrics.h src/core/RunMetrics.cpp \
        src/core/CMakeLists.txt tests/unit/core/RunMetricsTest.cpp \
        tests/unit/core/CMakeLists.txt
git commit -m "feat(core): add the span recorder for analyze phase metrics" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: The report model and its two renderers

**Files:**
- Create: `include/veritas/observability/RunReport.h`
- Create: `src/observability/RunReport.cpp`
- Create: `src/observability/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add `add_subdirectory(src/observability)` after line 254)
- Create: `tests/unit/observability/RunReportTest.cpp`
- Create: `tests/unit/observability/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt` (add `add_subdirectory(observability)`)

**Interfaces:**
- Consumes: `veritas::core::RunMetricsStats`, `RunMetricsOptions`, `SpanStats`, `Counter`, `MemorySample` from Task 1; `llvm::json::OStream` from LLVM.
- Produces: `RunIdentity`, `RunEnvironment`, `RunInputInventory`, `RunOutputInventory`, `RunIncrementality`, `RunInventory`, `TableRowCount`, `NamedBytes`, `CrossCheck`, `StoreSummary`, `RunReport` (all with the fields listed below), plus `std::string RenderRunReportJson(const RunReport&)`, `std::string RenderRunReportText(const RunReport&)`, `std::string FormatDuration(std::chrono::nanoseconds)`, `std::string FormatBytes(std::uint64_t)`.

- [ ] **Step 1: Write the header**

Create `include/veritas/observability/RunReport.h` with the license header and the aggregate types. Key decisions to preserve: every count is `std::uint64_t`; durations stay `std::chrono::nanoseconds` until rendering; ordered maps are `std::vector<std::pair<std::string, std::uint64_t>>` so ordering is explicit rather than incidental.

```cpp
#ifndef VERITAS_OBSERVABILITY_RUNREPORT_H_
#define VERITAS_OBSERVABILITY_RUNREPORT_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "veritas/core/RunMetrics.h"

namespace veritas::observability {

// The identity block is separated because it is the part that MUST move
// between runs. A comparison script can exclude it wholesale without parsing
// the rest of the artifact.
struct RunIdentity {
  std::string run_id;
  std::string batch_id;
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::string projection_id;
  std::string svf_config_hash;
  std::string wpa_config_hash;
  std::string engine_toolchain_identity;
};

struct RunEnvironment {
  std::string os;
  std::string arch;
  std::string cpu_model;
  std::uint64_t cores = 0;
  std::uint64_t ram_bytes = 0;
  std::string build_type;
  std::string host_compiler;
  std::string veritas_version;
  std::string git_revision;
};

struct RunInputInventory {
  std::uint64_t translation_units = 0;
  std::string compiler_id;
  std::string compiler_version;
  std::string target_triple;
  std::string source_tree_hash;
  std::string include_closure_hash;
};

struct RunOutputInventory {
  std::uint64_t summaries_published = 0;
  std::vector<std::pair<std::string, std::uint64_t>> unknowns_by_reason;
  std::uint64_t cpg_nodes = 0;
  std::uint64_t cpg_edges = 0;
  std::uint64_t svfg_nodes = 0;
  std::uint64_t svfg_edges = 0;
  std::vector<std::pair<std::string, std::uint64_t>> components_by_kind;
  std::uint64_t rooted_input_facts = 0;
  std::uint64_t canonical_facts = 0;
};

struct RunIncrementality {
  std::uint64_t components_reused = 0;
  std::uint64_t components_executed = 0;
  std::uint64_t summaries_recomputed = 0;
  std::uint64_t summaries_reused = 0;
};

struct RunInventory {
  RunInputInventory input;
  RunOutputInventory output;
  RunIncrementality incrementality;
};

struct TableRowCount {
  std::string table;
  std::uint64_t rows = 0;
};

struct NamedBytes {
  std::string name;
  std::uint64_t bytes = 0;
};

// A count the pipeline holds in memory paired with the same count read back
// from the store. An independent check is worth more than a self-report.
struct CrossCheck {
  std::string name;
  std::uint64_t from_store = 0;
  std::uint64_t from_memory = 0;
  bool agrees = false;
};

struct StoreSummary {
  std::vector<TableRowCount> tables;
  std::vector<NamedBytes> bytes;
  std::vector<CrossCheck> cross_checks;
};

struct RunReport {
  RunIdentity identity;
  RunEnvironment environment;
  RunInventory inventory;
  StoreSummary store;
  core::RunMetricsOptions metrics_options;
  core::RunMetricsStats metrics;
  // The one analysis-config knob no configuration hash covers. It changes what
  // the run does — a second full WPA whose canonical results must agree — so a
  // reader comparing two artifacts needs it, and cannot recover it from
  // svf_config_hash or wpa_config_hash. Every other AnalysisConfig field IS
  // covered by one of those two hashes; see design section 6.3.
  bool conformance_oracle = false;
};

// RenderRunReportJson emits the versioned artifact. Object keys are written in
// sorted order, durations are integer nanoseconds, and no absolute path
// appears anywhere, so two artifacts can be diffed byte for byte.
std::string RenderRunReportJson(const RunReport& report);

// RenderRunReportText emits the human report with a wall/self/CPU table and a
// memory column, then the component, store and cross-check summaries.
std::string RenderRunReportText(const RunReport& report);

// FormatDuration renders 1234567890ns as "1.235s"; FormatBytes renders
// 9223372036 as "8.59 GiB".
std::string FormatDuration(std::chrono::nanoseconds value);
std::string FormatBytes(std::uint64_t bytes);

}  // namespace veritas::observability

#endif  // VERITAS_OBSERVABILITY_RUNREPORT_H_
```

- [ ] **Step 2: Write the failing tests**

Create `tests/unit/observability/RunReportTest.cpp` with the license header and cases that pin the format rules, not the numbers:

```cpp
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
```

`MakeFixtureReport()` is a helper in the same file. Write it exactly as follows: its counters and component kinds are **deliberately out of order**, so the ordering assertions below prove the renderer sorts rather than restating the fixture's insertion order.

```cpp
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
  return report;
}
```


Register the test target in `tests/unit/observability/CMakeLists.txt` (and add `add_subdirectory(observability)` to `tests/unit/CMakeLists.txt` if Task 2 is the first to touch that directory):

```cmake
add_executable(RunReportTest RunReportTest.cpp)
target_include_directories(RunReportTest SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(RunReportTest PRIVATE
  veritas_observability
  LLVM
  GTest::gtest_main
)
veritas_add_warnings(RunReportTest)
gtest_discover_tests(RunReportTest DISCOVERY_TIMEOUT 60)
```

**`${LLVM_INCLUDE_DIRS}` is not optional, and linking `LLVM` does not imply it.** The imported `LLVM` target brings no include directories, so a test that includes `<llvm/Support/JSON.h>` without this line silently compiles against whatever LLVM headers are on the default search path — on this machine, Homebrew's llvm@17 — while linking LLVM 24. That is a header/library version mismatch that compiles cleanly and is exactly the class of bug that hides until it does not. Every other target in this repository that uses LLVM headers adds this line explicitly (`src/build/CMakeLists.txt:40`, `src/evidence/CMakeLists.txt:87`, `src/facts/CMakeLists.txt:68`); follow them.

- [ ] **Step 3: Run the tests to confirm they fail**

```bash
cmake --build build --target RunReportTest -j 8
ctest --test-dir build -R "^RunReportTest\." --output-on-failure
```

Expected: build failure first (no `veritas_observability` target), then after creating the library skeleton, assertion failures.

- [ ] **Step 4: Create the library and implement the renderers**

Create `src/observability/CMakeLists.txt`:

```cmake
add_library(veritas_observability
  RunReport.cpp
)
target_include_directories(veritas_observability PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(veritas_observability SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(veritas_observability
  PUBLIC veritas_core
  PRIVATE LLVM
)
target_compile_features(veritas_observability PUBLIC cxx_std_20)
veritas_add_warnings(veritas_observability)
add_library(veritas::observability ALIAS veritas_observability)
```

Add `add_subdirectory(src/observability)` to `CMakeLists.txt` immediately after `add_subdirectory(src/evidence)`.

Implement `RenderRunReportJson` with `llvm::json::OStream` writing **attributes in sorted key order** and durations via `static_cast<std::int64_t>(value.count())`.

**The renderer sorts every name-keyed list itself, on a copy — counters, `unknowns_by_reason`, and `components_by_kind`.** Do not trust the producer to have sorted them: `TakeStats` sorts counters today, but a defensive sort here makes the artifact's stability a property of the renderer, which is where the diffability contract lives. Sort a local copy; never mutate the `RunReport` passed in, since callers render the same report twice (JSON and text).

The recursive span emitter:

```cpp
void EmitSpan(llvm::json::OStream& j, const core::SpanStats& span) {
  j.object([&] {
    j.attribute("name", span.name);
    j.attribute("count", static_cast<std::int64_t>(span.count));
    j.attribute("wall_inclusive_ns",
                static_cast<std::int64_t>(span.wall_inclusive.count()));
    j.attribute("wall_self_ns",
                static_cast<std::int64_t>(span.wall_self.count()));
    if (span.cpu_measured) {
      j.attribute("cpu_inclusive_ns",
                  static_cast<std::int64_t>(span.cpu_inclusive.count()));
    }
    j.attribute("min_ns", static_cast<std::int64_t>(span.min.count()));
    j.attribute("max_ns", static_cast<std::int64_t>(span.max.count()));
    if (span.distribution.has_value()) {
      j.attributeObject("distribution", [&] {
        j.attribute("p50_ns", static_cast<std::int64_t>(
                                  span.distribution->p50.count()));
        j.attribute("p95_ns", static_cast<std::int64_t>(
                                  span.distribution->p95.count()));
        j.attribute("p99_ns", static_cast<std::int64_t>(
                                  span.distribution->p99.count()));
        j.attribute("max_ns", static_cast<std::int64_t>(
                                  span.distribution->max.count()));
        j.attribute("samples_truncated",
                    span.distribution->samples_truncated);
      });
    }
    if (!span.top_n.empty()) {
      j.attributeArray("top_n", [&] {
        for (std::size_t i = 0; i < span.top_n.size(); ++i) {
          j.object([&] {
            j.attribute("rank", static_cast<std::int64_t>(i + 1));
            j.attribute("label", span.top_n.at(i).label);
            j.attribute("wall_ns", static_cast<std::int64_t>(
                                       span.top_n.at(i).wall.count()));
          });
        }
      });
    }
    if (span.memory.has_value()) {
      j.attributeObject("memory", [&] {
        j.attribute("rss_start",
                    static_cast<std::int64_t>(span.memory->rss_start));
        j.attribute("rss_end",
                    static_cast<std::int64_t>(span.memory->rss_end));
        j.attribute("delta", span.memory->rss_delta);
        j.attribute("peak_within",
                    static_cast<std::int64_t>(span.memory->peak_within));
        j.attribute("footprint_peak", static_cast<std::int64_t>(
                                          span.memory->footprint_peak));
      });
    }
    j.attributeArray("children", [&] {
      for (const core::SpanStats& child : span.children) EmitSpan(j, child);
    });
  });
}
```

`FormatDuration` renders seconds with three decimals for values at or above one second, milliseconds with three decimals below that, and `"0ns"` for zero.

`FormatBytes` uses **three** tiers, not two: GiB with two decimals at or above 1 GiB, MiB with two decimals at or above 1 MiB, and KiB with one decimal below that, with plain bytes below 1 KiB. A two-tier rule that bottomed out at MiB would print a genuinely small peak — a phase that allocates a few hundred kilobytes — as `"0.00 MiB"`, which reads as *nothing was measured* rather than *this phase is small*. The whole point of the memory column is to distinguish those two cases.

- [ ] **Step 5: Run the tests and confirm they pass**

```bash
cmake --build build --target RunReportTest -j 8
ctest --test-dir build -R "^RunReportTest\." --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add include/veritas/observability/RunReport.h src/observability/CMakeLists.txt \
        src/observability/RunReport.cpp CMakeLists.txt \
        tests/unit/observability tests/unit/CMakeLists.txt
git commit -m "feat(observability): add the run report model and renderers" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: Thread the recorder through the analyzer, and guard identity

**Files:**
- Modify: `include/veritas/analysis/ProjectAnalyzer.h` (the `AnalyzeProject` signature)
- Modify: `src/analysis/ProjectAnalyzer.cpp` (top-level spans; `RunWpa` parameter; batch and publication spans)
- Modify: `include/veritas/wpa/WpaOrchestrator.h` (add `WpaRunRequest::metrics`)
- Create: `tests/integration/analysis/PhaseObservabilityIdentityTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `core::RunMetrics`, `core::PhaseSpan`, `SpanMode`.
- Produces: `StatusOr<ProjectAnalysisResult> ProjectAnalyzer::AnalyzeProject(const ProjectAnalysisRequest&, const AnalysisConfig&, core::RunMetrics* metrics = nullptr)`; `core::RunMetrics* WpaRunRequest::metrics = nullptr`.

- [ ] **Step 1: Write the failing identity test**

The single most important test in this change: it is the executable form of spec section 5.2 Rule 2. It runs the analyzer twice on one fixture — once recording, once not — and asserts that every identity is byte-equal.

Create `tests/integration/analysis/PhaseObservabilityIdentityTest.cpp` with the license header, then the includes and namespace that `ProjectAnalyzerTest.cpp` in the same directory already uses. Inside `veritas::analysis`, the names `core::` and `testing::` resolve to `veritas::core` and `veritas::testing` through enclosing-namespace lookup, so do not qualify them further:

```cpp
#include "veritas/analysis/ProjectAnalyzer.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace veritas::analysis
```

Register the target at the end of `tests/integration/analysis/CMakeLists.txt`, mirroring the `ProjectAnalyzerTest` block in that file exactly. The two support libraries are both required — `veritas_test_support` carries `FixtureProject`, and `veritas_unit_test_support` is the INTERFACE library that brings in `GTest::gtest_main`, which is why that block does not name GTest directly. The timeout is 60 s rather than `ProjectAnalyzerTest`'s 30 s because this test analyzes the fixture twice:

```cmake
add_executable(phase_observability_identity_integration_test
  PhaseObservabilityIdentityTest.cpp
)
target_link_libraries(phase_observability_identity_integration_test
  PRIVATE
    veritas_analysis
    veritas_test_support
    veritas_unit_test_support
)
add_test(NAME PhaseObservabilityIdentityTest
  COMMAND phase_observability_identity_integration_test)
set_tests_properties(PhaseObservabilityIdentityTest PROPERTIES
  TIMEOUT 60
  LABELS "integration;analysis"
)
```

The CTest name and the gtest suite name are deliberately the same string; `add_test` names it explicitly rather than via `gtest_discover_tests`, which is the convention in this directory.

- [ ] **Step 2: Run it to confirm it fails**

```bash
cmake --build build --target phase_observability_identity_integration_test -j 8
ctest --test-dir build -R "PhaseObservabilityIdentityTest" --output-on-failure
```

**The build target and the CTest name differ, deliberately and confusingly.** The executable is `phase_observability_identity_integration_test` (the convention in that directory); `PhaseObservabilityIdentityTest` is only the `add_test` name. Passing the test name to `--target` fails with "unknown target", so build the executable and filter CTest by the test name.

Note the filter has **no `^...\.` anchor and no trailing dot**. This target is registered with `add_test`, so its CTest name is exactly `PhaseObservabilityIdentityTest`; the `Suite.Case` form only exists for targets that use `gtest_discover_tests`. An anchored filter here would match zero tests, and a zero-match CTest filter **reports success** — so a green result would be meaningless. Confirm a test actually ran.

Expected: build failure — `AnalyzeProject` takes two arguments and there is no third parameter.

- [ ] **Step 3: Add the parameter and the spans**

In `include/veritas/analysis/ProjectAnalyzer.h`, add the include and change the declaration:

```cpp
#include "veritas/core/RunMetrics.h"

  // AnalyzeProject runs the full pipeline on the specified project.
  //
  // When `metrics` is non-null the pipeline records per-phase spans into it.
  // The recorder is caller-owned so that a run which fails part-way still
  // yields its partial timeline. Metrics are non-semantic: they never enter
  // any content-addressed identity (design section 5.2).
  StatusOr<ProjectAnalysisResult> AnalyzeProject(
      const ProjectAnalysisRequest& request, const AnalysisConfig& config,
      core::RunMetrics* metrics = nullptr);
```

In `src/analysis/ProjectAnalyzer.cpp`, thread the parameter and add spans at exactly these sites, using `core::PhaseSpan span(metrics, "<name>", core::SpanMode::kBearing)` unless stated otherwise:

| Span | Site in `Impl::AnalyzeProject` |
| --- | --- |
| `m1.ingest` | around `build::ResolveProjectInput` + `build::LoadProjectManifest` (`:362-367`) |
| `m4.local_analysis` | around `pipeline::RunLocalAnalysis` (`:370`) |
| `m5.svf` | around `svf_stage_->Analyze` (`:380`) |
| `m5.model_bundle_load` | around `semantic::ModelBundle::Load` (`:388-392`) |
| `m5.merge_svf_facts` | around `svf::MergeSvfFactsV2` (`:395-396`) |
| `m6.cpg_projection` | around `cpg::BuildThinCpg` (`:409-414`) |
| `m2m3.publish_summaries` | around the coordinator open, `PersistManifestContext`, and `Publish` (`:424-437`) |
| `facts.batch_assemble` | around `facts::MakeAnalysisFactBatch` (`:332`) |
| `facts.store_open` | around `facts::FactStore::Open` (`:333-336`) |
| `facts.publish` | around `bus.Publish` (`:339`) |

`RunWpa` gains the parameter and passes it into the request:

```cpp
Status RunWpa(const std::filesystem::path &output_root,
              const AnalysisConfig &config,
              const std::vector<summary::v2::FunctionSummary> &summaries,
              const build::AnalysisManifest &manifest,
              ProjectAnalysisResult *result,
              core::RunMetrics *metrics) {
```

and, where `wpa_request` is built:

```cpp
  wpa_request.metrics = metrics;
```

In `include/veritas/wpa/WpaOrchestrator.h`:

```cpp
#include "veritas/core/RunMetrics.h"

struct WpaRunRequest {
  facts::AnalysisRunManifest run;
  std::span<const summary::SummaryArtifact> summaries;
  const analysis::semantic::ModelBundle* models = nullptr;
  std::span<const WpaComponentKind> components;
  WpaExecutionLimits limits;
  // Optional recorder. Non-owning, not hashed, and excluded from every
  // identity derivation (design section 5.2, Rule 2).
  core::RunMetrics* metrics = nullptr;
};
```

- [ ] **Step 4: Run the identity test and the existing analyzer suite**

```bash
cmake --build build --target phase_observability_identity_integration_test \
  project_analyzer_integration_test project_analyzer_wpa_integration_test -j 8
ctest --test-dir build -R "PhaseObservabilityIdentityTest|ProjectAnalyzer" --output-on-failure
```

Expected: all pass. The identity test proves the guard; the existing tests prove the defaulted parameter changed nothing.

- [ ] **Step 5: Commit**

```bash
git add include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp \
        include/veritas/wpa/WpaOrchestrator.h \
        tests/integration/analysis/PhaseObservabilityIdentityTest.cpp \
        tests/integration/analysis/CMakeLists.txt
git commit -m "feat(analysis): record top-level phase spans and guard run identity" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: The sampler thread and the interval join

**Files:**
- Modify: `include/veritas/core/RunMetrics.h` (add `class SeriesBuffer`; declare `CurrentResidentBytes`, `CurrentFootprintBytes`; add a `series()` accessor to `RunMetrics`)
- Modify: `src/core/RunMetrics.cpp` (sampler, thinning, join)
- Modify: `tests/unit/core/RunMetricsTest.cpp`

**Interfaces:**
- Consumes: Task 1's recorder.
- Produces: `std::uint64_t core::CurrentResidentBytes()`, `std::uint64_t core::CurrentFootprintBytes()`; `core::SeriesBuffer` with `explicit SeriesBuffer(std::size_t capacity = 65536)`, `void Append(MemorySample)`, `const std::vector<MemorySample>& samples() const`, `std::uint64_t decimation() const`, `std::size_t capacity() const`; `SeriesBuffer& RunMetrics::series()`; `RunMetricsStats::series`, `series_decimation`, `peak_rss_bytes`, `peak_footprint_bytes`, `peak_at`; `SpanStats::memory` populated for bearing spans.

**Why `SeriesBuffer` is a class and not a field.** The append-and-thin policy is the one piece of the recorder a second thread touches, and the plan requires these tests not to sleep. A separate class with a direct `Append` makes the policy testable with no thread and no test-only seam in the production API — the alternative was a `ForTest` setter plus a decimation getter, two seams that exist only for tests. It also splits the sampler's two concerns cleanly: the thread handle and stop flag stay in the sampler, the data and its policy live in the buffer.

- [ ] **Step 1: Write the failing tests**

These tests must not sleep and must not start the sampler. Drive the series through `RunMetrics::series()`, which returns a `SeriesBuffer&`; the tests call `Append` directly, which is the same code path the sampler thread runs. No test-only seam is needed. Keep every case in the `RunMetricsTest` suite so the filtered CTest command below still selects them — a second suite in the same file would be silently excluded by the `^RunMetricsTest\.` filter.

```cpp
// Appends one memory sample at time t, with resident set and physical
// footprint both in bytes.
void AppendSample(RunMetrics& metrics, std::chrono::milliseconds t,
                  std::uint64_t rss, std::uint64_t footprint) {
  metrics.series().Append(MemorySample{t, rss, footprint});
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
```

- [ ] **Step 2: Run them to confirm they fail**

```bash
cmake --build build --target RunMetricsTest -j 8
ctest --test-dir build -R "^RunMetricsTest\." --output-on-failure
```

- [ ] **Step 3: Implement the platform readers and the sampler**

Add to `src/core/RunMetrics.cpp`:

```cpp
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

std::uint64_t CurrentResidentBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info {};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.resident_size);
#else
  // Linux: /proc/self/statm field 2 is the resident set in pages.
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) return 0;
  unsigned long total_pages = 0;
  unsigned long resident_pages = 0;
  const int matched = std::fscanf(file, "%lu %lu", &total_pages,
                                 &resident_pages);
  std::fclose(file);
  if (matched != 2) return 0;
  return static_cast<std::uint64_t>(resident_pages) * 4096u;
#endif
}

std::uint64_t CurrentFootprintBytes() {
#if defined(__APPLE__)
  task_vm_info_data_t info {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.phys_footprint);
#else
  // Linux has no separate physical-footprint counter in this sense; the
  // resident set is the honest answer.
  return CurrentResidentBytes();
#endif
}
```

The sampler lives in `Impl`:

```cpp
  // The thread handle and its stop flag only. The samples and the thinning
  // policy live in SeriesBuffer, which is the object the tests drive directly.
  struct Sampler {
    pthread_t thread {};
    std::atomic<bool> stop {false};
    bool started = false;
  };
  Sampler sampler;
  SeriesBuffer series;
  std::chrono::steady_clock::time_point run_start;
  // Set when the sampler could not start: samples are then taken at span
  // boundaries instead of on a timer, and the artifact reports itself
  // incomplete.
  bool boundary_only = false;

  static void* SamplerMain(void* arg);
```

And in the header, beside `RunMetrics`:

```cpp
// SeriesBuffer holds the memory samples and owns the bounded-memory policy.
//
// It is the one part of the recorder a second thread appends to, so it is a
// class with a direct Append rather than a field: tests exercise the policy
// without starting a thread, and the sampler's two concerns stay separate.
class SeriesBuffer {
 public:
  explicit SeriesBuffer(std::size_t capacity = 65536);

  // Append one sample. While the buffer is below capacity this is a push.
  // On overflow it drops every other retained sample, halves the size, and
  // doubles the decimation factor, so the whole timeline stays covered at
  // coarser resolution rather than the early curve being lost. The first
  // sample and the newest are never discarded.
  void Append(MemorySample sample);

  const std::vector<MemorySample>& samples() const { return samples_; }
  std::uint64_t decimation() const { return decimation_; }
  std::size_t capacity() const { return capacity_; }

 private:
  std::size_t capacity_;
  std::uint64_t decimation_ = 1;
  std::vector<MemorySample> samples_;
};
```

Rules to implement exactly:

- Start the sampler in the constructor **only** when `options.sample_interval > 0`. Record `run_start` there.
- The thread loop sleeps `sample_interval`, measures resident and footprint, and calls `series.Append(...)`. The buffer's own policy handles overflow; the loop does not manage capacity, a cursor, or decimation itself.
- `pthread_create` failure: `Diagnose("sampler thread unavailable: ...")`, set `boundary_only = true`, and from then on append one sample in `BeginSpan` and one in `EndSpan`, but only while a **bearing** span is open, so the boundary series stays sparse rather than one sample per component.
- The destructor sets `stop`, then `pthread_join`s if started. Join before reading the buffer.
- No lock on the analysis path: only the sampler thread appends during the run, and only `TakeStats` reads, after the join. When `boundary_only` is set there is no sampler thread at all, and the appends happen on the analysis thread.
- `TakeStats` converts each bearing accumulator's `first_start`/`last_end` to millisecond offsets from `run_start` and applies the join, with the boundary rule **start-inclusive, end-inclusive**. For a bearing span that occurred more than once, the window is the union `[first_start, last_end]`, and that is what the memory figures describe — not a per-occurrence average; say so in a comment. It also computes `peak_rss_bytes`, `peak_footprint_bytes` and `peak_at` over the whole series, and copies the used series into `stats.series` (or leaves it empty when `emit_series` is false), setting `series_decimation` from the buffer. It **copies** rather than moves: the pinned interface exposes only a `const` view of the buffer, so a move is not available without widening it, and the copy is once per run.

**`SpanStats::memory` is engaged only when a sample actually falls inside the span's window.** `JoinMemory` returns `std::nullopt` when the series is empty and also when no sample lies in `[first_start, last_end]`. A populated block therefore means "measured". An all-zero block is indistinguishable from a measured zero, and the renderer's own contract — and the memory column's reason for existing — is that an absent measurement is visibly absent. This is a spec correction made after Task 4 was first reviewed; the artifact bytes change as a result, so any test asserting a zeroed `memory` block must expect absence instead.

Link: `src/core/CMakeLists.txt` already gained `Threads::Threads` in Task 1.

- [ ] **Step 4: Run the tests**

```bash
cmake --build build --target RunMetricsTest -j 8
ctest --test-dir build -R "^RunMetricsTest\." --output-on-failure
```

Expected: all pass — 16 cases, the 11 from Task 1 plus the five added here. Confirm the count in the output rather than trusting the exit code, since a filter matching zero tests also reports success.

- [ ] **Step 5: Commit**

```bash
git add include/veritas/core/RunMetrics.h src/core/RunMetrics.cpp \
        tests/unit/core/RunMetricsTest.cpp
git commit -m "feat(core): sample resident set and footprint, and join it to span intervals" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: The inventory and the store read-back

**Files:**
- Create: `include/veritas/observability/StoreSummary.h`
- Create: `src/observability/StoreSummary.cpp`
- Modify: `src/observability/CMakeLists.txt` (add `StoreSummary.cpp`; link `veritas_summarydb`; add this library's own build-identity compile definitions)
- Modify: `CMakeLists.txt` **only if** `veritas_observability` is added before `src/facts`/`src/summarydb` — it is added after `src/evidence`, so no change is needed.
- Create: `tests/unit/observability/StoreSummaryTest.cpp`
- Modify: `tests/unit/observability/CMakeLists.txt`

**Interfaces:**
- Consumes: `summarydb::MetadataStore::Open(const std::filesystem::path&)` and `MetadataStore::Query(const std::string&, const std::vector<std::string>&)` returning `StatusOr<std::vector<std::vector<std::string>>>`.
- Produces: `StatusOr<StoreSummary> observability::CollectStoreSummary(const std::filesystem::path& output_root)`; the `store` block of `RunReport`; `observability::FillEnvironment(RunEnvironment*)` and `observability::FillInventoryFromManifest(const build::AnalysisManifest&, RunInputInventory*)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/observability/StoreSummaryTest.cpp` with the license header, then:

```cpp
#include "veritas/observability/StoreSummary.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "veritas/summarydb/MetadataStore.h"

namespace veritas::observability {
namespace {

namespace fs = std::filesystem;

// A unique temp directory per case, so the cases never share a store.
fs::path FreshDir(const std::string& tag) {
  const fs::path dir = fs::temp_directory_path() /
                       ("veritas-store-summary-" + tag + "-" +
                        std::to_string(std::rand()));
  std::error_code error;
  fs::remove_all(dir, error);
  return dir;
}

TEST(StoreSummaryTest, CountsEveryPublishedTableIncludingEmptyOnes) {
  // A schema-applied store has the tables but no rows. This is the test that
  // catches a misspelled table name: a name the schema does not define makes
  // the count query fail, and CollectStoreSummary propagates that failure
  // rather than reporting zero.
  const fs::path output_root = FreshDir("empty");
  ASSERT_TRUE(fs::create_directories(output_root));
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();

  // The four published fact tables plus the component-state table the
  // cross-check reads. Ordering is by name, which the next assertion pins.
  ASSERT_EQ(summary->tables.size(), 5u);
  for (const TableRowCount& table : summary->tables) {
    EXPECT_EQ(table.rows, 0u) << table.table;
  }
  EXPECT_TRUE(std::is_sorted(
      summary->tables.begin(), summary->tables.end(),
      [](const TableRowCount& left, const TableRowCount& right) {
        return left.table < right.table;
      }));
  // An empty store is a success, not a failure. That is already asserted by
  // reaching this line at all: CollectStoreSummary returns non-OK if any count
  // query fails.
  //
  // Do NOT add an assertion about `cross_checks` here. This collector leaves
  // that vector empty by design — comparing against the in-memory count is the
  // caller's job — so any claim about its contents is vacuously true in this
  // test and can never fail. The cross-check assertions belong where the vector
  // is actually populated.
}

TEST(StoreSummaryTest, CountsInsertedRows) {
  const fs::path output_root = FreshDir("rows");
  ASSERT_TRUE(fs::create_directories(output_root));
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  // provenance_nodes has NO foreign-key parent — only five NOT NULL columns and
  // the composite primary key (run_id, output_fact_id, witness_id) — so one
  // synthetic row needs no other table to exist first, and the insert cannot
  // fail for a reason unrelated to counting. Read the columns from
  // `src/summarydb/schema/v3.sql` rather than assuming them: an earlier draft
  // of this plan inserted into a `node_id` column this table does not have.
  ASSERT_TRUE(store
                  ->Execute("INSERT INTO provenance_nodes "
                            "(run_id, output_fact_id, witness_id, selected, "
                            "producer_kind) VALUES (?, ?, ?, ?, ?)",
                            {"run:sha256:test", "fact:sha256:test",
                             "witness:sha256:test", "0", "0"})
                  .ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();
  const auto found = std::find_if(
      summary->tables.begin(), summary->tables.end(),
      [](const TableRowCount& table) {
        return table.table == "provenance_nodes";
      });
  ASSERT_NE(found, summary->tables.end());
  EXPECT_EQ(found->rows, 1u);
}

TEST(StoreSummaryTest, GroupsStoreBytesByTopLevelEntryWithoutAbsolutePaths) {
  const fs::path output_root = FreshDir("bytes");
  ASSERT_TRUE(fs::create_directories(output_root / "cas"));
  std::ofstream(output_root / "cas" / "one.bin") << "12345678";

  // `metadata.db` must be a REAL store, not a placeholder file. The collector
  // queries it, and it propagates a failed count query rather than reporting
  // zero, so a four-byte text file makes it fail with SQLite's "file is not a
  // database" and this test unsatisfiable. Create the schema instead.
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();
  ASSERT_TRUE(store->ApplySchema().ok());

  auto summary = CollectStoreSummary(output_root);
  ASSERT_TRUE(summary.ok()) << summary.status().message();
  ASSERT_FALSE(summary->bytes.empty());
  EXPECT_TRUE(std::is_sorted(
      summary->bytes.begin(), summary->bytes.end(),
      [](const NamedBytes& left, const NamedBytes& right) {
        return left.name < right.name;
      }));
  for (const NamedBytes& entry : summary->bytes) {
    // Names are relative to the output root; an absolute path here would make
    // two machines' artifacts differ for no semantic reason.
    EXPECT_EQ(entry.name.find(output_root.string()), std::string::npos);
    EXPECT_EQ(entry.name.find('/'), std::string::npos);
  }
}

TEST(StoreSummaryTest, ReportsFailureRatherThanZeroForAMissingStore) {
  const auto summary = CollectStoreSummary(FreshDir("absent"));
  EXPECT_FALSE(summary.ok());
}

}  // namespace
}  // namespace veritas::observability
```

`FreshDir` returns a path that does not exist, so the "absent" case needs no creation.

Register the target at the end of `tests/unit/observability/CMakeLists.txt`, mirroring the `RunReportTest` block that Task 2 added, and adding `veritas_summarydb` because this test opens a store directly:

```cmake
add_executable(StoreSummaryTest StoreSummaryTest.cpp)
target_link_libraries(StoreSummaryTest PRIVATE
  veritas_observability
  veritas_summarydb
  veritas_build
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(StoreSummaryTest)
gtest_discover_tests(StoreSummaryTest DISCOVERY_TIMEOUT 60)
```

`veritas_build` and `veritas_test_support` are needed only by the last case, which builds a manifest through the real M1 ingestion (`veritas_build::ResolveProjectInput` and `LoadProjectManifest`) and the `multiple_tus` fixture. `veritas_analysis` is **not** needed: `ProjectAnalysisRequest` is a header-only struct, and this test never runs the analyzer, so no SVF and no WPA are linked or executed.

- [ ] **Step 2: Run it to confirm it fails**

- [ ] **Step 3: Implement the collector**

Create `src/observability/StoreSummary.cpp` with the license header, then:

```cpp
#include "veritas/observability/StoreSummary.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::observability {

namespace {

// Every table the report counts. The four analysis tables are published
// content; wpa_component_states_v2 is the component-RESULT table, read so the
// cross-check can compare the store's component count against the expected
// count the run held in memory.
//
// Note the _v2 suffix. `wpa_component_states` (schema v1) is the SCC
// convergence state and holds a different row count, so "correcting" this name
// to v1 would silently make the cross-check meaningless while still passing.
constexpr std::string_view kCountedTables[] = {
    "analysis_facts", "provenance_edges", "provenance_nodes",
    "run_fact_bindings", "wpa_component_states_v2"};

// Groups on-disk bytes by the first path component under output_root, so the
// artifact names stores (cas, metadata.db, ...) rather than individual files,
// and never contains an absolute path.
StatusOr<std::vector<NamedBytes>> MeasureStoreBytes(
    const std::filesystem::path& output_root) {
  std::map<std::string, std::uint64_t> totals;
  std::error_code error;
  const auto options =
      std::filesystem::directory_options::skip_permission_denied;
  std::filesystem::recursive_directory_iterator it(output_root, options, error);
  const std::filesystem::recursive_directory_iterator end;

  // The error must be read in THREE places, not one, because the two that end
  // the walk are exactly the ones a check inside the loop body cannot see. A
  // constructor that fails sets the iterator to `end`, so the loop is never
  // entered; and an `increment` that fails while advancing to `end` exits the
  // loop with no further iteration. Either way a failed measurement would
  // return OK with an empty or truncated byte list — the "plausible-looking
  // partial summary" this design exists to prevent. Verified against the real
  // iterator: a nonexistent root yields `it == end` with `ec = ENOENT`, and a
  // file root yields `ec = ENOTDIR`.
  if (error) {
    return Status::Internal("cannot walk " + output_root.string() + ": " +
                            error.message());
  }
  for (; it != end; it.increment(error)) {
    if (error) {
      return Status::Internal("cannot walk " + output_root.string() + ": " +
                              error.message());
    }
    // Read the error from `is_regular_file` IMMEDIATELY, and do not fold it
    // into the `continue`. The `continue` is correct for a directory or a
    // symlink — those are not failures — but a stat that FAILED also arrives
    // here as false, and the loop's own `increment(error)` clears `error` on
    // success, so a skipped entry would be silently dropped from the byte
    // total with nothing reporting it. Same rule as everywhere else in this
    // design: a failed measurement must not read as a smaller measurement.
    const bool is_regular = it->is_regular_file(error);
    if (error) {
      return Status::Internal("cannot stat " + it->path().string() + ": " +
                              error.message());
    }
    if (!is_regular) continue;
    const auto relative =
        std::filesystem::relative(it->path(), output_root, error);
    if (error) {
      return Status::Internal("cannot relativize " + it->path().string() +
                              ": " + error.message());
    }
    const auto first = relative.begin();
    if (first == relative.end()) continue;
    const std::uint64_t size = it->file_size(error);
    if (error) {
      return Status::Internal("cannot size " + it->path().string() + ": " +
                              error.message());
    }
    totals[first->string()] += size;
  }
  // The third read: an `increment` that failed while advancing to `end` exits
  // the loop above with `error` set and no iteration left to observe it.
  if (error) {
    return Status::Internal("cannot walk " + output_root.string() + ": " +
                            error.message());
  }
  // std::map iterates in key order, so the vector is already sorted by name.
  std::vector<NamedBytes> bytes;
  bytes.reserve(totals.size());
  for (const auto& entry : totals) {
    bytes.push_back(NamedBytes{entry.first, entry.second});
  }
  return bytes;
}

}  // namespace

StatusOr<StoreSummary> CollectStoreSummary(
    const std::filesystem::path& output_root) {
  std::error_code error;
  if (!std::filesystem::exists(output_root / "metadata.db", error) || error) {
    return Status::NotFound("no metadata.db under " + output_root.string());
  }

  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  if (!store.ok()) return store.status();

  StoreSummary summary;
  for (const std::string_view table : kCountedTables) {
    auto rows = store->Query("SELECT COUNT(*) FROM " + std::string(table), {});
    if (!rows.ok()) return rows.status();
    if (rows->empty() || rows->front().empty()) {
      return Status::Internal("count query returned no rows for " +
                              std::string(table));
    }
    // std::stoull throws; the project builds with -fno-exceptions.
    const unsigned long long count =
        std::strtoull(rows->front().front().c_str(), nullptr, 10);
    summary.tables.push_back(
        TableRowCount{std::string(table), static_cast<std::uint64_t>(count)});
  }
  std::sort(summary.tables.begin(), summary.tables.end(),
            [](const TableRowCount& left, const TableRowCount& right) {
              return left.table < right.table;
            });

  auto bytes = MeasureStoreBytes(output_root);
  if (!bytes.ok()) return bytes.status();
  summary.bytes = std::move(*bytes);
  return summary;
}
```

Every `std::filesystem` call uses the `std::error_code` overload, because the throwing ones are banned. `cross_checks` is left empty here: the store side cannot fill it, since comparing against the in-memory count is the caller's job (Task 6).

- [ ] **Step 3b: Add the environment and input-inventory fillers**

Both live in `StoreSummary.{h,cpp}`, not in `RunReport.{h,cpp}`: they need `veritas/build/AnalysisManifest.h` and POSIX headers, and `RunReport.h` deliberately includes neither.

**A constraint that is not negotiable: do not touch `src/analysis/CMakeLists.txt`.** Its `VERITAS_CPP_BUILD_FINGERPRINT` definition (line 46) feeds `CppToolchainIdentity`, which feeds `engine_toolchain_identity`, which feeds `run_id` and every digest chained off it. Re-spelling it so both libraries could share one definition would move every content-addressed identity in the project. `veritas_observability` therefore gets its **own** definitions:

```cmake
target_compile_definitions(veritas_observability PRIVATE
  VERITAS_OBSERVABILITY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  VERITAS_OBSERVABILITY_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  VERITAS_OBSERVABILITY_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
)
```

Add to the header:

```cpp
// FillEnvironment reads the machine and build identity for the artifact's
// environment block: uname for os/arch, sysctlbyname on Darwin for cores, RAM
// and CPU model, sysconf(_SC_NPROCESSORS_ONLN) and sysinfo on Linux for cores
// and RAM, and this library's own compile definitions for the build identity.
// It deliberately does not reuse the analysis library's build fingerprint,
// which is an identity input.
//
// `cpu_model` is Darwin-only: the Linux branch fills `cores` and `ram_bytes`
// and leaves it empty. An empty string is the honest rendering of "not
// measured here" — do not fill it with a placeholder, and do not let a comment
// claim the Linux branch provides it.
void FillEnvironment(RunEnvironment* environment);

// FillInventoryFromManifest copies the input-scale fields the manifest
// already carries. It is defined in StoreSummary.cpp; the reason it lives in
// this header rather than RunReport.h is that it needs AnalysisManifest.h,
// which RunReport.h deliberately does not include.
void FillInventoryFromManifest(const build::AnalysisManifest& manifest,
                               RunInputInventory* input);
```

Add `#include "veritas/build/AnalysisManifest.h"` to the header, and to the `.cpp`:

```cpp
#include <sys/utsname.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include "veritas/core/Version.h"

void FillInventoryFromManifest(const build::AnalysisManifest& manifest,
                               RunInputInventory* input) {
  input->translation_units = manifest.translation_units.size();
  input->compiler_id = manifest.context.compiler_id;
  input->compiler_version = manifest.context.compiler_version;
  input->target_triple = manifest.context.target_triple;
  input->source_tree_hash = manifest.context.source_tree_hash;
  input->include_closure_hash = manifest.context.include_closure_hash;
}

void FillEnvironment(RunEnvironment* environment) {
  struct utsname uts {};
  if (uname(&uts) == 0) {
    environment->os = uts.sysname;
    environment->arch = uts.machine;
  }
#if defined(__APPLE__)
  const auto sysctl_u64 = [](const char* name) -> std::uint64_t {
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return 0;
    return value;
  };
  char brand[256] = {};
  std::size_t brand_size = sizeof(brand);
  if (sysctlbyname("machdep.cpu.brand_string", brand, &brand_size, nullptr, 0) ==
      0) {
    environment->cpu_model = brand;
  }
  environment->cores = sysctl_u64("hw.ncpu");
  environment->ram_bytes = sysctl_u64("hw.memsize");
#else
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (cores > 0) environment->cores = static_cast<std::uint64_t>(cores);
  struct sysinfo info {};
  if (sysinfo(&info) == 0) {
    environment->ram_bytes =
        static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
  }
#endif
  environment->build_type = VERITAS_OBSERVABILITY_BUILD_TYPE;
  environment->host_compiler = std::string(VERITAS_OBSERVABILITY_COMPILER_ID) +
                               " " +
                               VERITAS_OBSERVABILITY_COMPILER_VERSION;
  const auto version = veritas::GetVersion();
  environment->veritas_version = std::to_string(version.major) + "." +
                                 std::to_string(version.minor) + "." +
                                 std::to_string(version.patch);
  environment->git_revision = version.git_revision;
}
```

Append these two cases to the same test file:

```cpp
TEST(StoreSummaryTest, FillsEnvironmentFromTheMachine) {
  RunEnvironment environment;
  FillEnvironment(&environment);
  EXPECT_FALSE(environment.os.empty());
  EXPECT_FALSE(environment.arch.empty());
  EXPECT_GT(environment.cores, 0u);
  EXPECT_GT(environment.ram_bytes, 0u);
  EXPECT_FALSE(environment.host_compiler.empty());
  // A version string with no dot would mean the fields were never filled.
  EXPECT_NE(environment.veritas_version.find('.'), std::string::npos);
}

TEST(StoreSummaryTest, FillsInputInventoryFromTheManifest) {
  // The manifest is built by the same ingestion the analyzer uses, so this
  // asserts the copy, not the ingestion.
  const auto project = testing::FixtureProject("multiple_tus");
  auto input = veritas::build::ResolveProjectInput(
      veritas::analysis::ProjectAnalysisRequest{
          .project_root = project,
          .output_root = FreshDir("inventory")});
  ASSERT_TRUE(input.ok()) << input.status().message();
  auto manifest = veritas::build::LoadProjectManifest(*input);
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();

  RunInputInventory inventory;
  FillInventoryFromManifest(*manifest, &inventory);
  EXPECT_EQ(inventory.translation_units, manifest->translation_units.size());
  EXPECT_FALSE(inventory.compiler_id.empty());
  EXPECT_FALSE(inventory.target_triple.empty());
}
```

That last test needs `veritas_build` and `veritas_analysis` on the link line plus the fixture support, which makes it heavier than a unit test. If the link line grows awkwardly, keep `FillsInputInventoryFromTheManifest` and add the three libraries; do not drop the test, since an unfilled input block is the kind of zero that reads as a measurement.

- [ ] **Step 4: Run the tests and confirm they pass**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(observability): collect published row counts and store sizes" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 6: Wire the CLI end to end

After this task a real run produces the report and the artifact.

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Modify: `src/tools/CMakeLists.txt` (link `veritas_observability` into `veritas-build`)
- Modify: `tests/integration/build/CMakeLists.txt` (link `LLVM` into `VeritasBuildAnalyzeCliTest`, plus `target_include_directories(... SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})` — the include line is not optional; linking the imported `LLVM` target brings no include directories, and without it the test would compile `llvm/Support/JSON.h` from whatever LLVM is on the default search path while linking another. Add `#include <llvm/Support/JSON.h>` and `#include <optional>` to the test.)
- Modify: `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`

**Interfaces:**
- Consumes: everything above; `analysis::ProjectAnalysisResult`, `build::AnalysisManifest`.
- Produces: the five `--metrics*` flags, the stdout report block, `<output>/run-metrics.json`, and stderr diagnostics.

- [ ] **Step 1: Write the failing CLI tests**

```cpp
TEST(VeritasBuildAnalyzeCliTest, WritesRunMetricsByDefault) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-cli-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(output / "run-metrics.json"));
  EXPECT_NE(result.stdout_text.find("Analysis phase report"), std::string::npos)
      << result.stdout_text;

  // Both ingest spans must appear. They are the measurable form of the
  // duplication spec section 10 records, and the first finding this feature
  // exists to surface — so their absence is a failure, not a detail.
  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  const std::string json = buffer.str();
  EXPECT_NE(json.find("cli.ingest"), std::string::npos) << json.substr(0, 500);
  EXPECT_NE(json.find("m1.ingest"), std::string::npos) << json.substr(0, 500);

  // Parse, rather than searching for a substring, because the root's NAME
  // cannot distinguish the thing this asserts. The synthetic-root fallback in
  // TakeStats is ALSO named "run", so `json.find("\"name\": \"run\"")` passes
  // identically whether the CLI's own run span was promoted or the fallback
  // was used — and it passes even if `run` was left open at TakeStats, which
  // folds nothing and yields count 0 with wall 0.
  auto parsed = llvm::json::parse(json);
  ASSERT_TRUE(static_cast<bool>(parsed)) << json.substr(0, 500);
  const llvm::json::Object* root = parsed->getAsObject();
  ASSERT_NE(root, nullptr);
  const llvm::json::Array* phases = root->getArray("phases");
  ASSERT_NE(phases, nullptr);
  ASSERT_FALSE(phases->empty());
  const llvm::json::Object* run = phases->front().getAsObject();
  ASSERT_NE(run, nullptr);
  EXPECT_EQ(*run->getString("name"), "run");
  // These two are the discriminating assertions: an open or absent root span
  // folds nothing, so count is 0 and the wall is 0.
  EXPECT_EQ(*run->getInteger("count"), 1);
  const std::optional<std::int64_t> wall = run->getInteger("wall_inclusive_ns");
  ASSERT_TRUE(wall.has_value());
  EXPECT_GT(*wall, 0);
}

TEST(VeritasBuildAnalyzeCliTest, MetricsFalseWritesNoArtifactAndNoReport) {
  const auto project = testing::FixtureProject("multiple_tus");
  // The prefix must differ from the one `PhaseObservabilityIdentityTest` uses
  // for its metrics-off run. Both files call `std::rand()` unseeded, so the
  // first value is identical in every process, and an identical prefix makes
  // the two cases resolve to the SAME directory. Each gtest case is its own
  // CTest entry, so under `ctest -j` they would analyze into one store — this
  // repository's recorded single-writer-per-store hazard.
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-disabled-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string(),
                                       "--metrics", "false"});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_FALSE(fs::exists(output / "run-metrics.json"));
  EXPECT_EQ(result.stdout_text.find("Analysis phase report"), std::string::npos)
      << result.stdout_text;
}

TEST(VeritasBuildAnalyzeCliTest, RejectsANonBooleanMetricsValue) {
  const auto result = RunVeritasBuild(
      {"analyze", "--project", "/tmp", "--metrics", "maybe"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("--metrics must be true or false"),
            std::string::npos);
}

TEST(VeritasBuildAnalyzeCliTest, AcceptsZeroSamplingInterval) {
  // Zero disables the series entirely: no sampler thread and no samples. The
  // run must still succeed and still produce a parseable artifact, because
  // turning the series off is a choice about the series, not a failure of the
  // run. (Span-boundary sampling is a different mechanism — the fallback when
  // pthread_create fails — and is not reachable through this flag.)
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-zero-" + std::to_string(std::rand()));
  const auto result =
      RunVeritasBuild({"analyze", "--project", project.string(), "--output",
                       output.string(), "--metrics-interval-ms", "0"});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  EXPECT_NE(buffer.str().find("veritas.run-metrics.v1"), std::string::npos);
}

TEST(VeritasBuildAnalyzeCliTest, HonoursTopNAndMetricsPath) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-path-" + std::to_string(std::rand()));
  const auto custom = output / "custom-metrics.json";
  const auto result = RunVeritasBuild(
      {"analyze", "--project", project.string(), "--output", output.string(),
       "--metrics-top-n", "3", "--metrics-path", custom.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(custom));
  // The default location is not also written: --metrics-path replaces it.
  EXPECT_FALSE(fs::exists(output / "run-metrics.json"));
}

TEST(VeritasBuildAnalyzeCliTest, EmitsNoAbsolutePathInTheArtifact) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-paths-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;

  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  const std::string json = buffer.str();
  ASSERT_FALSE(json.empty());
  // Spec section 6.5 rule 4: an absolute path would make two machines'
  // artifacts differ for no semantic reason, so the artifact carries none.
  EXPECT_EQ(json.find(output.string()), std::string::npos)
      << json.substr(0, 500);
  EXPECT_EQ(json.find(project.string()), std::string::npos)
      << json.substr(0, 500);
}
```

- [ ] **Step 2: Run them to confirm they fail**

- [ ] **Step 3: Implement the flags, the report, and the artifact**

In `AnalyzeArguments` add:

```cpp
  bool metrics = true;
  std::size_t metrics_interval_ms = 250;
  std::size_t metrics_top_n = 10;
  bool metrics_series = true;
  fs::path metrics_path;  // empty = <output>/run-metrics.json
```

Parse them with the existing `take_value` idiom, following `--field-sensitive` exactly for the two booleans and `ParsePositiveSize` for the integers. `--metrics-interval-ms` must accept `0`, which disables the series entirely — `ParsePositiveSize` rejects zero by design, so add `ParseUnsigned` beside it rather than reusing the positive-only helper.

In `Analyze`, construct the recorder **first**, before any ingest, and open the root span there. Position matters: if the recorder is built after `ResolveProjectInput`, the `run` span cannot enclose `cli.ingest`, and spec section 10's headline finding — that the CLI ingests the project and then the analyzer ingests it again — becomes invisible in the artifact.

```cpp
  // The recorder is built before anything else so the "run" root encloses the
  // whole command, and so the rootless-accumulator promotion in TakeStats
  // applies rather than the synthetic-"run" fallback.
  veritas::core::RunMetricsOptions metrics_options;
  if (parsed->metrics) {
    metrics_options.sample_interval =
        std::chrono::milliseconds(parsed->metrics_interval_ms);
  }
  metrics_options.top_n = parsed->metrics_top_n;
  metrics_options.emit_series = parsed->metrics_series;
  veritas::core::RunMetrics metrics(metrics_options);
  veritas::core::RunMetrics* const recorder =
      parsed->metrics ? &metrics : nullptr;
  veritas::core::PhaseSpan run_span(recorder, "run",
                                    veritas::core::SpanMode::kBearing);

  // The CLI's own ingest: resolve the project, load the manifest, write the
  // diagnostic manifest. The analyzer performs the same two steps again under
  // `m1.ingest`, so this pair is the measurable form of that duplication.
  {
    veritas::core::PhaseSpan ingest_span(recorder, "cli.ingest",
                                         veritas::core::SpanMode::kBearing);
    auto input = veritas::build::ResolveProjectInput(request);
    if (!input.ok()) return input.status();
    auto manifest = veritas::build::LoadProjectManifest(*input);
    if (!manifest.ok()) return manifest.status();
    if (auto status = WriteDiagnosticManifest(input->output_root, *manifest);
        !status.ok()) {
      return status;
    }
  }
```

The existing body then follows unchanged, except that `input` and `manifest` must remain in scope for the later print block — so either hoist those two declarations above the `cli.ingest` scope, or move the print block inside it. Prefer hoisting the declarations and keeping the span scope tightly around the three calls.

Then pass the recorder to the analyzer:

```cpp
  auto result = analyzer.AnalyzeProject(request, config, recorder);
```

**`run` must close immediately BEFORE `TakeStats`, not after the artifact is written.** An earlier draft of this plan said the latter, and it is impossible: a span is folded into its accumulator only in `EndSpan` (`src/core/RunMetrics.cpp:438`), so a root still open when `TakeStats` runs reports `count == 0` and `wall_inclusive == 0` — the report's total row would read `0ns` while every child showed a real duration.

The consequence to state plainly rather than hide: rendering the report and writing the artifact fall **outside** `run`, so the total row is the *analysis* wall time, not the command's full wall time. They differ by the render and write, which is small but nonzero. If a future change wants the command's true wall time, the place for it is a separate span that closes last and is not part of the tree's total — not an open root.

After the existing `Analysis complete` block, when `parsed->metrics`, build the `RunReport`, print `RenderRunReportText`, write `RenderRunReportJson` to the artifact path, and print any recorder diagnostics to **stderr**.

**Use two prefixes, not one, and choose them by severity rather than by subsystem.** A missing producer — a counter an uninstrumented stage did not add — is *not* a degradation, and on a healthy metrics-on run there are ten of them. Labelling all ten `metrics degraded:` puts a false alarm in front of the operator ten times per run, which devalues the words for the case that matters. So:

- `veritas-build: metrics degraded: ` — for a **failure**: a failed artifact write, a missing store block, a failed `pthread_create` or memory probe, anything that clears `complete`.
- `veritas-build: metrics note: ` — for a **not-recorded**: an absent counter or a not-yet-populated field, which keeps `complete` as the recorder found it.

The sentence after the prefix carries the detail either way. The analysis exit code is unaffected by any metrics failure (spec section 7.1); a write failure prints a `degraded` line and returns `Status::Ok()`.

Populate the report from these exact sources. **Output-scale counts come from the recorder's counters, not from a second plumbing path** — Task 7 adds them at the producing site, and this task reads them back:

| Report field | Source |
| --- | --- |
| `identity.run_id`, `projection_id`, `revision_id`, `build_variant_id`, `repository_id` | `*result` and `manifest->context` — no new plumbing needed for these five |
| `identity.batch_id`, `svf_config_hash`, `wpa_config_hash`, `engine_toolchain_identity` | **four new fields on `ProjectAnalysisResult`** — see below |

**Four identity fields need plumbing the CLI cannot reach, and leaving them empty is not acceptable.** `SvfConfigurationHash`, `WpaConfigurationHash` and `CppToolchainIdentity` are file-local in `src/analysis/ProjectAnalyzer.cpp` with no declaration under `include/`, and `batch_id` is minted inside the fact bus. So the CLI has no source for them, and the artifact would ship four empty strings.

That is worse than a cosmetic gap, for two reasons. First, the design elsewhere records that the rest of the `AnalysisConfig` is deliberately absent from the artifact *because* `svf_config_hash` and `wpa_config_hash` cover it — with both empty, the artifact records **no configuration at all**, and nothing distinguishes a `--wpa-engine cpp-emergency` run, a `--field-sensitive false` run, or a `--max-alias-pairs` run from the default. Second, the keys are emitted as `""` rather than omitted, so an absent value *diffs as unchanged* between two differently-configured runs — precisely the failure class this feature exists to prevent.

So this task carries a **small authorized cross-task change**: add four fields to `ProjectAnalysisResult` (`include/veritas/analysis/ProjectAnalyzer.h`) and populate them in `RunWpa` (`src/analysis/ProjectAnalyzer.cpp`), which already holds the descriptor it builds and the batch it assembles:

```cpp
  // On ProjectAnalysisResult, alongside wpa_run_id et al.
  std::string svf_configuration_hash;      // descriptor.svf_configuration_hash
  std::string wpa_configuration_hash;      // descriptor.wpa_configuration_hash
  std::string engine_toolchain_identity;   // descriptor / toolchain_identity
  std::string batch_id;                    // core::ToString(batch.batch_id)
```

`RunWpa` already computes all four — `descriptor.svf_configuration_hash` and `descriptor.wpa_configuration_hash` at the point it builds the descriptor, `toolchain_identity` just after, and `batch.batch_id` from the batch it publishes. Set them on `*result` before returning, and populate the identity block from them here.

**And make absence visible rather than plausible.** If any identity field still has no value, **omit the key** rather than emitting `""`. The rule is the same one the memory block follows: an absent value must not be representable as a value, because `""` compares equal across runs that differ.
| `metrics_options`, `metrics` | the recorder: `metrics.TakeStats()` and the options you constructed |
| `conformance_oracle` | `config.run_cpp_conformance_oracle` — the one AnalysisConfig knob neither configuration hash covers (design section 6.3) |
| `environment` | `observability::FillEnvironment` |
| `inventory.input.*` | `manifest->context` |
| `inventory.output.summaries_published` | `result->published_summary_ids.size()` |
| `inventory.output.unknowns_by_reason` | histogram over `result->unknowns`, keyed by `reason`, sorted by key |
| `inventory.output.cpg_nodes`, `cpg_edges` | `result->cpg_node_count`, `result->cpg_edge_count` |
| `inventory.output.svfg_nodes`, `svfg_edges` | counter `svf.svfg_nodes`, `svf.svfg_edges` |
| `inventory.output.components_by_kind` | counters named `wpa.component.<kind-name>.expected` |
| `inventory.output.rooted_input_facts` | counter `facts.rooted_input` |
| `inventory.output.canonical_facts` | counter `facts.canonical` |
| `inventory.incrementality.components_reused` | counter `wpa.components.reused` |
| `inventory.incrementality.components_executed` | counter `wpa.components.executed` |
| `store.*` | `CollectStoreSummary`, plus `cross_checks` pairing `wpa.components.expected` (memory) against the `wpa_component_states_v2` row count (store) |
| `complete`, `diagnostics` | `RunMetricsStats::complete`, `RunMetricsStats::diagnostics` |

A counter that is absent because its producer did not run leaves the field at zero **and** is named in a diagnostic, so an uninstrumented zero is never mistaken for a measured zero. Do not derive a count from a source not listed here.

**Add a divergence diagnostic.** The text report prints the expected-component count as the *sum* of `components_by_kind`, while the store cross-check compares the separate `wpa.components.expected` counter against the store's row count. Both are derived from the same frozen expected set in Task 7, so they should always agree — but if they ever diverge, two numbers in one artifact would both claim to be "expected components" and nothing would say which is right. Compare them while building the report and, on disagreement, record a diagnostic naming both values. A cheap check that makes a silent contradiction loud is exactly what this feature is for.

- [ ] **Step 4: Run the CLI tests and the full build test suite**

```bash
cmake --build build --target veritas-build VeritasBuildAnalyzeCliTest -j 8
ctest --test-dir build -R "VeritasBuildAnalyzeCliTest" --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(build): emit the analyze phase report and run-metrics artifact" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 7: Sub-spans, per-component aggregates, and counters

**Files:**
- Modify: `include/veritas/analysis/svf/SvfAnalysisStage.h`, `src/analysis/svf/SvfAnalysisStage.cpp`
- Modify: `src/analysis/svf/SvfSession.h`, `src/analysis/svf/SvfSession.cpp`
- Modify: `src/wpa/WpaOrchestrator.cpp`
- Modify: `include/veritas/facts/AnalysisFactBus.h`, `src/facts/AnalysisFactBus.cpp`
- Modify: `src/analysis/ProjectAnalyzer.cpp` — the SVF call site must now pass the recorder, and the conformance-oracle request must be given a null one (both below)
- Modify: `tests/integration/analysis/PhaseObservabilityIdentityTest.cpp` (add the span-tree test)
- Modify: `tests/integration/analysis/CMakeLists.txt` (timeout 60 → 180)

**Interfaces:**
- Consumes: `core::PhaseSpan`, `SpanMode::kBearing`, `SpanMode::kDistributed`, `RunMetrics::AddCounter`.
- Produces: `SvfAnalysisStage::Analyze(program_ir, run_context, config, core::RunMetrics* = nullptr)`; `RunWithSvfSession(program_ir, config, callback, core::RunMetrics* = nullptr)`; `AnalysisFactBus::SetMetrics(core::RunMetrics*)`.

- [ ] **Step 1: Instrument the SVF session's five steps**

Each step becomes a `kBearing` span, since the SVF phase is where round 3 measured a +1.66 GiB burst inside one sampling interval:

| Span | Site in `SvfSession.cpp` |
| --- | --- |
| `m5.svf.module_set` | `:103` `buildSVFModule` |
| `m5.svf.svfi` | `:112-113` `SVFIRBuilder::build` |
| `m5.svf.andersen` | `:120-121` `createAndersenWaveDiff` |
| `m5.svf.svfg` | `:128-129` `buildFullSVFG` |
| `m5.svf.map_facts` | `:136` the `callback(view)` call |

Inside the callback, where `view.svfg` is still live, record the SVFG scale as counters — these two accessors are verified to exist:

```cpp
  if (metrics != nullptr) {
    metrics->AddCounter("svf.svfg_nodes",
                        static_cast<std::uint64_t>(view.svfg->getSVFGNodeNum()),
                        "count");
    metrics->AddCounter("svf.svfg_edges",
                        static_cast<std::uint64_t>(view.svfg->getTotalEdgeNum()),
                        "count");
  }
```

`getSVFGNodeNum()` is `SVFG.h:271-274`; `getTotalEdgeNum()` is `GenericGraph.h:428-431`, reachable because `SVFG` derives from `VFG` and thence from `GenericGraph`. `view` is valid only inside the callback, so these counts must be taken there and not later.

**Two call sites in `ProjectAnalyzer.cpp` must change, and they pull in opposite directions.**

1. The M5 call site, currently `svf_stage_->Analyze(local->program_ir, run_context, ToSvfConfig(config))`, must pass the recorder as the new fourth argument. Task 3 left it relying on the default, so without this edit none of the sub-spans you are adding here can ever fire — and your own span-tree test will fail on `m5.svf.andersen` and friends. That failure is the correct signal, not a test to relax.
2. The conformance-oracle request must be given a **null** recorder. `RunWpa` builds it as `WpaRunRequest conformance_request = wpa_request;`, which copies the `metrics` pointer, so the oracle's second full WPA run would record its spans under exactly the same names as the primary run. Every per-component span would then be counted twice, and the percentiles and the top-N list would silently describe two runs blended into one. The report describes the primary run; the oracle is a conformance check, not a phase.

```cpp
  wpa::WpaRunRequest conformance_request = wpa_request;
  conformance_request.run = *conformance_run;
  // The oracle re-runs the same components. Sharing the recorder would double
  // every per-component count and blend two runs into one top-N list.
  conformance_request.metrics = nullptr;
```

Two mechanical requirements for this step, because `SvfSession.cpp` and `SvfAnalysisStage.cpp` live in namespace `veritas::analysis::svf`:

- Every guard is `core::PhaseSpan`, and every mode is `core::SpanMode::...`. An unqualified `PhaseSpan` does not resolve there.
- Both files need `#include "veritas/core/RunMetrics.h"`, and the four signatures gain a defaulted trailing parameter. `SvfAnalysisStage::Analyze` is `virtual`; a grep found no subclass or override anywhere in `tests/`, so widening the signature breaks no fake. Do not change the parameter order or drop the default — Task 3's call site relies on the default until you update it.

The callback already receives `metrics` through the session parameter, so `m5.svf.map_facts` wraps the `callback(view)` call rather than being taken inside `MapSvfFacts`.

- [ ] **Step 2: Instrument the WPA loop**

In `src/wpa/WpaOrchestrator.cpp`, read `request.metrics` once into a local. Around the `for (const auto& scc_id : scc_order)` / `for (const auto component : request.components)` body at `:172`:

| Span | Mode | Site |
| --- | --- | --- |
| `wpa.orchestrate` | `kBearing` | around `Run`'s component loop |
| `wpa.graph_build` | `kBearing` | `:150-170` |
| `wpa.component.materialize` | `kDistributed` | `:176-194` plus `:203-210` |
| `wpa.component.cache_lookup` | `kDistributed` | `:212-214` |
| `wpa.component.execute` | `kDistributed` | `:227` |
| `wpa.component.canonicalize` | `kDistributed` | `:234-238` |
| `wpa.scc_state_flush` | `kBearing` | `:292` |

Give every distributed span a label via `token`/`SetLabel` of the form `<component-kind>/<scc-id>` so top-N entries are attributable. **Do not call `AddCounter` inside the loop** — accumulate into local counters and add them once after the loop, so the counter list stays small and deterministic.

After the loop add:

```cpp
  metrics->AddCounter("wpa.components.expected", expected_count, "count");
  metrics->AddCounter("wpa.components.reused", reused_count, "count");
  metrics->AddCounter("wpa.components.executed", executed_count, "count");
```

Per-kind expected counts are the source of the report's `components_by_kind`, one counter per kind, named `wpa.component.<kind-name>.expected` with `<kind-name>` from `ComponentKindName`. Accumulate into a `std::map` first — this avoids both `.at()`, which the compilation policy forbids, and a fixed-size array indexed by an enum whose range this plan has not verified:

```cpp
  std::map<WpaComponentKind, std::uint64_t> per_kind;
  for (const WpaComponentKey& expected : expected_components) {
    ++per_kind[expected.component];
  }
  for (const auto& entry : per_kind) {
    metrics->AddCounter("wpa.component." +
                            std::string(ComponentKindName(entry.first)) +
                            ".expected",
                        entry.second, "count");
  }
```

Add `#include <map>` to `WpaOrchestrator.cpp`.

Because `wpa.orchestrate` is `kBearing` and the per-component spans are not, `cpu_inclusive` on `wpa.orchestrate` is the CPU cost of the whole component loop — the number round 3 could not attribute.

For the span label, use the verified helper `std::string_view ComponentKindName(WpaComponentKind)` (`include/veritas/wpa/WpaComponent.h:52`). **Qualify the guard as `core::PhaseSpan`**: this file is in namespace `veritas::wpa`, so an unqualified `PhaseSpan` does not resolve.

```cpp
  core::PhaseSpan span(metrics, "wpa.component.execute",
                       core::SpanMode::kDistributed);
  span.SetLabel(std::string(ComponentKindName(key.component)) + "/" +
                core::ToString(key.scc_id));
```

`WpaComponentKey` carries both `scc_id` and `component` (`include/veritas/wpa/WpaRunRepository.h:58-63`), so both fields are already in hand at that site.

Also add the rooted-input and canonical fact counts once, after the batch is assembled, so the inventory has two more counters rather than a second plumbing path:

```cpp
  metrics->AddCounter("facts.rooted_input", batch.rooted_input_fact_ids.size(),
                      "count");
  metrics->AddCounter("facts.canonical", batch.facts.size(), "count");
```

- [ ] **Step 3: Instrument the fact bus**

Add to `AnalysisFactBus`:

```cpp
  // Optional recorder. Non-owning; never read for control flow.
  void SetMetrics(core::RunMetrics* metrics) { metrics_ = metrics; }
```

and wrap `Validate` in `facts.publish.validate` and each sink's `Publish` in `facts.publish.sink.<sink_id>`.

- [ ] **Step 4: Write the test that proves the instrumentation fires**

Nothing so far asserts that a span actually opens. An instrumentation change can compile, run, and record nothing — a wrong null check, a name that does not resolve where you thought, a span opened in a scope that already returned — and no existing test would notice, because every existing test asserts analysis results, and this change deliberately leaves those identical.

Append this case to `tests/integration/analysis/PhaseObservabilityIdentityTest.cpp`. It asserts span **presence and counts, never durations**, so it cannot be flaky. Add `#include <functional>` and `#include <map>` to that file.

```cpp
TEST(PhaseObservabilityIdentityTest, RecordsTheExpectedSpanTree) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-spans-" + std::to_string(std::rand()));
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
  for (const char* name : {"svf.svfg_nodes", "svf.svfg_edges",
                           "wpa.components.expected", "wpa.components.reused",
                           "wpa.components.executed", "facts.rooted_input",
                           "facts.canonical"}) {
    EXPECT_EQ(counters.count(name), 1u) << name << " missing";
  }
  EXPECT_GT(counters["wpa.components.expected"], 0u);
  EXPECT_GT(counters["svf.svfg_nodes"], 0u);
}
```

That file now runs the analyzer three times, so raise its CTest timeout from 60 to 180 seconds in `tests/integration/analysis/CMakeLists.txt`.

If a span name here does not appear, do **not** weaken the assertion to make it pass — an absent span is the finding this step exists to produce. Either the span site is wrong or the name differs; report it.

- [ ] **Step 5: Build the touched targets and run the affected suites**

**Never run a bare `cmake --build build`.** This project has 631 targets and a full build has already stalled one session. Name your targets:

```bash
cmake --build build --target veritas-build VeritasBuildAnalyzeCliTest \
  phase_observability_identity_integration_test project_analyzer_integration_test -j 8
ctest --test-dir build -R "Wpa|Svf|ProjectAnalyzer|PhaseObservabilityIdentity" --output-on-failure
```

Expected: all pass, unchanged analysis results, plus the new case.

**Do not run the motivating leveldb command here.** It takes roughly ten minutes of wall time on this machine, and the design's measurement protocol requires one run per output root, so it belongs to Task 8 where it is explicitly budgeted. The instrumentation's presence is asserted by the test above. If you want one bounded end-to-end look, run the same binary against the `multiple_tus` fixture with a fresh output root under `/tmp`; do not use the leveldb fixture.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(wpa,svf,facts): record sub-spans, component aggregates, and counters" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 8: Measure the overhead, document it, and record the outcome

**Files:**
- Create: `docs/guides/analyze-phase-report-guide.md`
- Modify: `docs/guides/README.md`
- Modify: `docs/specs/veritas-build-analyze-phase-observability-design-spec.md` (verification record)

- [ ] **Step 1: Measure the overhead budget**

Three runs of the same binary with metrics off and three with metrics on, same output-root-per-run convention, worst-of-three reported per series:

This is **six full analyses of the LevelDB fixture, and each one takes roughly ten minutes of wall time on this machine**. Two consequences you must plan for:

- **Run them one at a time, never as one command.** A single `for` loop over all six would run for about an hour and will exceed any bounded command timeout. Run one analysis per invocation, in the background, and wait for it to exit before starting the next.
- **One run per output root.** A second concurrent run against the same output root dies on the RocksDB LOCK, so wait for the process to exit rather than for a log line. The six distinct output roots below already satisfy this.

Metrics **off**, three runs:

```bash
/usr/bin/time -lp ./build/bin/veritas-build analyze \
  --project /Users/skg7on/Workspace/Projects/leveldb \
  --output /tmp/veritas-metrics-off-1 --metrics false
```

then the same command with `--output /tmp/veritas-metrics-off-2` and `-3`.

Metrics **on**, three runs: the same three commands with `--metrics false` removed, writing to `/tmp/veritas-metrics-on-1`, `-2`, and `-3`.

Each run prints its own phase report and writes its own artifact. Compare worst-of-three per series for both CPU (`user` + `sys` from `/usr/bin/time -lp`) and peak resident. Confirm `CMakeCache.txt`'s build type and compiler match between the two series before quoting any delta — on this machine the host compiler moves these numbers more than the change does.

**Report the result whether or not it meets the ≤0.5 % CPU and ≤0.05 GiB ceiling.** A miss is a finding, not a failure to hide. If the six runs cannot be completed, say so and report how many were taken; **do not** extrapolate a series from fewer runs than the budget requires, and do not present a two-run comparison as evidence — this fixture's own spread is 0.72 GiB.

- [ ] **Step 2: Write the reader's guide**

`docs/guides/analyze-phase-report-guide.md`, carrying: how to read the four columns; the diffability rules from spec section 6.5; the full span inventory from spec section 4.6 so that a *missing* row is noticeable; the meaning and the caveat of `cpu_inclusive`; and the boundary rule for the memory join. Add it to `docs/guides/README.md`.

- [ ] **Step 3: Record the outcome in the spec**

Append a verification-record section to the spec in the round-3 style: the acceptance revision, the machine, the build configuration, the measured on/off series, the verdict against both budget numbers, the artifact's own reading of the run (phase table), and any refuted hypothesis the work produced. If the budget was missed, say so plainly and record the measurement rather than revising the ceiling.

- [ ] **Step 4: Full verification and commit**

The pre-push policy requires a full build and a full suite here, so unlike every other task this one does need the whole tree. **Run the build in the background** — 631 targets takes a long time, and a foreground bounded command will time out. A previous task stalled the session by running it in the foreground.

```bash
cmake --build build
```

Then, once that has exited:

```bash
ctest --test-dir build --output-on-failure
git diff --check
```

The suite takes roughly five minutes (822 cases plus the new ones) and must pass with zero skips. Verify the case count against the expected name set rather than the summary line, because a `GTEST_SKIP` reports as passed. Note that `ctest` is run **serially** here: `-j` races on fixed fixture paths in this repository.

Then commit both documents.

```bash
git commit -m "docs(analyze): record phase-observability overhead and add the reader guide" \
           -m "Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage.** Every numbered spec section maps to a task. Section 4.2/4.3/4.4 (recorder, distributions, counters) → T1. Section 4.5 (sampler and join) → T4. Section 4.6 (span inventory) → T3 and T7. Section 4.7 (store read-back) → T5. Section 5.2 (five identity rules) → T3's test for Rules 1–2, T6 for Rule 3, T1's null-recorder test for Rule 4, and the not-a-budget constraint is a Global Constraint. Sections 6.1–6.5 → T2 and T6. Section 7.1–7.4 → T4 and T6. Section 8.1–8.6 → distributed across every task's test steps, with 8.5 landing in T8. Sections 9.1–9.2 → the task list. Section 10 (the duplicated ingest) is measured by T3's `cli.ingest`/`m1.ingest` spans and reported in T8.

**Gaps found and closed.** §8.3's schema contract needed `llvm::json::Parse`, so `RunReportTest` links `LLVM` directly rather than relying on the library's `PRIVATE` link. §6.3's `"complete"` and `"diagnostics"` needed a producer: T1 records them, T2 emits them, T6 surfaces them on stderr.

**The largest gap found in review, and how it was closed.** Section 4.6's inventory lists a `run` root span and a `cli.ingest` span, and §10's headline finding — that the CLI ingests the project and then the analyzer ingests it again — depends on both. **No task created either one.** T1's promotion logic and T2's fixture merely *handle* a span named `run`; neither opens one, and T3's span list begins at `m1.ingest`. As written, the feature would have shipped with the duplication invisible and the report's total resting on the synthetic-root fallback. T6 now opens `run` before the first ingest, opens `cli.ingest` around the CLI's three calls, and asserts in its CLI test that both `cli.ingest` and `m1.ingest` appear in the artifact, with the real `run` node present rather than the fallback.

**The initial draft's soft spot, and how it was closed.** The first version of T5/T6 named the inventory fields but left their assembly to the implementer, because the counting sites had not been read. That is a placeholder wearing a caveat, so the sites were read and the plan now names real APIs:

- SVFG nodes: `SVFG::getSVFGNodeNum()` (`third_party/SVF/svf/include/Graphs/SVFG.h:271-274`), verified.
- SVFG edges: `GenericGraph::getTotalEdgeNum()` (`third_party/SVF/svf/include/Graphs/GenericGraph.h:428-431`), verified reachable through `SVFG → VFG → GenericGraph`.
- Component kind: `ComponentKindName(WpaComponentKind)` (`include/veritas/wpa/WpaComponent.h:52`); `WpaComponentKey` carries `scc_id` and `component` (`include/veritas/wpa/WpaRunRepository.h:58-63`).
- Store counts: `MetadataStore::Open` and `MetadataStore::Query` (`include/veritas/summarydb/MetadataStore.h:94,128`).
- Test fixture helper: `veritas::testing::FixtureProject(std::string_view)` (`tests/support/ProjectFixture.h:40`).

One architectural consequence fell out of that reading and is now the rule in T6: output-scale counts travel as **recorder counters** added at the producing site, not through a second plumbing path into `ProjectAnalysisResult`. The SVF stage's header is private to `veritas_analysis`, so a plumbed path would have had to widen a struct that existing tests construct. Counters avoid that, and a counter that was never recorded is visibly absent rather than plausibly zero.
