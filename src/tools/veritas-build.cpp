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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/RunMetrics.h"
#include "veritas/core/Status.h"
#include "veritas/core/Version.h"
#include "veritas/observability/RunReport.h"
#include "veritas/observability/StoreSummary.h"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-build --version\n"
    "  veritas-build analyze --project <directory> [--output <directory>]\n"
    "      [--wpa-engine souffle|cpp-emergency]\n"
    "      [--field-sensitive true|false] [--max-alias-pairs <n>]\n"
    "      [--metrics true|false] [--metrics-interval-ms <n>]\n"
    "      [--metrics-top-n <k>] [--metrics-series true|false]\n"
    "      [--metrics-path <file>]\n"
    "\n"
    "`analyze` is the only source-input command. No `--compile-db`,\n"
    "`--manifest`, `--bitcode`, `--llvm-module`, or `--svf-input` alternative\n"
    "is accepted; the project directory is the sole public source-input.\n";

struct AnalyzeArguments {
  fs::path project;
  fs::path output;
  std::string wpa_engine = "souffle";
  bool field_sensitive = true;
  std::size_t max_alias_pairs = 0;  // 0 = keep the AnalysisConfig default
  bool metrics = true;
  std::size_t metrics_interval_ms = 250;
  std::size_t metrics_top_n = 10;
  bool metrics_series = true;
  fs::path metrics_path;  // empty = <output>/run-metrics.json
};

