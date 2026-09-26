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

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <utility>

namespace veritas::core {

namespace {

using std::chrono::nanoseconds;

constexpr std::size_t kNoParent = static_cast<std::size_t>(-1);

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
