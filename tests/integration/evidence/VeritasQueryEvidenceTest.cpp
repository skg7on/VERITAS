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

// VeritasQueryEvidenceTest.cpp — DEM-001.
//
// `veritas-query evidence overflow` must publish the M10B slice as deterministic
// diagnostic JSON. These tests materialize the unsafe fixture through the real
// M1→M6→M9→M10A pipeline, build the typed oracle in process, then run the
// PUBLIC CLI against the same materialized store. Typed content is validated
// first (claim seed, the real value-flow path, the pinned run, provenance, and
// the completeness of every fact set), and only then are bytes compared: CLI
// stdout vs `ToDiagnosticJson(oracle)`, vs the checked-in golden, and across a
// second independent materialization ("second clean store").
//
// The JSON is the DESC0PED slice. What the real pipeline produces and what this
// CLI therefore publishes is the value-flow closure (GlobalFlow projected onto
// CPG kFlowsTo edges, from the origin value to the value that leaves the
// analyzed code into the unmodeled sink) plus the provenance closure of the six
// per-query completion certificates.
//
// Two slots are complete-EMPTY by construction of BuildEvidenceInput, not by
// omission here, and are serialized faithfully rather than dropped:
//   * `dominating_checks` matches only POSITIVE "dominating_check" facts, and
//     M9/M10A derives only the negative "dominating_check_absence" certificate
//     (asserted directly on the fact store by OverflowEvidenceFixtureTest).
//   * `unknowns` is scoped through FindContainingFunction, which resolves a
//     node to its function through a kContains edge. The M6 projection emits no
//     kContains edges, so the scope falls back to the degenerate empty
//     FunctionVariant ref and no UnknownEffect fact matches.
// DEFERRED (scoping decision, not a defect): value-range [0,65535], capacity
// 2048, alias states, and the positive "dominating_check" fact are DEFERRED to
// a later milestone — M9/M10A does not emit them.
//
// The evidence_overflow_* fixtures compile with
// `-fdebug-prefix-map=@PROJECT_ROOT@=.` so the materialized checkout root cannot
// leak into debug info; without it the slice JSON differs between two
// materializations of the same source (asserted below).

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "evidence/RealEvidencePipeline.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/FactStoreEvidenceBackend.h"
#include "veritas/evidence/OverflowClaimSeed.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/summarydb/MetadataStore.h"

#ifndef VERITAS_QUERY_BINARY
#error "VERITAS_QUERY_BINARY must be defined by the build system"
#endif
#ifndef VERITAS_GOLDEN_DIR
#error "VERITAS_GOLDEN_DIR must be defined by the build system"
#endif

