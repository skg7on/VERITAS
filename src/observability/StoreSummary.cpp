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

#include "veritas/observability/StoreSummary.h"

#include <sys/utsname.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include "veritas/core/Status.h"
#include "veritas/core/Version.h"
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
    if (!it->is_regular_file(error)) continue;
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

}  // namespace veritas::observability
