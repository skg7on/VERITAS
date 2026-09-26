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

// RunReport.cpp — the JSON artifact writer and the stdout report writer.
//
// Both renderers are pure functions of the RunReport they are given: the CLI
// renders one report twice, once to the artifact and once to stdout, so neither
// may mutate its argument. Every name-keyed list is sorted on a copy here
// rather than trusted to the producer, because the artifact's diffability is a
// property of the renderer (design spec, section 6.5).
//
// Attribute order. `llvm::json::OStream` emits attributes in call order and
// sorts nothing, so the sorted-key rule is produced explicitly: within every
// object, attributes are written in lexicographic key order. The conditional
// keys (`cpu_inclusive_ns`, `distribution`, `top_n`, `memory`) appear only when
// the producer set the corresponding field, so an absent measurement is visibly
// absent rather than plausibly zero.

#include "veritas/observability/RunReport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace veritas::observability {
namespace {

constexpr std::uint64_t kBytesPerGiB = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t kBytesPerMiB = 1024ull * 1024ull;
constexpr std::uint64_t kBytesPerKiB = 1024ull;
constexpr double kNanosPerSecond = 1000000000.0;
constexpr double kNanosPerMilli = 1000000.0;

// SortedCopy returns a copy of `items` ordered by a key projection, so the
// renderer owns the artifact's stability instead of trusting each producer to
// have sorted. stable_sort keeps a repeated key in its input order, which makes
// the result deterministic even for a duplicate name.
template <typename T, typename Key>
std::vector<T> SortedCopy(const std::vector<T>& items, Key key) {
  std::vector<T> copy = items;
  std::stable_sort(copy.begin(), copy.end(),
                   [&key](const T& left, const T& right) {
                     return key(left) < key(right);
                   });
  return copy;
}

// PadLeft right-aligns `text` in a column `width` characters wide. Text wider
// than its column is emitted unchanged rather than truncated: a clipped number
// is a wrong number.
std::string PadLeft(std::string_view text, std::size_t width) {
  std::string padded;
  if (text.size() < width) padded.assign(width - text.size(), ' ');
  padded.append(text);
  return padded;
}

// DisplayWidth counts the columns a label occupies on a terminal rather than
// its bytes. The tree's box-drawing characters ("│", "├", "└", "─") are three
// bytes and one column each, so a label column measured in bytes would be two
// columns narrower at every level of nesting and the numbers would drift left
// as the tree deepens.
std::size_t DisplayWidth(std::string_view text) {
  std::size_t columns = 0;
  for (const char c : text) {
    // 0b10xxxxxx is a UTF-8 continuation byte; every other byte opens a
    // character, and every character a label can hold is one column wide.
    if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++columns;
  }
  return columns;
}

// PadRight is PadLeft's mirror, for the label column: the tree's connectors
// must sit at their own indent, so the name is padded on the right, to the
// column count the widest label needs.
std::string PadRight(std::string_view text, std::size_t width) {
  std::string padded(text);
  const std::size_t columns = DisplayWidth(text);
  if (columns < width) padded.append(width - columns, ' ');
  return padded;
}

// ---------------------------------------------------------------------------
// The JSON artifact
// ---------------------------------------------------------------------------

void EmitSpan(llvm::json::OStream& j, const core::SpanStats& span) {
  j.object([&] {
    j.attributeArray("children", [&] {
      for (const core::SpanStats& child : span.children) EmitSpan(j, child);
    });
    j.attribute("count", static_cast<std::int64_t>(span.count));
    // Measured only on interval-bearing spans; see SpanMode.
    if (span.cpu_measured) {
      j.attribute("cpu_inclusive_ns",
                  static_cast<std::int64_t>(span.cpu_inclusive.count()));
    }
    if (span.distribution.has_value()) {
      j.attributeObject("distribution", [&] {
        j.attribute(
            "max_ns",
            static_cast<std::int64_t>(span.distribution->max.count()));
        j.attribute(
            "p50_ns",
            static_cast<std::int64_t>(span.distribution->p50.count()));
        j.attribute(
            "p95_ns",
            static_cast<std::int64_t>(span.distribution->p95.count()));
        j.attribute(
            "p99_ns",
            static_cast<std::int64_t>(span.distribution->p99.count()));
        j.attribute("samples_truncated", span.distribution->samples_truncated);
      });
    }
    j.attribute("max_ns", static_cast<std::int64_t>(span.max.count()));
    // Present only where the sampler retained intervals for this span.
    if (span.memory.has_value()) {
      j.attributeObject("memory", [&] {
        j.attribute("delta", span.memory->rss_delta);
        j.attribute("footprint_peak",
                    static_cast<std::int64_t>(span.memory->footprint_peak));
        j.attribute("peak_within",
                    static_cast<std::int64_t>(span.memory->peak_within));
        j.attribute("rss_end",
                    static_cast<std::int64_t>(span.memory->rss_end));
        j.attribute("rss_start",
                    static_cast<std::int64_t>(span.memory->rss_start));
      });
    }
    j.attribute("min_ns", static_cast<std::int64_t>(span.min.count()));
    j.attribute("name", span.name);
    // Ordered by the recorder — (wall desc, label asc), design spec section
    // 6.5 rule 2 — and emitted as given. `rank` restates that order, so
    // re-sorting here could only disagree with the rank the producer chose.
    if (!span.top_n.empty()) {
      j.attributeArray("top_n", [&] {
        for (std::size_t i = 0; i < span.top_n.size(); ++i) {
          j.object([&] {
            j.attribute("label", span.top_n[i].label);
            j.attribute("rank", static_cast<std::int64_t>(i + 1));
            j.attribute(
                "wall_ns",
                static_cast<std::int64_t>(span.top_n[i].wall.count()));
          });
        }
      });
    }
    j.attribute("wall_inclusive_ns",
                static_cast<std::int64_t>(span.wall_inclusive.count()));
    j.attribute("wall_self_ns",
                static_cast<std::int64_t>(span.wall_self.count()));
  });
}

void EmitIdentity(llvm::json::OStream& j, const RunIdentity& identity) {
  j.attributeObject("identity", [&] {
    j.attribute("batch_id", identity.batch_id);
    j.attribute("build_variant_id", identity.build_variant_id);
    j.attribute("engine_toolchain_identity", identity.engine_toolchain_identity);
    j.attribute("projection_id", identity.projection_id);
    j.attribute("repository_id", identity.repository_id);
    j.attribute("revision_id", identity.revision_id);
    j.attribute("run_id", identity.run_id);
    j.attribute("svf_config_hash", identity.svf_config_hash);
    j.attribute("wpa_config_hash", identity.wpa_config_hash);
  });
}

void EmitEnvironment(llvm::json::OStream& j, const RunEnvironment& environment) {
  j.attributeObject("environment", [&] {
    j.attribute("arch", environment.arch);
    j.attribute("build_type", environment.build_type);
    j.attribute("cores", static_cast<std::int64_t>(environment.cores));
    j.attribute("cpu_model", environment.cpu_model);
    j.attribute("git_revision", environment.git_revision);
    j.attribute("host_compiler", environment.host_compiler);
    j.attribute("os", environment.os);
    j.attribute("ram_bytes", static_cast<std::int64_t>(environment.ram_bytes));
    j.attribute("veritas_version", environment.veritas_version);
  });
}

void EmitInventory(llvm::json::OStream& j, const RunInventory& inventory) {
  const RunIncrementality& incrementality = inventory.incrementality;
  const RunInputInventory& input = inventory.input;
  const RunOutputInventory& output = inventory.output;
  j.attributeObject("inventory", [&] {
    j.attributeObject("incrementality", [&] {
      j.attribute("components_executed",
                  static_cast<std::int64_t>(incrementality.components_executed));
      j.attribute("components_reused",
                  static_cast<std::int64_t>(incrementality.components_reused));
      j.attribute(
          "summaries_recomputed",
          static_cast<std::int64_t>(incrementality.summaries_recomputed));
      j.attribute("summaries_reused",
                  static_cast<std::int64_t>(incrementality.summaries_reused));
    });
    j.attributeObject("input", [&] {
      j.attribute("compiler_id", input.compiler_id);
      j.attribute("compiler_version", input.compiler_version);
      j.attribute("include_closure_hash", input.include_closure_hash);
      j.attribute("source_tree_hash", input.source_tree_hash);
      j.attribute("target_triple", input.target_triple);
      j.attribute("translation_units",
                  static_cast<std::int64_t>(input.translation_units));
    });
    j.attributeObject("output", [&] {
      j.attribute("canonical_facts",
                  static_cast<std::int64_t>(output.canonical_facts));
      j.attributeObject("components_by_kind", [&] {
        for (const auto& kind : SortedCopy(
                 output.components_by_kind,
                 [](const std::pair<std::string, std::uint64_t>& entry)
                     -> const std::string& { return entry.first; })) {
          j.attribute(kind.first, static_cast<std::int64_t>(kind.second));
        }
      });
      j.attribute("cpg_edges", static_cast<std::int64_t>(output.cpg_edges));
      j.attribute("cpg_nodes", static_cast<std::int64_t>(output.cpg_nodes));
      j.attribute("rooted_input_facts",
                  static_cast<std::int64_t>(output.rooted_input_facts));
      j.attribute("summaries_published",
                  static_cast<std::int64_t>(output.summaries_published));
      j.attribute("svfg_edges", static_cast<std::int64_t>(output.svfg_edges));
      j.attribute("svfg_nodes", static_cast<std::int64_t>(output.svfg_nodes));
      j.attributeObject("unknowns_by_reason", [&] {
        for (const auto& reason : SortedCopy(
                 output.unknowns_by_reason,
                 [](const std::pair<std::string, std::uint64_t>& entry)
                     -> const std::string& { return entry.first; })) {
          j.attribute(reason.first, static_cast<std::int64_t>(reason.second));
        }
      });
    });
  });
}

void EmitStore(llvm::json::OStream& j, const StoreSummary& store) {
  j.attributeObject("store", [&] {
    j.attributeObject("bytes", [&] {
      for (const NamedBytes& entry : SortedCopy(
               store.bytes,
               [](const NamedBytes& named) -> const std::string& {
                 return named.name;
               })) {
        j.attribute(entry.name, static_cast<std::int64_t>(entry.bytes));
      }
    });
    j.attributeArray("cross_checks", [&] {
      for (const CrossCheck& check : SortedCopy(
               store.cross_checks,
               [](const CrossCheck& entry) -> const std::string& {
                 return entry.name;
               })) {
        j.object([&] {
          j.attribute("agrees", check.agrees);
          j.attribute("from_memory",
                      static_cast<std::int64_t>(check.from_memory));
          j.attribute("from_store",
                      static_cast<std::int64_t>(check.from_store));
          j.attribute("name", check.name);
        });
      }
    });
    j.attributeArray("tables", [&] {
      for (const TableRowCount& table :
           SortedCopy(store.tables,
                      [](const TableRowCount& entry) -> const std::string& {
                        return entry.table;
                      })) {
        j.object([&] {
          j.attribute("rows", static_cast<std::int64_t>(table.rows));
          j.attribute("table", table.table);
        });
      }
    });
  });
}

void EmitMemory(llvm::json::OStream& j, const core::RunMetricsStats& metrics,
                bool emit_series) {
  // The presence test is "was it measured", never "was it emitted". Those are
  // different questions: a run with no sampler, or none that measured
  // successfully, has nothing to report, while a run whose series was
  // suppressed still has a peak. A span's block follows the same rule.
  if (!metrics.memory_measured) return;
  j.attributeObject("memory", [&] {
    j.attributeObject("peak", [&] {
      j.attribute("at_ms",
                  static_cast<std::int64_t>(metrics.peak_at.count()));
      j.attribute("footprint_bytes",
                  static_cast<std::int64_t>(metrics.peak_footprint_bytes));
      j.attribute("rss_bytes",
                  static_cast<std::int64_t>(metrics.peak_rss_bytes));
    });
    // Suppressed samples are absent, not an empty array: an empty array is a
    // third rendering, and reads as "nothing was measured".
    if (emit_series) {
      j.attributeArray("series", [&] {
        for (const core::MemorySample& sample : metrics.series) {
          j.object([&] {
            j.attribute("footprint_bytes",
                        static_cast<std::int64_t>(sample.footprint_bytes));
            j.attribute("rss_bytes",
                        static_cast<std::int64_t>(sample.rss_bytes));
            j.attribute("t_ms", static_cast<std::int64_t>(sample.t.count()));
          });
        }
      });
    }
    j.attribute("series_decimation",
                static_cast<std::int64_t>(metrics.series_decimation));
  });
}

// ---------------------------------------------------------------------------
// The text report
// ---------------------------------------------------------------------------

// One rendered tree row: the label already carries the box-drawing prefix, so
// the column width can be measured before anything is written.
struct PhaseRow {
  std::string label;
  const core::SpanStats* span = nullptr;
};

void CollectPhaseRows(const core::SpanStats& span, std::string_view prefix,
                      bool is_root, bool is_last,
                      std::vector<PhaseRow>* rows) {
  std::string label(prefix);
  if (!is_root) label.append(is_last ? "└─ " : "├─ ");
  label.append(span.name);
  rows->push_back(PhaseRow{label, &span});

  // The root's children share the root's indent; a non-root node's children
  // hang off its own connector column, and the sibling that follows a last
  // child contributes no vertical rule.
  const std::string child_prefix =
      std::string(prefix) + (is_root ? "" : (is_last ? "   " : "│  "));
  for (std::size_t i = 0; i < span.children.size(); ++i) {
    CollectPhaseRows(span.children[i], child_prefix, /*is_root=*/false,
                     /*is_last=*/i + 1 == span.children.size(), rows);
  }
}

// FormatSignedBytes renders a signed memory delta with an explicit sign and the
// same tiers as FormatBytes, so a MiB-scale release beside a GiB-scale peak
// reads as "-3.00 MiB" instead of being rounded away to a bare "-0.00".
std::string FormatSignedBytes(std::int64_t bytes) {
  const bool negative = bytes < 0;
  // Negate in unsigned arithmetic: the magnitude of INT64_MIN has no signed
  // counterpart, and this form has no overflow to reason about.
  const std::uint64_t magnitude =
      negative ? ~static_cast<std::uint64_t>(bytes) + 1ull
               : static_cast<std::uint64_t>(bytes);
  return std::string(negative ? "-" : "+") + FormatBytes(magnitude);
}

std::string FormatPhaseRow(const PhaseRow& row, std::size_t width) {
  const core::SpanStats& span = *row.span;
  // "-" rather than zero wherever the producer recorded no measurement: an
  // uninstrumented zero must not be mistaken for a measured one.
  const std::string cpu = span.cpu_measured
                              ? FormatDuration(span.cpu_inclusive)
                              : std::string("-");
  const std::string peak = span.memory.has_value()
                               ? FormatBytes(span.memory->peak_within)
                               : std::string("-");
  // The delta carries its own unit, like the peak beside it, so the reader can
  // compare the two columns and tell a MiB-scale release from a GiB-scale one.
  // The column is as wide as the widest value the tiers can produce
  // ("-1024.00 MiB"): a longer string would push the row's columns out of line.
  const std::string delta = span.memory.has_value()
                                ? FormatSignedBytes(span.memory->rss_delta)
                                : std::string("-");

  std::string line = PadRight(row.label, width);
  line.append("  ");
  line.append(PadLeft(FormatDuration(span.wall_inclusive), 10));
  line.append("  ");
  line.append(PadLeft(FormatDuration(span.wall_self), 10));
  line.append("  ");
  line.append(PadLeft(cpu, 10));
  line.append("  ");
  line.append(PadLeft(peak, 10));
  line.append("  ");
  line.append(PadLeft(delta, 11));
  line.push_back('\n');
  return line;
}

void CollectDistributedSpans(const core::SpanStats& span,
                             std::vector<const core::SpanStats*>* out) {
  if (span.distribution.has_value()) out->push_back(&span);
  for (const core::SpanStats& child : span.children) {
    CollectDistributedSpans(child, out);
  }
}

void AppendPhaseBlock(std::string_view name, const core::SpanStats& span,
                      std::string* out) {
  out->append("  ");
  out->append(name);
  out->append(" p50 ");
  out->append(FormatDuration(span.distribution->p50));
  out->append(" · p95 ");
  out->append(FormatDuration(span.distribution->p95));
  out->append(" · p99 ");
  out->append(FormatDuration(span.distribution->p99));
  out->append(" · max ");
  out->append(FormatDuration(span.distribution->max));
  out->push_back('\n');
  if (span.top_n.empty()) return;
  out->append("  slowest:");
  for (std::size_t i = 0; i < span.top_n.size(); ++i) {
    if (i != 0) out->append(" ·");
    out->push_back(' ');
    out->append(span.top_n[i].label);
    out->push_back(' ');
    out->append(FormatDuration(span.top_n[i].wall));
  }
  out->push_back('\n');
}

}  // namespace