namespace veritas::testing {
namespace {

namespace ev = evidence;

// Mirrors the public CLI defaults documented in the M10B design spec §6; the
// test passes them explicitly so a change to either side is caught.
ev::EvidenceQueryBudget Budget() {
  return ev::EvidenceQueryBudget{/*max_depth=*/8, /*max_nodes=*/256,
                                 /*max_paths=*/5, /*max_facts_per_query=*/64,
                                 /*max_provenance_depth=*/8};
}

std::vector<std::string> BudgetFlags() {
  return {"--max-depth",    "8", "--max-nodes", "256",
          "--max-paths",    "5", "--max-facts", "64",
          "--max-provenance-depth", "8"};
}

std::string ShellQuote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
};

CliResult RunVeritasQuery(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_QUERY_BINARY);
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(ShellQuote(argument));
  }
  command.append(" 2>&1");

  CliResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }
  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result.stdout_text.append(buffer.data());
  }
  const int status = ::pclose(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

CliResult RunEvidenceOverflow(const std::filesystem::path& db_dir) {
  std::vector<std::string> arguments{"evidence", "overflow", "--sink", "memcpy",
                                     "--format", "json", "--db", db_dir.string()};
  const auto budget = BudgetFlags();
  arguments.insert(arguments.end(), budget.begin(), budget.end());
  return RunVeritasQuery(arguments);
}

// The in-process typed oracle: the exact `EvidenceBuildInput` the CLI must
// serialize, built from the same materialized store through the same public
// service entry point, plus the store root the CLI is pointed at.
struct Oracle {
  ev::EvidenceBuildInput input;
  cpg::ThinCpg cpg;
  std::filesystem::path output_root;
};

StatusOr<Oracle> BuildOracle(std::string_view fixture) {
  auto snapshot = AnalyzeRealFixture(fixture);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  if (!facts.ok()) {
    return facts.status();
  }
  // The CLI resolves the seed itself; the oracle resolves it through the same
  // shared production entry point so the two cannot drift. The CLI's own
  // resolution is exercised end to end by the byte comparison below.
  auto seed = ev::ResolveOverflowClaimSeed(snapshot->cpg, *facts, "memcpy");
  if (!seed.ok()) {
    return seed.status();
  }
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  ev::EvidenceQueryService service(snapshot->cpg, backend, snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, Budget());
  if (!input.ok()) {
    return input.status();
  }
  return Oracle{.input = std::move(*input),
                .cpg = std::move(snapshot->cpg),
                .output_root = snapshot->output_root};
}

// Typed assertions on the oracle before any byte comparison.
void ExpectTypedSliceContent(const ev::EvidenceBuildInput& input,
                             const cpg::ThinCpg& cpg) {
  // Claim seed: a buffer-overflow finding with a resolvable subject/source/sink.
  EXPECT_EQ(input.claim_seed.kind, ev::ClaimKind::kBufferOverflow);
  EXPECT_EQ(input.claim_seed.severity, ev::Severity::kHigh);
  EXPECT_EQ(input.claim_seed.finding_id.kind, core::IdKind::kFact);
  EXPECT_EQ(input.claim_seed.subject_ref.kind, core::IdKind::kMemoryRef);
  EXPECT_EQ(input.claim_seed.source_ref.kind, core::IdKind::kValueRef);
  EXPECT_EQ(input.claim_seed.sink_ref.kind, core::IdKind::kValueRef);
  EXPECT_NE(input.claim_seed.subject_ref.digest_hex, std::string());
  EXPECT_NE(input.claim_seed.source_ref.digest_hex, std::string());
  EXPECT_NE(input.claim_seed.sink_ref.digest_hex, std::string());

  // The value-flow path the CLI publishes is non-empty, and every node and edge
  // it contains is a real member of the pinned projection (no fabricated graph
  // content).
  EXPECT_FALSE(input.flow_slice.nodes.empty());
  EXPECT_FALSE(input.flow_slice.edges.empty());
  std::set<core::StableId> cpg_nodes;
  for (const auto& node : cpg.nodes()) {
    cpg_nodes.insert(node.node_id);
  }
  std::set<core::StableId> cpg_edges;
  for (const auto& edge : cpg.edges()) {
    cpg_edges.insert(edge.edge_id);
  }
  for (const auto& node : input.flow_slice.nodes) {
    EXPECT_EQ(cpg_nodes.count(node.node_id), 1u) << node.label;
  }
  for (const auto& edge : input.flow_slice.edges) {
    EXPECT_EQ(cpg_edges.count(edge.edge_id), 1u);
  }
  // The seed's endpoints are the ends of the published path.
  EXPECT_EQ(cpg_nodes.count(input.claim_seed.source_ref), 1u);
  EXPECT_EQ(cpg_nodes.count(input.claim_seed.sink_ref), 1u);

  // Each query result is completeness-qualified and carries the pinned run.
  EXPECT_EQ(input.flow_slice.metadata.completeness,
            ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(input.flow_slice.metadata.truncation_reasons.empty());
  EXPECT_EQ(input.flow_slice.metadata.analysis_run_id.kind,
            core::IdKind::kAnalysisRun);

  // Deferred relations are complete-empty, not omitted and not fabricated.
  for (const ev::EvidenceFactSet* set :
       {&input.ranges, &input.capacities, &input.aliases,
        &input.dominating_checks, &input.unknowns}) {
    EXPECT_EQ(set->metadata.completeness, ev::QueryCompleteness::kComplete);
    EXPECT_TRUE(set->metadata.truncation_reasons.empty());
    EXPECT_TRUE(set->facts.empty());
  }

  // The one analysis run is pinned and every query's completion certificate is
  // resolvable.
  EXPECT_FALSE(input.query_completion_facts.empty());
  EXPECT_FALSE(input.query_completion_bindings.empty());
  EXPECT_GT(input.provenance.nodes_size(), 0);
}

// "Reverse backend insertion": re-insert the store's run fact bindings in the
// opposite physical order. The durable read surface is content-ordered — every
// read the evidence path issues is served by a covering index keyed on the
// content-derived id (`run_fact_bindings_current` for the fact query,
// `(projection_id, node_id)` for the projection), so `GetCurrentFacts` already
// yields fact-id order and insertion order is not observable today. Reversing
// it is therefore a regression guard: if a store change ever makes physical row
// order visible, this comparison fails instead of silently reordering the
// published slice.
void ReverseFactBindingInsertionOrder(const std::filesystem::path& db_dir) {
  auto store = veritas::summarydb::MetadataStore::Open(db_dir / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto first_fact = [&](std::string_view order_by) {
    auto rows = store->Query(
        "SELECT fact_id FROM run_fact_bindings WHERE is_current = 1 " +
            std::string(order_by) + " LIMIT 1",
        {});
    EXPECT_TRUE(rows.ok()) << rows.status().message();
    return rows.ok() && !rows->empty() ? (*rows)[0][0] : std::string();
  };

  const std::string before_rowid = first_fact("ORDER BY rowid");
  const std::string before_index = first_fact("");
  EXPECT_FALSE(before_rowid.empty());

  // binding_id is the table's rowid and is excluded, so the re-inserted rows
  // are assigned fresh, ascending rowids in the reversed sequence.
  static constexpr std::string_view kBindingColumns =
      "run_id, fact_id, confidence, producer_kind, analyzer_run_id, scope_kind, "
      "scope_id, selected_witness_id, is_current";
  const std::string create =
      "CREATE TABLE veritas_reversed_bindings AS SELECT " +
      std::string(kBindingColumns) + " FROM run_fact_bindings ORDER BY rowid DESC";
  const std::string insert = "INSERT INTO run_fact_bindings (" +
                             std::string(kBindingColumns) +
                             ") SELECT * FROM veritas_reversed_bindings";
  for (const std::string& statement :
       {create, std::string("DELETE FROM run_fact_bindings"), insert,
        std::string("DROP TABLE veritas_reversed_bindings")}) {
    const Status status = store->Execute(statement, {});
    ASSERT_TRUE(status.ok()) << status.message();
  }

  // The physical insertion order really was reversed ...
  EXPECT_NE(first_fact("ORDER BY rowid"), before_rowid)
      << "the reversal did not rewrite the physical row order";
  // ... while the read the evidence path performs is index-served and therefore
  // unchanged, which is why the CLI cannot observe insertion order.
  EXPECT_EQ(first_fact(""), before_index);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

TEST(VeritasQueryEvidenceTest, CliEmitsGoldenSliceJsonDeterministically) {
  auto oracle = BuildOracle("evidence_overflow_unsafe");
  ASSERT_TRUE(oracle.ok()) << oracle.status().message();

  // Typed oracle content first.
  ExpectTypedSliceContent(oracle->input, oracle->cpg);
  const std::string expected = ev::ToDiagnosticJson(oracle->input);
  EXPECT_EQ(expected.back(), '\n') << "slice JSON must end with one newline";
  EXPECT_EQ(expected.find("}}\n\n"), std::string::npos)
      << "slice JSON must not add a second trailing newline";

  // The public CLI over the same materialized store.
  const CliResult cli = RunEvidenceOverflow(oracle->output_root);
  ASSERT_EQ(cli.exit_code, 0) << cli.stdout_text;
  EXPECT_EQ(cli.stdout_text, expected)
      << "CLI output drifted from the typed oracle";

  // Determinism against the store's physical order: the same materialized store
  // with its run fact bindings re-inserted backwards must publish the same
  // bytes.
  ReverseFactBindingInsertionOrder(oracle->output_root);
  const CliResult reversed = RunEvidenceOverflow(oracle->output_root);
  ASSERT_EQ(reversed.exit_code, 0) << reversed.stdout_text;
  EXPECT_EQ(reversed.stdout_text, cli.stdout_text)
      << "slice JSON depends on fact-binding insertion order";

  // Determinism across an independent materialization: a second clean store in
  // a different checkout root must produce byte-identical JSON.
  auto second = BuildOracle("evidence_overflow_unsafe");
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_NE(second->output_root, oracle->output_root);
  const CliResult second_cli = RunEvidenceOverflow(second->output_root);
  ASSERT_EQ(second_cli.exit_code, 0) << second_cli.stdout_text;
  EXPECT_EQ(second_cli.stdout_text, cli.stdout_text)
      << "slice JSON is not byte-stable across stores/checkout roots";

  // The checked-in golden is the slice JSON the CLI publishes.
  const std::filesystem::path golden =
      std::filesystem::path(VERITAS_GOLDEN_DIR) / "overflow_unsafe.slice.json";
  ASSERT_TRUE(std::filesystem::exists(golden))
      << "missing golden; regenerate with the CLI: " << golden;
  EXPECT_EQ(cli.stdout_text, ReadFile(golden))
      << "CLI output drifted from " << golden;
}

TEST(VeritasQueryEvidenceTest, RejectsInvalidOptionSurface) {
  const std::string db = "/nonexistent/veritas-query-evidence";
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
      {"unknown format",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "sarif", "--db",
        db}},
      {"missing format", {"evidence", "overflow", "--sink", "memcpy", "--db", db}},
      {"missing sink", {"evidence", "overflow", "--format", "json", "--db", db}},
      {"unsupported sink",
       {"evidence", "overflow", "--sink", "strcpy", "--format", "json", "--db",
        db}},
      {"missing db",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json"}},
      {"duplicate option",
       {"evidence", "overflow", "--sink", "memcpy", "--sink", "memcpy",
        "--format", "json", "--db", db}},
      {"unknown option",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--bogus", "1"}},
      {"missing option value",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db"}},
      {"zero budget",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-nodes", "0"}},
      {"overflowing integer",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-nodes", "99999999999999999999999999"}},
      {"non-numeric budget",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-depth", "eight"}},
  };

  for (const auto& [name, arguments] : cases) {
    const CliResult result = RunVeritasQuery(arguments);
    EXPECT_NE(result.exit_code, 0) << name << ": " << result.stdout_text;
    EXPECT_EQ(result.stdout_text.find('{'), std::string::npos)
        << name << " produced JSON instead of a stable error";
    EXPECT_NE(result.stdout_text.find("veritas-query: "), std::string::npos)
        << name << ": " << result.stdout_text;
  }
}

TEST(VeritasQueryEvidenceTest, MissingStoreFailsBeforeEmittingJson) {
  const CliResult result = RunEvidenceOverflow(
      std::filesystem::path("/nonexistent/veritas-query-evidence"));
  EXPECT_NE(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text.find('{'), std::string::npos);
  EXPECT_NE(result.stdout_text.find("veritas-query: "), std::string::npos)
      << result.stdout_text;
}

}  // namespace
}  // namespace veritas::testing
