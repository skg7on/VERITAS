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

#include "veritas/analysis/ProjectAnalyzer.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "ProjectFixture.h"
#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::analysis {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Published-table fingerprints
// ---------------------------------------------------------------------------

// Spec section 8.2's second requirement: the four published table digests must
// match between the recording run and the non-recording one. A digest over the
// whole table is the strongest form of that claim, because it is sensitive to
// every cell that a count or a schema check would sail past.
//
// Two boundaries are deliberate.
//
// The ORDER BY is the table's PRIMARY KEY, never `rowid`. A rowid-ordered dump
// has already differed across builds in this repository, so a comparison built
// on one would fail for a reason that is not the thing under test. Where a
// table's declared primary key contains a run-scoped column, the ordering uses
// the primary key restricted to the dumped columns, which within a single
// store is the same order.
//
// `run_id` is the only column excluded, from the three tables that carry it
// (`run_fact_bindings`, `provenance_nodes`, `provenance_edges`) and from
// nothing else. It is excluded because the digest is a statement about the
// published content rather than about the run key: `run_id` is content-derived
// from the run descriptor, and the `EXPECT_EQ` cases in
// `RecordingDoesNotMoveAnyIdentity` pin its equality between these same two
// runs directly, so re-pinning it here would compare one string twice.
// Excluding a whole table would make this comparison a statement about the
// tables that were left.
// `analysis_facts` carries no run-scoped column at all, so its dump is the
// table.
//
// `run_fact_bindings.binding_id` is excluded too, and for a different reason:
// it is declared `INTEGER PRIMARY KEY AUTOINCREMENT`, which in SQLite is the
// rowid under another name. Dumping it would make the comparison rowid-ordered
// by construction. The columns that remain are a total order.
struct TableDump {
  const char* table;
  const char* sql;
};

constexpr TableDump kPublishedTableDumps[] = {
    {"analysis_facts",
     "SELECT fact_id, relation_name, cells_hex FROM analysis_facts "
     "ORDER BY fact_id"},
    {"run_fact_bindings",
     "SELECT fact_id, confidence, producer_kind, analyzer_run_id, scope_kind, "
     "scope_id, selected_witness_id, is_current FROM run_fact_bindings "
     "ORDER BY fact_id, is_current, producer_kind, analyzer_run_id, scope_kind, "
     "scope_id, selected_witness_id, confidence"},
    {"provenance_nodes",
     "SELECT output_fact_id, witness_id, selected, producer_kind, producer_id, "
     "rule_id, rule_version, analyzer_run_id, source_anchor_id, summary_id, "
     "description FROM provenance_nodes "
     "ORDER BY output_fact_id, witness_id"},
    {"provenance_edges",
     "SELECT output_fact_id, witness_id, input_kind, input_id, input_ordinal "
     "FROM provenance_edges "
     "ORDER BY output_fact_id, witness_id, input_ordinal, input_kind, input_id"},
};

// One row's fields joined, and rows terminated, by separator bytes, so a value
// that happens to contain a separator character cannot forge a field boundary
// and make two different tables dump alike.
std::string CanonicalDump(const std::vector<std::vector<std::string>>& rows) {
  std::string text;
  for (const std::vector<std::string>& row : rows) {
    for (const std::string& field : row) {
      text.append(field);
      text.push_back('\x1f');
    }
    text.push_back('\x1e');
  }
  return text;
}

std::string DigestOf(std::string_view text) {
  const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
  return core::DigestToHex(
      core::ComputeSHA256(std::span<const std::byte>(bytes, text.size())));
}

struct TableFingerprint {
  std::string table;
  std::size_t rows = 0;
  std::string digest;
};

// Opens the store under `output_root` and fingerprints each published table.
// Propagates a failed open or query rather than returning an empty digest: an
// unreadable store must not compare equal to a readable one.
StatusOr<std::vector<TableFingerprint>> FingerprintPublishedTables(
    const fs::path& output_root) {
  auto store = summarydb::MetadataStore::Open(output_root / "metadata.db");
  if (!store.ok()) return store.status();

  std::vector<TableFingerprint> fingerprints;
  for (const TableDump& dump : kPublishedTableDumps) {
    auto rows = store->Query(dump.sql, {});
    if (!rows.ok()) return rows.status();
    TableFingerprint fingerprint;
    fingerprint.table = dump.table;
    fingerprint.rows = rows->size();
    fingerprint.digest = DigestOf(CanonicalDump(*rows));
    fingerprints.push_back(std::move(fingerprint));
  }
  return fingerprints;
}

// A per-process-unique output root, as `RecordsTheExpectedSpanTree` explains:
// the component-result cache is keyed on the output root, so a root left behind
// by an earlier run would turn the per-component execute span into a cache hit
// and the two runs under comparison would not be the same shape of run.
fs::path UniqueOutputRoot(const std::string& label) {
  std::string output_template =
      (fs::temp_directory_path() / ("veritas-" + label + "-XXXXXX")).string();
  char* created = ::mkdtemp(output_template.data());
  if (created == nullptr) {
    ADD_FAILURE() << "cannot create an output root under "
                  << fs::temp_directory_path();
    return {};
  }
  return fs::path(created);
}

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
  // The four identity fields spec section 8.2's first requirement covers, all
  // public on `ProjectAnalysisResult` and all read into the artifact's identity
  // block. `batch_id` is named there in the byte-equality list; the two
  // configuration hashes and the toolchain identity are the coordinates a
  // reader is told to compare *instead of* excluding the block, so a recording
  // run that moved either would break the comparison the block exists to
  // enable. They are compared here because the field set grew after this case
  // was first written, not because they were judged stable.
  EXPECT_EQ(result_a->batch_id, result_b->batch_id);
  EXPECT_EQ(result_a->svf_configuration_hash, result_b->svf_configuration_hash);
  EXPECT_EQ(result_a->wpa_configuration_hash, result_b->wpa_configuration_hash);
  EXPECT_EQ(result_a->engine_toolchain_identity,
            result_b->engine_toolchain_identity);
  // The recorder actually ran, so an empty tree cannot make this vacuous.
  EXPECT_FALSE(metrics.TakeStats().root.children.empty());
}