std::string RenderRunReportJson(const RunReport& report) {
  std::string out;
  llvm::raw_string_ostream os(out);
  llvm::json::OStream j(os, /*IndentSize=*/2);

  // Top-level keys in sorted order: complete, config, counters, diagnostics,
  // environment, identity, inventory, memory, phases, schema, store.
  j.object([&] {
    j.attribute("complete", report.metrics.complete);

    // The recorder options the run actually used, beside the one analysis knob
    // the artifact has to carry: a second full WPA whose canonical results must
    // agree changes what the run does and is invisible in either configuration
    // hash. The rest of the analysis configuration is deliberately absent —
    // RunReport does not depend on the analysis library, and svf_config_hash
    // and wpa_config_hash cover every other AnalysisConfig field.
    const core::RunMetricsOptions& options = report.metrics_options;
    j.attributeObject("config", [&] {
      j.attribute("conformance_oracle", report.conformance_oracle);
      j.attributeObject("metrics", [&] {
        j.attribute("emit_series", options.emit_series);
        j.attribute("sample_interval_ms",
                    static_cast<std::int64_t>(options.sample_interval.count()));
        j.attribute("series_capacity",
                    static_cast<std::int64_t>(options.series_capacity));
        j.attribute("span_sample_cap",
                    static_cast<std::int64_t>(options.span_sample_cap));
        j.attribute("top_n", static_cast<std::int64_t>(options.top_n));
      });
    });

    j.attributeArray("counters", [&] {
      for (const core::Counter& counter : SortedCopy(
               report.metrics.counters,
               [](const core::Counter& entry) -> const std::string& {
                 return entry.name;
               })) {
        j.object([&] {
          j.attribute("name", counter.name);
          j.attribute("unit", counter.unit);
          j.attribute("value", static_cast<std::int64_t>(counter.value));
        });
      }
    });

    j.attributeArray("diagnostics", [&] {
      for (const std::string& message : report.metrics.diagnostics) {
        j.value(message);
      }
    });

    EmitEnvironment(j, report.environment);

    EmitIdentity(j, report.identity);

    EmitInventory(j, report.inventory);

    EmitMemory(j, report.metrics, report.metrics_options.emit_series);

    j.attributeArray("phases", [&] { EmitSpan(j, report.metrics.root); });

    j.attribute("schema", "veritas.run-metrics.v1");

    EmitStore(j, report.store);
  });

  os.flush();
  out.push_back('\n');
  return out;
}