// Parse a positive decimal integer without exceptions (the project builds with
// -fno-exceptions). Returns false on empty, non-digit, zero, or overflow input.
bool ParsePositiveSize(std::string_view value, std::size_t* out) {
  if (value.empty() || out == nullptr) return false;
  std::size_t result = 0;
  for (char c : value) {
    if (c < '0' || c > '9') return false;
    const std::size_t digit = static_cast<std::size_t>(c - '0');
    if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  if (result == 0) return false;
  *out = result;
  return true;
}

// Parse a decimal integer that may be zero. Returns false on empty, non-digit,
// overflow, or a value above `limit`. Zero is a real value here rather than a
// rejected one: it is how `--metrics-interval-ms` asks for no sampler thread
// and no series at all, which ParsePositiveSize rejects by construction.
bool ParseUnsigned(std::string_view value, std::size_t limit,
                   std::size_t* out) {
  if (value.empty() || out == nullptr) return false;
  std::size_t result = 0;
  for (char c : value) {
    if (c < '0' || c > '9') return false;
    const std::size_t digit = static_cast<std::size_t>(c - '0');
    if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  if (result > limit) return false;
  *out = result;
  return true;
}

// A sampling interval has to be representable as std::chrono::milliseconds. A
// larger value would wrap to a negative duration, which the recorder reads as
// "no interval" — a silently different run from the one that was asked for, so
// the flag rejects it instead.
constexpr std::size_t kMaxMetricsIntervalMs =
    static_cast<std::size_t>(std::chrono::milliseconds::max().count());

veritas::StatusOr<AnalyzeArguments> ParseAnalyzeArguments(
    const std::vector<std::string>& args) {
  static constexpr std::string_view kRejectedFlags[] = {
      "--compile-db", "--manifest", "--bitcode", "--llvm-module", "--svf-input"};

  AnalyzeArguments parsed;
  bool project_seen = false;
  bool output_seen = false;

  auto rejection_error = [](std::string_view flag) {
    return veritas::Status::InvalidArgument(
        std::string("veritas-build analyze does not accept ") +
        std::string(flag) +
        "; the project directory is the only source-input abstraction.");
  };
  auto is_rejected = [](std::string_view value) -> std::string_view {
    for (const auto rejected : kRejectedFlags) {
      if (value == rejected) return rejected;
    }
    return {};
  };

  // Consume the next argument as an option value. We validate that it exists
  // and that it is not itself a rejected artifact flag — otherwise
  // `veritas-build analyze --project --compile-db path` would silently take
  // `--compile-db` as the project path and bypass the rejection contract.
  auto take_value = [&](std::size_t& i, std::string_view option)
      -> veritas::StatusOr<std::string> {
    if (i + 1 >= args.size()) {
      return veritas::Status::InvalidArgument(
          std::string(option) + " requires a value argument");
    }
    const auto& value = args[i + 1];
    if (const auto rejected = is_rejected(value); !rejected.empty()) {
      return rejection_error(rejected);
    }
    if (!value.empty() && value.front() == '-') {
      return veritas::Status::InvalidArgument(
          std::string(option) + " requires a value, got flag: " + value);
    }
    ++i;
    return value;
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (const auto rejected = is_rejected(arg); !rejected.empty()) {
      return rejection_error(rejected);
    }
    if (arg == "--project") {
      if (project_seen) {
        return veritas::Status::InvalidArgument(
            "--project must be provided exactly once");
      }
      auto value = take_value(i, "--project");
      if (!value.ok()) return value.status();
      parsed.project = *value;
      project_seen = true;
    } else if (arg == "--output") {
      if (output_seen) {
        return veritas::Status::InvalidArgument(
            "--output must be provided at most once");
      }
      auto value = take_value(i, "--output");
      if (!value.ok()) return value.status();
      parsed.output = *value;
      output_seen = true;
    } else if (arg == "--wpa-engine") {
      auto value = take_value(i, "--wpa-engine");
      if (!value.ok()) return value.status();
      if (*value != "souffle" && *value != "cpp-emergency") {
        return veritas::Status::InvalidArgument(
            "--wpa-engine must be souffle or cpp-emergency");
      }
      parsed.wpa_engine = *value;
    } else if (arg == "--field-sensitive") {
      auto value = take_value(i, "--field-sensitive");
      if (!value.ok()) return value.status();
      if (*value == "true") {
        parsed.field_sensitive = true;
      } else if (*value == "false") {
        parsed.field_sensitive = false;
      } else {
        return veritas::Status::InvalidArgument(
            "--field-sensitive must be true or false");
      }
    } else if (arg == "--max-alias-pairs") {
      auto value = take_value(i, "--max-alias-pairs");
      if (!value.ok()) return value.status();
      if (!ParsePositiveSize(*value, &parsed.max_alias_pairs)) {
        return veritas::Status::InvalidArgument(
            "--max-alias-pairs requires a positive integer, got: " + *value);
      }
    } else if (arg == "--metrics") {
      auto value = take_value(i, "--metrics");
      if (!value.ok()) return value.status();
      if (*value == "true") {
        parsed.metrics = true;
      } else if (*value == "false") {
        parsed.metrics = false;
      } else {
        return veritas::Status::InvalidArgument(
            "--metrics must be true or false");
      }
    } else if (arg == "--metrics-interval-ms") {
      auto value = take_value(i, "--metrics-interval-ms");
      if (!value.ok()) return value.status();
      if (!ParseUnsigned(*value, kMaxMetricsIntervalMs,
                         &parsed.metrics_interval_ms)) {
        return veritas::Status::InvalidArgument(
            "--metrics-interval-ms requires a non-negative integer, got: " +
            *value);
      }
    } else if (arg == "--metrics-top-n") {
      auto value = take_value(i, "--metrics-top-n");
      if (!value.ok()) return value.status();
      if (!ParsePositiveSize(*value, &parsed.metrics_top_n)) {
        return veritas::Status::InvalidArgument(
            "--metrics-top-n requires a positive integer, got: " + *value);
      }
    } else if (arg == "--metrics-series") {
      auto value = take_value(i, "--metrics-series");
      if (!value.ok()) return value.status();
      if (*value == "true") {
        parsed.metrics_series = true;
      } else if (*value == "false") {
        parsed.metrics_series = false;
      } else {
        return veritas::Status::InvalidArgument(
            "--metrics-series must be true or false");
      }
    } else if (arg == "--metrics-path") {
      auto value = take_value(i, "--metrics-path");
      if (!value.ok()) return value.status();
      parsed.metrics_path = *value;
    } else {
      return veritas::Status::InvalidArgument("unknown argument: " + arg);
    }
  }

  if (!project_seen) {
    return veritas::Status::InvalidArgument(
        "--project is required for `veritas-build analyze`");
  }
  return parsed;
}

int ReportStatus(const veritas::Status& status) {
  std::cerr << "veritas-build: " << status.message() << '\n';
  return 1;
}

veritas::Status WriteDiagnosticManifest(
    const fs::path& output_root,
    const veritas::build::AnalysisManifest& manifest) {
  std::error_code error;
  fs::create_directories(output_root, error);
  if (error) {
    return veritas::Status::Internal("cannot create output directory " +
                                     output_root.string() + ": " +
                                     error.message());
  }
  const auto path = output_root / "manifest.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return veritas::Status::Internal("cannot write manifest: " + path.string());
  }
  out << veritas::build::ToDiagnosticJson(manifest);
  return veritas::Status::Ok();
}

// Every metrics line on stderr carries one of these prefixes, so a reader can
// tell a not-recorded field from a failure — and an analysis failure from both.
// A degradation is something that failed and cleared `complete`; a note is a
// field whose producer did not run, which leaves `complete` as the recorder
// found it. Ten notes on every healthy run would otherwise wear out the words
// that have to mean something on the run where the write really did fail.
constexpr std::string_view kMetricsDegraded =
    "veritas-build: metrics degraded: ";
constexpr std::string_view kMetricsNote = "veritas-build: metrics note: ";

// WriteRunMetrics writes the rendered artifact. A path that cannot be written
// is reported rather than swallowed, and a partially written file is removed:
// a truncated artifact is a smaller measurement presented as a whole one, which
// is the reading this whole feature exists to prevent.
veritas::Status WriteRunMetrics(const fs::path& path, std::string_view json) {
  std::ofstream out(path, std::ios::trunc);
  const bool opened = static_cast<bool>(out);
  if (opened) {
    out << json;
    out.flush();
  }
  const bool failed = !out;
  out.close();
  if (!failed && out) return veritas::Status::Ok();
  if (opened) {
    // Only a file this call created is removed. A path that never opened may be
    // a directory, and fs::remove deletes an empty one — destruction the caller
    // never asked for.
    std::error_code ignored;
    fs::remove(path, ignored);
  }
  return veritas::Status::Internal("cannot write " + path.string());
}

// ReportBuilder assembles the report from counters whose producers may not have
// run yet. Every read of an absent counter leaves its report field at zero AND
// records a diagnostic naming it, so an uninstrumented zero is never mistaken
// for a measured one.
class ReportBuilder {
 public:
  ReportBuilder(const std::vector<veritas::core::Counter>& counters,
                veritas::observability::RunReport* report)
      : report_(report) {
    for (const veritas::core::Counter& counter : counters) {
      values_[counter.name] = counter.value;
    }
  }

  // Has reports whether a producer recorded this counter at all.
  bool Has(std::string_view name) const {
    return values_.find(std::string(name)) != values_.end();
  }

  // Counter returns the recorded value, or zero with a diagnostic when no
  // producer recorded it.
  std::uint64_t Counter(std::string_view name) {
    const auto it = values_.find(std::string(name));
    if (it != values_.end()) return it->second;
    Note("counter " + std::string(name) +
         " was not recorded; the report field it fills is zero rather than a "
         "measurement");
    return 0;
  }

  // Values exposes the counter table, for the per-kind scan that has no single
  // name to look up.
  const std::map<std::string, std::uint64_t>& Values() const {
    return values_;
  }

  // Note records a diagnostic that does not mean a whole part of the report is
  // missing: an absent producer's zero, or a contradiction between two numbers
  // the artifact carries. `complete` stays the recorder's own verdict here. The
  // index is kept so the caller can prefix it as a note rather than a failure.
  void Note(std::string message) {
    note_indices_.push_back(report_->metrics.diagnostics.size());
    report_->metrics.diagnostics.push_back(std::move(message));
  }

  // Degrade records a diagnostic for a failure that DOES leave part of the
  // report missing and clears `complete`, which is the artifact-level half of
  // design section 7.1's "loud and self-describing".
  void Degrade(std::string message) {
    report_->metrics.complete = false;
    report_->metrics.diagnostics.push_back(std::move(message));
  }

  // note_indices lists, in ascending order, the diagnostics that are notes.
  // Every other entry is a degradation: the recorder's own diagnostics all are
  // (each one cleared `complete`), and so is anything recorded by Degrade.
  const std::vector<std::size_t>& note_indices() const {
    return note_indices_;
  }

 private:
  std::map<std::string, std::uint64_t> values_;
  std::vector<std::size_t> note_indices_;
  veritas::observability::RunReport* report_;
};

// FillReport populates the report from the analysis outcome, the manifest, the
// recorder's counters, and the post-publication store. Every value comes from
// one of the sources the plan names; nothing is derived from a second plumbing
// path, and no absolute path reaches the report — a status message that might
// carry one is described in the report's own words instead of quoted.
//
// Returns the indices, in ascending order, of the diagnostics that are notes
// rather than degradations, so the caller can prefix the two differently on
// stderr. Both kinds are entries in the artifact's `diagnostics`.
std::vector<std::size_t> FillReport(
    const veritas::analysis::ProjectAnalysisResult& result,
    const veritas::build::AnalysisManifest& manifest,
    const veritas::analysis::AnalysisConfig& config,
    const fs::path& output_root,
    veritas::observability::RunReport* report) {
  // The identity block is separated so a comparison script can exclude its
  // run-scoped coordinates without parsing the rest — SELECTIVELY, not
  // wholesale: only run_id and batch_id move between two runs of one fixture,
  // while the two configuration hashes, the toolchain identity, projection id
  // and the revision coordinates are content- and config-derived and stable.
  // Excluding the whole block would discard exactly the comparison it exists to
  // enable. Every coordinate comes from the run itself: four from the analysis
  // outcome, the repository from the manifest, and the two configuration hashes,
  // the toolchain identity and the batch id from the fields
  // ProjectAnalysisResult carries for exactly this purpose. Without them the
  // artifact would record no configuration at all — nothing would separate a
  // cpp-emergency, non-field-sensitive, or alias-limited run from the default —
  // and an unset coordinate written as "" would diff as unchanged, which is the
  // failure this block exists to prevent.
  report->identity.run_id = result.wpa_run_id;
  report->identity.projection_id = result.projection_id;
  report->identity.revision_id = result.revision_id;
  report->identity.build_variant_id = result.build_variant_id;
  report->identity.repository_id = manifest.context.repository_id;
  report->identity.svf_config_hash = result.svf_configuration_hash;
  report->identity.wpa_config_hash = result.wpa_configuration_hash;
  report->identity.engine_toolchain_identity = result.engine_toolchain_identity;
  report->identity.batch_id = result.batch_id;

  // The one analysis knob no configuration hash covers: it changes what the run
  // does by executing a second full WPA whose canonical results must agree, and
  // neither svf_config_hash nor wpa_config_hash moves for it (design 6.3).
  report->conformance_oracle = config.run_cpp_conformance_oracle;

  veritas::observability::FillEnvironment(&report->environment);
  veritas::observability::FillInventoryFromManifest(manifest,
                                                    &report->inventory.input);

  ReportBuilder builder(report->metrics.counters, report);

  // An identity coordinate with no value has its key omitted (RunReport), and
  // is named here: an absent key is visibly absent, but only to a reader who
  // knows the schema expects it.
  const auto note_if_empty = [&builder](std::string_view field,
                                        const std::string& value) {
    if (value.empty()) {
      builder.Note("identity field " + std::string(field) +
                   " has no value; its key is omitted from the artifact");
    }
  };
  note_if_empty("batch_id", report->identity.batch_id);
  note_if_empty("engine_toolchain_identity",
                report->identity.engine_toolchain_identity);
  note_if_empty("svf_config_hash", report->identity.svf_config_hash);
  note_if_empty("wpa_config_hash", report->identity.wpa_config_hash);
  veritas::observability::RunOutputInventory& output = report->inventory.output;

  output.summaries_published = result.published_summary_ids.size();
  // Histogram over the unknowns, keyed by reason. std::map iterates in key
  // order, so the vector comes out sorted by key, which is what the artifact's
  // diffability wants.
  std::map<std::string, std::uint64_t> unknowns_by_reason;
  for (const veritas::analysis::UnknownFact& unknown : result.unknowns) {
    ++unknowns_by_reason[unknown.reason];
  }
  for (const auto& entry : unknowns_by_reason) {
    output.unknowns_by_reason.emplace_back(entry.first, entry.second);
  }
  output.cpg_nodes = result.cpg_node_count;
  output.cpg_edges = result.cpg_edge_count;

  // Output-scale counts come from the recorder's counters, never from a second
  // plumbing path: the producing site is where the number is known.
  output.svfg_nodes = builder.Counter("svf.svfg_nodes");
  output.rooted_input_facts = builder.Counter("facts.rooted_input");
  output.canonical_facts = builder.Counter("facts.canonical");
  veritas::observability::RunIncrementality& incrementality =
      report->inventory.incrementality;
  incrementality.components_reused = builder.Counter("wpa.components.reused");
  incrementality.components_executed =
      builder.Counter("wpa.components.executed");
  // These three have no producer anywhere in the pipeline, so there is no
  // counter to read them back from and no value to write. They are left unset,
  // and the renderer omits their keys: a present key means "measured", so an
  // unproduced count must be absent rather than 0, which would diff as
  // *unchanged* against a second run that also never measured it (design
  // section 6.3). svfg_edges is the sharpest case: a producer looked like it
  // existed, but the SVF accessor it read returns a field nothing increments for
  // an SVFG. The notes below name each one, since an absent key is visibly
  // absent only to a reader who knows the schema expects it.
  builder.Note(
      "inventory.incrementality.summaries_recomputed has no producer; its key "
      "is omitted from the artifact");
  builder.Note(
      "inventory.incrementality.summaries_reused has no producer; its key is "
      "omitted from the artifact");
  builder.Note(
      "inventory.output.svfg_edges has no producer: the SVF SVFG edge counter "
      "is never incremented in the pinned revision, so its key is omitted from "
      "the artifact");

  // Per-kind expected counts, one counter per kind, named
  // `wpa.component.<kind-name>.expected`.
  static constexpr std::string_view kKindPrefix = "wpa.component.";
  static constexpr std::string_view kKindSuffix = ".expected";
  for (const auto& entry : builder.Values()) {
    const std::string& name = entry.first;
    if (name.size() <= kKindPrefix.size() + kKindSuffix.size()) continue;
    if (!name.starts_with(kKindPrefix)) continue;
    if (!name.ends_with(kKindSuffix)) continue;
    output.components_by_kind.emplace_back(
        name.substr(kKindPrefix.size(),
                    name.size() - kKindPrefix.size() - kKindSuffix.size()),
        entry.second);
  }
  if (output.components_by_kind.empty()) {
    builder.Note(
        "no wpa.component.<kind>.expected counters were recorded; "
        "components_by_kind is empty rather than a measured zero");
  }

  // The expected-component count is carried twice: the text report sums
  // components_by_kind, and the cross-check below reads the separate
  // wpa.components.expected counter. Both derive from the same frozen expected
  // set, so they agree; if they ever diverge, one artifact would hold two
  // numbers each claiming to be "expected components" with nothing saying which
  // is right. This check is what makes that contradiction loud. It compares
  // only when both numbers were measured: with either one absent, the absence
  // is what the diagnostics above already name.
  const std::uint64_t expected_components =
      builder.Counter("wpa.components.expected");
  if (builder.Has("wpa.components.expected") &&
      !output.components_by_kind.empty()) {
    std::uint64_t summed_components = 0;
    for (const auto& kind : output.components_by_kind) {
      summed_components += kind.second;
    }
    if (summed_components != expected_components) {
      // A degradation, not a note: two figures in this artifact both claim to
      // be the expected component count and disagree, which makes the artifact
      // internally inconsistent rather than merely under-populated.
      builder.Degrade("components_by_kind sums to " +
                      std::to_string(summed_components) +
                      " but wpa.components.expected reads " +
                      std::to_string(expected_components) +
                      "; the artifact carries two expected-component totals");
    }
  }

  auto store = veritas::observability::CollectStoreSummary(output_root);
  if (!store.ok()) {
    // The status message can name the output root, and rule 4 of design section
    // 6.5 bars any absolute path from the artifact, so the failure is described
    // here rather than quoted.
    builder.Degrade(
        store.status().code() == veritas::StatusCode::kNotFound
            ? "the store block is omitted: no metadata.db under the output root"
            : "the store block is omitted: the store read-back failed");
    return builder.note_indices();
  }
  report->store = std::move(*store);

  // The cross-check pairs the expected-component count the run held in memory
  // against the row count of the component-result table, read back from the
  // store. It is a cross-check only when both sides were measured: with the
  // counter absent there is nothing to compare, and a 0-versus-N "MISMATCH"
  // would be an uninstrumented zero dressed up as a disagreement.
  if (!builder.Has("wpa.components.expected")) return builder.note_indices();
  for (const veritas::observability::TableRowCount& table :
       report->store.tables) {
    // `_v2` deliberately. `wpa_component_states` (schema v1) is the SCC
    // convergence state and holds a different row count, so "correcting" this
    // name would silently make the cross-check meaningless while still passing.
    if (table.table != "wpa_component_states_v2") continue;
    report->store.cross_checks.push_back(veritas::observability::CrossCheck{
        "components", table.rows, expected_components,
        table.rows == expected_components});
    return builder.note_indices();
  }
  builder.Note(
      "the component cross-check is omitted: wpa_component_states_v2 was not "
      "counted in the store read-back");
  return builder.note_indices();
}

veritas::Status Analyze(const std::vector<std::string>& args) {
  auto parsed = ParseAnalyzeArguments(args);
  if (!parsed.ok()) return parsed.status();

  const veritas::analysis::ProjectAnalysisRequest request{
      .project_root = parsed->project,
      .output_root = parsed->output,
  };

  // Hoisted out of the span scopes below so the print block and the report can
  // still read them once the spans have closed.
  veritas::build::ProjectInput input;
  veritas::build::AnalysisManifest manifest;
  veritas::analysis::ProjectAnalysisResult result{};

  // The configuration is likewise built outside the span scopes, because the
  // report reads one of its knobs after the run span has closed.
  veritas::analysis::AnalysisConfig config =
      veritas::analysis::AnalysisConfig::Default();
  config.wpa_engine = parsed->wpa_engine == "cpp-emergency"
                          ? veritas::analysis::WpaEngineMode::kCppEmergency
                          : veritas::analysis::WpaEngineMode::kSouffle;
  config.svf_field_sensitive = parsed->field_sensitive;
  if (parsed->max_alias_pairs != 0) {
    config.svf_max_alias_pairs = parsed->max_alias_pairs;
  }

  // The recorder is built before any ingest, and the "run" root opens
  // immediately after it, so that root encloses the whole command — including
  // the CLI's own ingest — and TakeStats promotes it rather than wrapping a
  // synthetic root around it. Constructed in place: RunMetrics is neither
  // copyable nor movable. With `--metrics false` the interval stays at its zero
  // default, so no sampler thread is created and every span guard is an inert
  // pointer test.
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

  {
    veritas::core::PhaseSpan run_span(recorder, "run",
                                      veritas::core::SpanMode::kBearing);

    // The CLI's own ingest: resolve the project, load the manifest, write the
    // diagnostic manifest. The analyzer performs the same two steps again under
    // `m1.ingest`, so this pair is the measurable form of that duplication.
    {
      veritas::core::PhaseSpan ingest_span(
          recorder, "cli.ingest", veritas::core::SpanMode::kBearing);
      auto resolved = veritas::build::ResolveProjectInput(request);
      if (!resolved.ok()) return resolved.status();
      input = *resolved;
      auto loaded = veritas::build::LoadProjectManifest(input);
      if (!loaded.ok()) return loaded.status();
      manifest = *loaded;
      if (auto status = WriteDiagnosticManifest(input.output_root, manifest);
          !status.ok()) {
        return status;
      }
    }

    std::cout << "Project: " << input.project_root.string() << '\n'
              << "Repository: " << manifest.context.repository_id << '\n'
              << "Revision: " << manifest.context.revision_id << '\n'
              << "Build Variant: " << manifest.context.build_variant_id << '\n'
              << "Translation Units: " << manifest.translation_units.size()
              << '\n'
              << "Diagnostic Manifest: "
              << (input.output_root / "manifest.json").string() << '\n';

    veritas::analysis::ProjectAnalyzer analyzer;
    auto analyzed = analyzer.AnalyzeProject(request, config, recorder);
    if (!analyzed.ok()) return analyzed.status();
    result = std::move(*analyzed);

    std::cout << "Analysis complete\n"
              << "Published summaries: "
              << result.published_summary_ids.size() << '\n'
              << "CPG projection: " << result.projection_id << '\n'
              << "CPG nodes: " << result.cpg_node_count << '\n'
              << "CPG edges: " << result.cpg_edge_count << '\n'
              << "WPA engine: "
              << (result.wpa_engine ==
                          veritas::analysis::WpaEngineMode::kSouffle
                      ? "souffle"
                      : "cpp-emergency")
              << '\n'
              << "WPA run: " << result.wpa_run_id << '\n'
              << "Unknowns: " << result.unknowns.size() << '\n';
  }
  // The "run" span has closed, and that ordering is load-bearing: TakeStats
  // folds the accumulator named "run", so a root still open at this point would
  // carry no accumulated wall time and the report's total row — the number the
  // whole report is anchored to — would read zero instead of the command's wall
  // time. Only the report's own rendering and write fall outside the run.

  if (!parsed->metrics) return veritas::Status::Ok();

  veritas::observability::RunReport report;
  report.metrics = metrics.TakeStats();
  report.metrics_options = metrics_options;
  const std::vector<std::size_t> note_indices =
      FillReport(result, manifest, config, input.output_root, &report);

  std::cout << veritas::observability::RenderRunReportText(report);

  const fs::path artifact = parsed->metrics_path.empty()
                                ? input.output_root / "run-metrics.json"
                                : parsed->metrics_path;
  const auto written = WriteRunMetrics(
      artifact, veritas::observability::RenderRunReportJson(report));
  if (!written.ok()) {
    // Metrics never fail the analysis. By this point the summaries, facts, CPG
    // and diagnostic manifest are durably committed, so failing the command
    // would report completed work as failed; the stderr line below and
    // `complete: false` are the honest signals instead (spec section 7.1). The
    // artifact was rendered before this and is not rendered again, so the path
    // in the message never reaches an artifact.
    report.metrics.complete = false;
    report.metrics.diagnostics.push_back(
        "the run-metrics artifact was not written: " +
        std::string(written.message()));
  }

  // Notes and degradations share the artifact's diagnostics array and are told
  // apart here on stderr, where the operator needs to know whether a run failed
  // or merely has a field its producer has not filled in yet.
  for (std::size_t i = 0; i < report.metrics.diagnostics.size(); ++i) {
    const bool is_note =
        std::binary_search(note_indices.begin(), note_indices.end(), i);
    std::cerr << (is_note ? kMetricsNote : kMetricsDegraded)
              << report.metrics.diagnostics[i] << '\n';
  }
  return veritas::Status::Ok();
}

int RunAnalyze(const std::vector<std::string>& args) {
  const auto status = Analyze(args);
  return status.ok() ? 0 : ReportStatus(status);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << veritas::FormatVersion(veritas::GetVersion()) << '\n';
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "analyze") == 0) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
    return RunAnalyze(args);
  }
  std::cerr << kUsage;
  return 1;
}
