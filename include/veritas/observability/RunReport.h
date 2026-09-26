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

// RunReport.h — the artifact half of `veritas-build analyze` phase
// observability.
//
// A RunReport is a plain aggregate: producers fill it in, and the two
// renderers in this header turn it into the versioned JSON artifact and the
// human stdout report. It deliberately includes nothing from LLVM, so a
// consumer that only fills the report needs no LLVM headers; the JSON writer
// that does need them lives in src/observability/RunReport.cpp.

#ifndef VERITAS_OBSERVABILITY_RUNREPORT_H_
#define VERITAS_OBSERVABILITY_RUNREPORT_H_

#include <chrono>
#include <cstdint>
#include <optional>
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
  // No producer can fill this, so it stays unset and its key is omitted from
  // the artifact rather than emitted as 0 — an unproduced count must not diff
  // as "unchanged". See RunIncrementality and design section 6.3.
  std::optional<std::uint64_t> svfg_edges;
  std::vector<std::pair<std::string, std::uint64_t>> components_by_kind;
  std::uint64_t rooted_input_facts = 0;
  std::uint64_t canonical_facts = 0;
};

struct RunIncrementality {
  std::uint64_t components_reused = 0;
  std::uint64_t components_executed = 0;
  // Both unset for the same reason as svfg_edges above: no producer anywhere in
  // the pipeline, so the key is absent rather than a plausibly-zero value. The
  // tri-state is deliberate — present means measured.
  std::optional<std::uint64_t> summaries_recomputed;
  std::optional<std::uint64_t> summaries_reused;
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
  // The one analysis-config knob no configuration hash covers. It changes what
  // the run does — a second full WPA whose canonical results must agree — so a
  // reader comparing two artifacts needs it, and cannot recover it from
  // svf_config_hash or wpa_config_hash. Every other AnalysisConfig field IS
  // covered by one of those two hashes; see design section 6.3.
  bool conformance_oracle = false;
  core::RunMetricsStats metrics;
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
