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

// RunMetrics.cpp — the span recorder. See include/veritas/core/RunMetrics.h.

#include "veritas/core/RunMetrics.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <pthread.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace veritas::core {

namespace {

using std::chrono::nanoseconds;

constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);

nanoseconds ClampNonNegative(nanoseconds value) {
  return value < nanoseconds::zero() ? nanoseconds::zero() : value;
}

// SleepFor blocks the calling thread for the given duration. It uses
// nanosleep rather than std::this_thread::sleep_for so that no path in this
// file reaches a std::thread facility: those signal their failures with an
// exception, which -fno-exceptions leaves no way to handle. A signal
// interrupting the sleep resumes it with the remaining time nanosleep writes
// back.
void SleepFor(std::chrono::milliseconds duration) {
  timespec remaining{};
  remaining.tv_sec = static_cast<time_t>(duration.count() / 1000);
  remaining.tv_nsec = static_cast<long>((duration.count() % 1000) * 1000000);
  while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
  }
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
  // Linux: /proc/self/statm field 2 is the resident set in pages, and the
  // kernel reports its own page size only through sysconf, which is not
  // linked here; 4096 is the page size on every architecture this builds for.
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

SeriesBuffer::SeriesBuffer(std::size_t capacity) : capacity_(capacity) {}