std::string RenderRunReportText(const RunReport& report) {
  std::vector<PhaseRow> rows;
  CollectPhaseRows(report.metrics.root, "  ", /*is_root=*/true,
                   /*is_last=*/true, &rows);

  std::size_t width = 0;
  for (const PhaseRow& row : rows) {
    width = std::max(width, DisplayWidth(row.label));
  }

  std::string out = "Analysis phase report\n";
  for (const PhaseRow& row : rows) out.append(FormatPhaseRow(row, width));

  std::vector<const core::SpanStats*> distributed;
  CollectDistributedSpans(report.metrics.root, &distributed);
  for (const core::SpanStats* span : distributed) {
    AppendPhaseBlock(span->name, *span, &out);
  }

  // Every kind's expected count, summed: the report has no separate total
  // field, and the kinds partition the components. The line is omitted when
  // nothing was recorded, so a zero can never be read as a measurement.
  std::uint64_t expected_components = 0;
  for (const auto& kind : report.inventory.output.components_by_kind) {
    expected_components += kind.second;
  }
  const RunIncrementality& incrementality = report.inventory.incrementality;
  if (!report.inventory.output.components_by_kind.empty() ||
      incrementality.components_reused != 0 ||
      incrementality.components_executed != 0) {
    out.append("WPA components: ");
    out.append(std::to_string(expected_components));
    out.append(" expected, ");
    out.append(std::to_string(incrementality.components_reused));
    out.append(" reused, ");
    out.append(std::to_string(incrementality.components_executed));
    out.append(" executed\n");
  }

  if (!report.store.tables.empty() || !report.store.bytes.empty()) {
    std::string body;
    std::uint64_t total_bytes = 0;
    for (const TableRowCount& table :
         SortedCopy(report.store.tables,
                    [](const TableRowCount& entry) -> const std::string& {
                      return entry.table;
                    })) {
      if (!body.empty()) body.append(" · ");
      body.append(table.table);
      body.push_back(' ');
      body.append(std::to_string(table.rows));
      body.append(" rows");
    }
    for (const NamedBytes& entry : SortedCopy(
             report.store.bytes,
             [](const NamedBytes& named) -> const std::string& {
               return named.name;
             })) {
      total_bytes += entry.bytes;
      if (!body.empty()) body.append(" · ");
      body.append(entry.name);
      body.push_back(' ');
      body.append(FormatBytes(entry.bytes));
    }
    // The total is the sum of the named stores, so it is printed only when at
    // least one was measured: a total with no parts is not a measurement.
    if (!report.store.bytes.empty()) {
      if (!body.empty()) body.append(" · ");
      body.append("total ");
      body.append(FormatBytes(total_bytes));
    }
    out.append("Store: ");
    out.append(body);
    out.push_back('\n');
  }

  for (const CrossCheck& check : SortedCopy(
           report.store.cross_checks,
           [](const CrossCheck& entry) -> const std::string& {
             return entry.name;
           })) {
    out.append("Cross-check: ");
    out.append(check.name);
    out.push_back(' ');
    out.append(std::to_string(check.from_store));
    out.append(" (store) == ");
    out.append(std::to_string(check.from_memory));
    out.append(" (in-memory) ");
    out.append(check.agrees ? "OK" : "MISMATCH");
    out.push_back('\n');
  }

  return out;
}