// Spec section 8.2's second requirement, which no case covered: the four
// published table digests must be equal between the same two runs. Identity
// equality above is a statement about nine strings; this is a statement about
// the published content they key, and it is the strongest guard the design has
// — a digest moves for any cell in any row.
TEST(PhaseObservabilityIdentityTest,
     PublishesTheSameTableContentWithAndWithoutRecording) {
  const auto project = testing::FixtureProject("multiple_tus");
  const fs::path output_a = UniqueOutputRoot("table-digest-off");
  const fs::path output_b = UniqueOutputRoot("table-digest-on");
  ASSERT_FALSE(output_a.empty());
  ASSERT_FALSE(output_b.empty());

  const auto config = veritas::analysis::AnalysisConfig::Default();

  auto result_a = ProjectAnalyzer{}.AnalyzeProject(
      ProjectAnalysisRequest{.project_root = project, .output_root = output_a},
      config);
  ASSERT_TRUE(result_a.ok()) << result_a.status().message();

  core::RunMetricsOptions options;
  core::RunMetrics metrics(options);
  auto result_b = ProjectAnalyzer{}.AnalyzeProject(
      ProjectAnalysisRequest{.project_root = project, .output_root = output_b},
      config, &metrics);
  ASSERT_TRUE(result_b.ok()) << result_b.status().message();

  auto fingerprints_a = FingerprintPublishedTables(output_a);
  ASSERT_TRUE(fingerprints_a.ok()) << fingerprints_a.status().message();
  auto fingerprints_b = FingerprintPublishedTables(output_b);
  ASSERT_TRUE(fingerprints_b.ok()) << fingerprints_b.status().message();
  ASSERT_EQ(fingerprints_a->size(), 4u);
  ASSERT_EQ(fingerprints_b->size(), 4u);

  for (std::size_t i = 0; i < fingerprints_a->size(); ++i) {
    const TableFingerprint& a = (*fingerprints_a)[i];
    const TableFingerprint& b = (*fingerprints_b)[i];
    ASSERT_EQ(a.table, b.table);
    // A non-empty row count, so a digest comparison cannot pass by comparing
    // two empty tables: `DigestOf("")` is a digest, and it is the same one
    // whatever went wrong.
    EXPECT_GT(a.rows, 0u) << a.table << " published no rows";
    EXPECT_EQ(a.rows, b.rows) << a.table << " row count moved";
    EXPECT_EQ(a.digest, b.digest)
        << a.table
        << " content digest moved between the recording and non-recording run";
  }

  // And the four digests must differ from one another. Two of these tables hold
  // exactly the same number of rows in this fixture (24, measured), so a digest
  // that had collapsed to a row count — or to any constant, which a broken
  // `CanonicalDump` would produce — would satisfy every assertion above while
  // comparing nothing. Distinctness is what says the dump carries column
  // content, and it is the assertion that fails first if the digest stops being
  // a function of the rows.
  for (std::size_t i = 0; i < fingerprints_a->size(); ++i) {
    for (std::size_t j = i + 1; j < fingerprints_a->size(); ++j) {
      EXPECT_NE((*fingerprints_a)[i].digest, (*fingerprints_a)[j].digest)
          << (*fingerprints_a)[i].table << " and "
          << (*fingerprints_a)[j].table << " dumped alike ("
          << (*fingerprints_a)[i].rows << " and "
          << (*fingerprints_a)[j].rows << " rows)";
    }
  }
}

// Instrumentation can compile, run, and record nothing: a wrong null check, a
// name that does not resolve where the site assumed, a span opened in a scope
// that already returned. No analysis result moves either way, so this case
// asserts the recorded tree itself -- presence and counts, never durations, so
// it cannot flake.
TEST(PhaseObservabilityIdentityTest, RecordsTheExpectedSpanTree) {
  const auto project = testing::FixtureProject("multiple_tus");
  // A root unique to this process, as `FixtureProject` and the WPA test
  // fixtures both provide. A name derived from `std::rand()` is not enough:
  // the call is unseeded, so every run of this invocation shape resolves to
  // the same path. The component-result cache is keyed on the output root, so
  // a root left behind by an earlier run turns the per-component execute span
  // into a cache hit -- and this case would then fail on its second run for a
  // reason that has nothing to do with the instrumentation it exists to prove.
  std::string output_template =
      (fs::temp_directory_path() / "veritas-metrics-spans-XXXXXX").string();
  char* created = ::mkdtemp(output_template.data());
  ASSERT_NE(created, nullptr) << "cannot create an output root under "
                              << fs::temp_directory_path();
  const fs::path output = created;
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
  for (const char* name : {"svf.svfg_nodes", "wpa.components.expected",
                           "wpa.components.reused", "wpa.components.executed",
                           "facts.rooted_input", "facts.canonical"}) {
    EXPECT_EQ(counters.count(name), 1u) << name << " missing";
  }
  EXPECT_GT(counters["wpa.components.expected"], 0u);
  EXPECT_GT(counters["svf.svfg_nodes"], 0u);
}

}  // namespace
}  // namespace veritas::analysis