void SeriesBuffer::Append(MemorySample sample) {
  samples_.push_back(sample);
  if (samples_.size() <= capacity_) return;

  // Overflow: keep every other sample, so the whole timeline stays covered at
  // half the resolution instead of the early curve being lost. The stride
  // starts at the oldest sample, which is therefore never discarded.
  std::vector<MemorySample> kept;
  kept.reserve(samples_.size() / 2 + 1);
  for (std::size_t i = 0; i < samples_.size(); i += 2) {
    kept.push_back(samples_[i]);
  }
  // The newest sample survives too: if the stride stepped over it, it replaces
  // the last kept sample, which is the one nearest it in time. The index test
  // is exact, where comparing timestamps would also match a duplicate.
  if ((samples_.size() - 1) % 2 != 0) kept.back() = samples_.back();
  samples_.swap(kept);
  decimation_ *= 2;
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

  // The thread handle and its stop flag only. The samples and the thinning
  // policy live in SeriesBuffer, which is the object the tests drive directly.
  struct Sampler {
    pthread_t thread {};
    std::atomic<bool> stop {false};
    bool started = false;
  };

  RunMetricsOptions options;
  WallClock wall;
  CpuClock cpu;
  Sampler sampler;
  SeriesBuffer series;
  std::vector<Open> open;
  std::vector<Accum> accums;
  // Keyed by (parent accum index, span name) so one node exists per name per
  // parent, and no name has to be parsed out of a dotted path.
  std::map<std::pair<std::size_t, std::string>, std::size_t> by_parent_name;
  std::vector<Counter> counters;
  std::vector<std::string> diagnostics;
  bool complete = true;
  std::chrono::steady_clock::time_point run_start;
  // Set when the sampler could not start: samples are then taken at span
  // boundaries instead of on a timer, and the artifact reports itself
  // incomplete (Diagnose clears `complete`). It is NOT set for
  // sample_interval == 0: a zero interval asks for no sampling at all, so the
  // series stays empty rather than filling with boundary readings.
  bool boundary_only = false;

  Impl(RunMetricsOptions opts, WallClock w, CpuClock c)
      : options(opts),
        wall(std::move(w)),
        cpu(std::move(c)),
        series(opts.series_capacity) {
    // The zero of the series' time axis. Sampled before the sampler starts so
    // that every sample lands at a non-negative offset from it.
    run_start = this->wall();
    // Only a positive interval asks for a sampler thread. The default of zero
    // is deliberate: constructing a recorder must not start a thread.
    if (options.sample_interval <= std::chrono::milliseconds::zero()) return;
    const int rc =
        pthread_create(&sampler.thread, nullptr, &Impl::SamplerMain, this);
    if (rc != 0) {
      // No thread, no timer: fall back to span-boundary sampling, which is
      // sparse but still delimits every bearing span. Diagnose clears
      // `complete`, so the artifact says the series was degraded.
      Diagnose("sampler thread unavailable: pthread_create returned " +
               std::to_string(rc));
      boundary_only = true;
      return;
    }
    sampler.started = true;
  }

  void Diagnose(std::string message) {
    diagnostics.push_back(std::move(message));
    complete = false;
  }

  // SamplerMain is the body of VERITAS's only thread. Everything else in the
  // codebase is single-threaded and relies on that without saying so: while a
  // run is in progress this thread is the sole writer to the series buffer,
  // and the series is read only after StopSampler has joined it. Do not start
  // a second thread without re-reading the design spec, section 7.4.
  static void* SamplerMain(void* arg);

  // Measure now and append the reading at `at`, an instant on the series'
  // time axis. The sampler thread passes a fresh reading of the wall clock;
  // the boundary fallback passes the span boundary it is measuring, so it
  // takes no clock reading of its own.
  void AppendSampleAt(std::chrono::steady_clock::time_point at) {
    const auto offset =
        std::chrono::duration_cast<std::chrono::milliseconds>(at - run_start);
    series.Append(
        MemorySample{offset, CurrentResidentBytes(), CurrentFootprintBytes()});
  }

  // StopSampler stops the sampler and joins it, so that what the buffer holds
  // afterwards is complete and no longer being written. Idempotent, and a
  // no-op when there is no sampler thread — including the boundary_only
  // fallback, where the analysis thread did the appending.
  void StopSampler() {
    if (!sampler.started) return;
    sampler.stop.store(true, std::memory_order_relaxed);
    const int rc = pthread_join(sampler.thread, nullptr);
    sampler.started = false;
    if (rc != 0) {
      // A failed join means the thread may still be appending while the
      // series is read below. Record it rather than report a series that may
      // be torn.
      Diagnose("sampler thread did not join: pthread_join returned " +
               std::to_string(rc));
    }
  }

  // JoinMemory describes one bearing accumulator's interval against the
  // series, with both ends of the window inclusive. It returns nullopt when
  // no measurement exists for this span — no series at all, or a window no
  // sample landed in — so that an absent measurement is visibly absent rather
  // than plausibly zero, and a present block always means "measured".
  //
  // The window is the UNION of every occurrence: for a span that ran more
  // than once the interval is [first_start, last_end], and the figures below
  // describe that union — not any single occurrence, and not a per-occurrence
  // average. rss_start and rss_end are the oldest and newest samples inside
  // the window, and the series is monotonic in t, so they are its first and
  // last in-window entries.
  std::optional<SpanMemory> JoinMemory(const Accum& accum) const {
    if (series.samples().empty()) return std::nullopt;
    SpanMemory memory;
    const auto offset = [this](std::chrono::steady_clock::time_point at) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(at -
                                                                  run_start);
    };
    const auto start = offset(accum.first_start);
    const auto end = offset(accum.last_end);
    bool saw_any = false;
    for (const MemorySample& sample : series.samples()) {
      if (sample.t < start || sample.t > end) continue;
      if (!saw_any) {
        memory.rss_start = sample.rss_bytes;
        saw_any = true;
      }
      memory.rss_end = sample.rss_bytes;
      memory.peak_within = std::max(memory.peak_within, sample.rss_bytes);
      memory.footprint_peak =
          std::max(memory.footprint_peak, sample.footprint_bytes);
    }
    if (!saw_any) return std::nullopt;
    // Signed: a phase may release more than it allocates.
    memory.rss_delta = static_cast<std::int64_t>(memory.rss_end) -
                       static_cast<std::int64_t>(memory.rss_start);
    return memory;
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
};

// The sampler's whole loop: sleep one interval, measure, append. The buffer's
// own policy handles overflow, so there is no cursor, no capacity test and no
// decimation arithmetic here. std::this_thread::sleep_for is not used, and
// std::thread not at all: those signal failure with an exception, and this
// build cannot handle one.
void* RunMetrics::Impl::SamplerMain(void* arg) {
  auto* self = static_cast<Impl*>(arg);
  const auto interval = self->options.sample_interval;
  const auto slice = std::min(interval, std::chrono::milliseconds(20));
  while (!self->sampler.stop.load(std::memory_order_relaxed)) {
    // Sleep in bounded slices so that StopSampler, which waits at teardown,
    // does not have to wait out a whole interval.
    for (auto slept = std::chrono::milliseconds::zero();
         slept < interval &&
         !self->sampler.stop.load(std::memory_order_relaxed);
         slept += slice) {
      SleepFor(slice);
    }
    if (self->sampler.stop.load(std::memory_order_relaxed)) break;
    self->AppendSampleAt(self->wall());
  }
  return nullptr;
}