std::string FormatDuration(std::chrono::nanoseconds value) {
  if (value.count() == 0) return "0ns";
  const double nanos = static_cast<double>(value.count());
  char buffer[32];
  if (value >= std::chrono::seconds(1)) {
    std::snprintf(buffer, sizeof(buffer), "%.3fs", nanos / kNanosPerSecond);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.3fms", nanos / kNanosPerMilli);
  }
  return std::string(buffer);
}

std::string FormatBytes(std::uint64_t bytes) {
  const double value = static_cast<double>(bytes);
  char buffer[32];
  if (bytes >= kBytesPerGiB) {
    std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                  value / static_cast<double>(kBytesPerGiB));
  } else if (bytes >= kBytesPerMiB) {
    std::snprintf(buffer, sizeof(buffer), "%.2f MiB",
                  value / static_cast<double>(kBytesPerMiB));
  } else if (bytes >= kBytesPerKiB) {
    // A third tier, not a stop at MiB: a phase that allocates a few hundred
    // kilobytes would otherwise print as "0.00 MiB", which reads as *nothing
    // was measured* rather than *this phase is small*. The memory column exists
    // to tell those two apart.
    std::snprintf(buffer, sizeof(buffer), "%.1f KiB",
                  value / static_cast<double>(kBytesPerKiB));
  } else {
    // Below a KiB there is no smaller unit to round into, so print the count.
    std::snprintf(buffer, sizeof(buffer), "%llu B",
                  static_cast<unsigned long long>(bytes));
  }
  return std::string(buffer);
}

}  // namespace veritas::observability