RunMetrics::RunMetrics(RunMetricsOptions options, WallClock wall, CpuClock cpu)
    : impl_(std::make_unique<Impl>(options, std::move(wall), std::move(cpu))) {}

RunMetrics::~RunMetrics() {
  // Join before the Impl is destroyed, so the thread never outlives the
  // buffer it writes into. TakeStats has usually done this already.
  impl_->StopSampler();
}

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
  const std::size_t token = impl_->open.size() - 1;
  // Fallback sampling, and only for a span that keeps an interval of its own:
  // a kPlain span nested in a bearing one adds no sample, so the series stays
  // one sample per phase rather than one per component. The reading is
  // stamped with this span's own start, which is the boundary it measures and
  // costs no clock read of its own.
  if (impl_->boundary_only && mode == SpanMode::kBearing) {
    impl_->AppendSampleAt(impl_->open.back().start_wall);
  }
  return token;
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

  Impl::Accum& accum = impl_->accums[span.accum_index];
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
    impl_->accums[accum.parent_index].children_inclusive += wall;
  }

  // The closing half of the fallback's boundary pair, stamped with the end of
  // the span just closed. It therefore lands exactly on the window's last
  // millisecond, which is inside it.
  if (impl_->boundary_only && span.mode == SpanMode::kBearing) {
    impl_->AppendSampleAt(end_wall);
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

SeriesBuffer& RunMetrics::series() { return impl_->series; }

RunMetricsStats RunMetrics::TakeStats() {
  // The series is read below and the join is done below that, so the sampler
  // must be stopped and joined first: waiting for the destructor would mean
  // reading the buffer while the thread is still appending to it.
  impl_->StopSampler();

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
  //
  // Every index below is an element of accums or of child_indices, so the
  // subscripts are in range by construction.
  std::vector<std::vector<std::size_t>> child_indices(impl_->accums.size());
  std::vector<std::size_t> roots;
  for (std::size_t i = 0; i < impl_->accums.size(); ++i) {
    const std::size_t parent = impl_->accums[i].parent_index;
    if (parent == kNoParent) {
      roots.push_back(i);
    } else {
      child_indices[parent].push_back(i);
    }
  }
  const auto by_name = [this](std::size_t left, std::size_t right) {
    return impl_->accums[left].name < impl_->accums[right].name;
  };

  std::function<SpanStats(std::size_t)> build = [&](std::size_t index) {
    const Impl::Accum& accum = impl_->accums[index];
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
    if (accum.mode == SpanMode::kBearing) {
      // Bearings keep an interval, so these are the spans the series can be
      // joined to. A bearing span no sample landed in gets no block at all,
      // rather than one full of zeroes that reads as a measured zero.
      out.memory = impl_->JoinMemory(accum);
    }
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
    std::vector<std::size_t> children = child_indices[index];
    std::sort(children.begin(), children.end(), by_name);
    for (const std::size_t child : children) {
      out.children.push_back(build(child));
    }
    return out;
  };

  if (roots.size() == 1 && impl_->accums[roots.front()].name == "run") {
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

  // The run-level peak is a property of the whole run rather than of any one
  // span: the maximum over the series, carrying the time it occurred at.
  // Ties keep the earliest occurrence. The footprint peak can fall at a
  // different sample, so it is tracked on its own and not given a time.
  for (const MemorySample& sample : impl_->series.samples()) {
    if (sample.rss_bytes > stats.peak_rss_bytes) {
      stats.peak_rss_bytes = sample.rss_bytes;
      stats.peak_at = sample.t;
    }
    stats.peak_footprint_bytes =
        std::max(stats.peak_footprint_bytes, sample.footprint_bytes);
  }
  stats.series_decimation = impl_->series.decimation();
  if (impl_->options.emit_series) {
    // SeriesBuffer exposes a const view, so this copies. It is one memcpy of
    // at most the buffer's capacity, at teardown, after every measurement has
    // been taken, and the recorder is consumed here.
    stats.series = impl_->series.samples();
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
