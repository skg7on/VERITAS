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

// SouffleSessionTest.cpp — the in-memory session ABI in SouffleRunner.h.
//
// The session is the file-free execution path: open a component, insert the
// EDB rows, run once, scan the derived relations back out. These tests pin the
// two properties the executor depends on:
//
//   1. the session returns the cells the file-backed one-shot path writes for
//      the same input, cell for cell and byte for byte; and
//   2. every cell kind the relations.v2 schema declares round-trips — an
//      interned symbol, a signed number, and an `unsigned` column, which the
//      engine represents separately from `number` and which a signed push
//      would abort on in a Debug build.
//
// Cells reach the session through the same schema-driven conversion the
// executor will use (ExecutionRow -> schema ColumnDomain ->
// VeritasSouffleCellKind), so a kind the runner gets wrong fails here rather
// than silently mis-encoding facts.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/RelationIo.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SouffleRunner.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaComponent.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

namespace fs = std::filesystem;
namespace sem = analysis::semantic;

// The cell kinds of "Witness", which is not a relations.v2 registry relation:
// RelationIo decodes it against the bundle's own declaration instead. Taken
// from logic/common/semantic_key.dl, which declares it as
// (result_key:symbol, rule_id:symbol, derivation_key:symbol, input_key:symbol,
//  input_ordinal:unsigned).
const std::vector<unsigned char> kWitnessKinds = {
    VERITAS_SOUFFLE_CELL_SYMBOL, VERITAS_SOUFFLE_CELL_SYMBOL,
    VERITAS_SOUFFLE_CELL_SYMBOL, VERITAS_SOUFFLE_CELL_SYMBOL,
    VERITAS_SOUFFLE_CELL_NUMBER};

// One scanned row, keeping both encodings so an assertion can name the cells
// it checked rather than only their count.
struct ScannedRow {
  std::vector<unsigned char> kinds;
  std::vector<unsigned long long> numbers;
  std::vector<std::string> symbols;
};

int Collect(void* context, const VeritasSouffleCell* cells,
            unsigned long long arity) {
  auto* rows = static_cast<std::vector<ScannedRow>*>(context);
  ScannedRow row;
  for (unsigned long long i = 0; i < arity; ++i) {
    const char* symbol = cells[i].symbol == nullptr ? "" : cells[i].symbol;
    row.kinds.push_back(cells[i].kind);
    row.numbers.push_back(cells[i].number);
    row.symbols.push_back(symbol);
  }
  rows->push_back(std::move(row));
  return 0;
}

// The cell kinds a relation's columns declare, in the ABI's tag space:
// kString is an interned symbol and every other domain is a number. Derived
// from the schema rather than transcribed, so the test and the executor cannot
// drift.
std::vector<unsigned char> CellKindsFor(std::string_view relation) {
  std::vector<unsigned char> kinds;
  const auto id = facts::RelationsV2().FindByName(relation);
  if (!id.has_value()) {
    return kinds;
  }
  for (const auto& column : facts::RelationsV2().Get(*id).columns) {
    kinds.push_back(column.domain == facts::ColumnDomain::kString
                        ? VERITAS_SOUFFLE_CELL_SYMBOL
                        : VERITAS_SOUFFLE_CELL_NUMBER);
  }
  return kinds;
}

// One execution row as the session's flat cell buffer. `row` must outlive the
// returned cells: a symbol cell points into the row's own string.
std::vector<VeritasSouffleCell> CellsFromRow(const facts::ExecutionRow& row) {
  std::vector<VeritasSouffleCell> cells;
  for (const auto& cell : row.cells) {
    VeritasSouffleCell flat{};
    flat.kind = VERITAS_SOUFFLE_CELL_NUMBER;
    std::visit(
        [&flat](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, std::string>) {
            flat.kind = VERITAS_SOUFFLE_CELL_SYMBOL;
            flat.symbol = value.c_str();
          } else if constexpr (std::is_same_v<T, facts::FunctionId> ||
                               std::is_same_v<T, facts::ValueId> ||
                               std::is_same_v<T, facts::MemoryId> ||
                               std::is_same_v<T, facts::CallSiteId> ||
                               std::is_same_v<T, facts::FactId>) {
            flat.number = value.value;
          } else {
            // The typed semantic enums travel as their ordinal, which is the
            // encoding relations.v2.dl declares.
            flat.number = static_cast<unsigned long long>(value);
          }
        },
        cell);
    cells.push_back(flat);
  }
  return cells;
}

// Inserts every row of one relation, deriving the cell kinds from the schema.
void InsertRows(VeritasSouffleSession* session,
                const std::vector<facts::ExecutionRow>& rows) {
  for (const auto& row : rows) {
    const std::string name = facts::RelationsV2().Get(row.relation).name;
    const std::vector<VeritasSouffleCell> cells = CellsFromRow(row);
    ASSERT_EQ(veritas_souffle_session_insert(session, name.c_str(),
                                             cells.data(), cells.size(), 1),
              0)
        << "inserting into " << name;
  }
}

// Scans a relation whose column kinds the caller states.
std::vector<ScannedRow> Scan(VeritasSouffleSession* session,
                             const char* relation,
                             const std::vector<unsigned char>& kinds) {
  std::vector<ScannedRow> rows;
  EXPECT_EQ(veritas_souffle_session_scan(session, relation, kinds.data(),
                                         kinds.size(), Collect, &rows),
            0)
      << "scanning " << relation;
  return rows;
}

fs::path MakeUniqueDirectory(std::string_view slug) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto stamp = now.count();
  const auto pid = static_cast<std::uintmax_t>(::getpid());
  std::string name = "veritas-souffle-session-";
  name.append(slug);
  name.append("-").append(std::to_string(pid)).append("-");
  name.append(std::to_string(stamp));
  const fs::path path = fs::temp_directory_path() / name;
  std::error_code error;
  fs::create_directories(path, error);
  EXPECT_FALSE(error) << error.message();
  return path;
}

std::vector<std::string> SplitTab(std::string_view line) {
  std::vector<std::string> cells;
  std::size_t start = 0;
  while (true) {
    const std::size_t next = line.find('\t', start);
    if (next == std::string_view::npos) {
      cells.emplace_back(line.substr(start));
      return cells;
    }
    cells.emplace_back(line.substr(start, next - start));
    start = next + 1;
  }
}

// The tab-separated text of a scanned row: the form the file-backed path writes
// the same cells in.
std::vector<std::string> RowText(const ScannedRow& row) {
  std::vector<std::string> cells;
  for (std::size_t i = 0; i < row.kinds.size(); ++i) {
    if (row.kinds[i] == VERITAS_SOUFFLE_CELL_SYMBOL) {
      cells.push_back(row.symbols[i]);
    } else {
      cells.push_back(std::to_string(row.numbers[i]));
    }
  }
  return cells;
}

std::vector<std::vector<std::string>> ReadRelationFile(const fs::path& file) {
  std::ifstream stream(file);
  EXPECT_TRUE(stream.is_open()) << file.string();
  std::vector<std::vector<std::string>> rows;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    rows.push_back(SplitTab(line));
  }
  return rows;
}

// The EDB of one reachability component execution: one direct call from dense
// function 1 to dense function 2, and the identity maps the semantic-key rules
// resolve dense ids through.
std::vector<facts::ExecutionRow> ReachabilityEdb() {
  using facts::ExecutionRow;
  using facts::RelationId;
  return {
      ExecutionRow{RelationId::kFunctionMap,
                   {facts::FunctionId{1}, std::string("fn:sha256:1")}},
      ExecutionRow{RelationId::kFunctionMap,
                   {facts::FunctionId{2}, std::string("fn:sha256:2")}},
      ExecutionRow{RelationId::kCallSiteMap,
                   {facts::CallSiteId{1}, std::string("cs:sha256:1")}},
      ExecutionRow{RelationId::kDirectCall,
                   {facts::CallSiteId{1}, facts::FunctionId{1},
                    facts::FunctionId{2}, sem::DispatchKind::kDirect,
                    sem::EpistemicState::kMust}},
  };
}

TEST(SouffleSessionTest, InsertRunScanReturnsTheDerivedRows) {
  VeritasSouffleSession* session = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("reachability", 1, &session), 0);
  ASSERT_NE(session, nullptr);

  InsertRows(session, ReachabilityEdb());
  ASSERT_EQ(veritas_souffle_session_run(session), 0);

  // ReachableCall(f, g, e) :- DirectCall(_, f, g, _, e) derives exactly one row
  // from the single call edge: the callee is reachable at the edge's warrant.
  const std::vector<unsigned char> kinds = CellKindsFor("ReachableCall");
  ASSERT_EQ(kinds.size(), 3u);
  const std::vector<ScannedRow> rows = Scan(session, "ReachableCall", kinds);
  ASSERT_EQ(rows.size(), 1u);

  EXPECT_EQ(rows[0].kinds, kinds);
  EXPECT_EQ(rows[0].numbers,
            (std::vector<unsigned long long>{1, 2, 0}));  // 1 -> 2, MUST
  EXPECT_EQ(rows[0].symbols, (std::vector<std::string>{"", "", ""}));

  veritas_souffle_session_close(session);
}

TEST(SouffleSessionTest, ScanReportsSymbolAndUnsignedColumnsAsDeclared) {
  VeritasSouffleSession* session = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("reachability", 1, &session), 0);
  ASSERT_NE(session, nullptr);

  InsertRows(session, ReachabilityEdb());
  ASSERT_EQ(veritas_souffle_session_run(session), 0);

  // The direct-call witness names its result and its input by semantic key, so
  // its first four columns arrive as symbols and only the ordinal is a number.
  // The ordinal is declared `unsigned`, so reading it as a signed number would
  // trip the engine's own element-type assertion.
  const std::vector<ScannedRow> rows = Scan(session, "Witness", kWitnessKinds);
  ASSERT_EQ(rows.size(), 1u);

  EXPECT_EQ(rows[0].kinds, kWitnessKinds);
  EXPECT_EQ(rows[0].symbols[1], "wpa.reachability.direct.v2");
  // Witness(rk, rule, ik, ik, 0): the derivation key of a single-input firing
  // is the input key itself.
  EXPECT_FALSE(rows[0].symbols[0].empty());
  EXPECT_EQ(rows[0].symbols[2], rows[0].symbols[3]);
  EXPECT_EQ(rows[0].numbers[4], 0u);

  veritas_souffle_session_close(session);
}

TEST(SouffleSessionTest, SessionAgreesWithTheFileBackedPath) {
  const std::vector<facts::ExecutionRow> edb = ReachabilityEdb();

  // The session path: the same rows, handed over as cells.
  VeritasSouffleSession* session = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("reachability", 1, &session), 0);
  ASSERT_NE(session, nullptr);
  InsertRows(session, edb);
  ASSERT_EQ(veritas_souffle_session_run(session), 0);
  const std::vector<unsigned char> result_kinds = CellKindsFor("ReachableCall");
  ASSERT_EQ(result_kinds.size(), 3u);
  const std::vector<ScannedRow> session_results =
      Scan(session, "ReachableCall", result_kinds);
  const std::vector<ScannedRow> session_witnesses =
      Scan(session, "Witness", kWitnessKinds);

  // The file-backed path: the same rows, through a directory.
  const fs::path directory = MakeUniqueDirectory("differential");
  const fs::path input_dir = directory / "in";
  const fs::path output_dir = directory / "out";
  std::error_code error;
  fs::create_directories(input_dir, error);
  ASSERT_FALSE(error) << error.message();
  fs::create_directories(output_dir, error);
  ASSERT_FALSE(error) << error.message();

  WpaLogicalComponentInput input;
  input.component = WpaComponentKind::kReachability;
  input.edb = edb;
  ASSERT_TRUE(RelationIo::WriteInput(input_dir, input).ok());
  ASSERT_EQ(veritas_souffle_run("reachability", input_dir.c_str(),
                                output_dir.c_str(), 1),
            0);

  const std::vector<std::vector<std::string>> file_results =
      ReadRelationFile(output_dir / "ReachableCall.csv");
  const std::vector<std::vector<std::string>> file_witnesses =
      ReadRelationFile(output_dir / "Witness.csv");

  // Non-vacuity: both paths must have produced rows before they are compared.
  ASSERT_FALSE(file_results.empty());
  ASSERT_FALSE(file_witnesses.empty());
  ASSERT_EQ(session_results.size(), file_results.size());
  ASSERT_EQ(session_witnesses.size(), file_witnesses.size());
  for (std::size_t i = 0; i < session_results.size(); ++i) {
    EXPECT_EQ(RowText(session_results[i]), file_results[i]);
  }
  for (std::size_t i = 0; i < session_witnesses.size(); ++i) {
    EXPECT_EQ(RowText(session_witnesses[i]), file_witnesses[i]);
  }

  veritas_souffle_session_close(session);
  fs::remove_all(directory, error);
}

TEST(SouffleSessionTest, UnsignedSizeColumnsRoundTripInMemoryEffects) {
  VeritasSouffleSession* session = nullptr;
  ASSERT_EQ(veritas_souffle_session_open("memory-effects", 1, &session), 0);
  ASSERT_NE(session, nullptr);

  // DirectRead(function_id, memory_id, range_kind, offset, size, epistemic).
  // `size` is declared `unsigned`, which the engine represents as its own
  // primitive type: pushing it as a signed number aborts the engine's own
  // element-type assertion rather than quietly widening.
  const std::vector<facts::ExecutionRow> edb = {
      {facts::RelationId::kDirectRead,
       {facts::FunctionId{1}, facts::MemoryId{1}, sem::ByteRangeKind::kKnown,
        std::int64_t{8}, std::uint64_t{16}, sem::EpistemicState::kMust}}};
  InsertRows(session, edb);
  ASSERT_EQ(veritas_souffle_session_run(session), 0);

  // MayRead(f, x, e) :- DirectRead(f, x, _, _, _, e).
  const std::vector<unsigned char> kinds = CellKindsFor("MayRead");
  ASSERT_EQ(kinds.size(), 3u);
  const std::vector<ScannedRow> rows = Scan(session, "MayRead", kinds);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].numbers, (std::vector<unsigned long long>{1, 1, 0}));

  veritas_souffle_session_close(session);
}

TEST(SouffleSessionTest, OpeningAnUnknownComponentFails) {
  VeritasSouffleSession* session = nullptr;
  EXPECT_NE(veritas_souffle_session_open("not-a-component", 1, &session), 0);
  EXPECT_EQ(session, nullptr);
}

// --- The executor's pinned baseline -----------------------------------------
//
// The tests below add a second, independent check of the switch-over: they run
// a fixture component through SouffleWpaExecutor::Execute and compare the whole
// raw evaluation against a row dump captured from the *directory-backed*
// implementation, before the executor moved onto the session.
//
// The dump is a literal, so it cannot drift with the implementation that
// produced it: the same test passed against the old executor and must keep
// passing against the new one. Rows are sorted because the engine's iteration
// order is not part of the contract; the relation of every row, every cell, and
// every witness edge are.

namespace v1 = summary::v1;
namespace v2 = summary::v2;

core::StableId FixtureFunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId FixtureCallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId FixtureMemoryId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kMemoryRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId FixtureValueId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kValueRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

v2::FunctionSummary FixtureSummary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FixtureFunctionId(name)));
  return summary;
}

// A call edge; `resolved` false leaves the callee unresolved, which is how the
// fixture puts a row into UnknownCall.
void AddFixtureCall(v2::FunctionSummary* summary, std::string_view from,
                    std::string_view to, bool resolved) {
  auto* call = summary->add_calls();
  call->set_call_site_id(core::ToString(
      FixtureCallSiteId(std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  if (resolved) {
    call->set_resolved_callee_function_variant_id(
        core::ToString(FixtureFunctionId(to)));
  }
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

void AddFixtureMemoryEffect(v2::FunctionSummary* summary,
                            std::string_view memory, bool is_write,
                            bool known_range, std::int64_t offset,
                            std::uint64_t size) {
  auto* effect = summary->add_memory_effects();
  effect->set_kind(is_write ? v1::EFFECT_KIND_WRITE : v1::EFFECT_KIND_READ);
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:memory");
  auto* location = effect->mutable_location();
  location->set_memory_location_id(core::ToString(FixtureMemoryId(memory)));
  auto* range = location->mutable_byte_range();
  range->set_offset_known(known_range);
  range->set_offset(offset);
  range->set_size_known(known_range);
  range->set_size(size);
}

void AddFixtureValueFlow(v2::FunctionSummary* summary, std::string_view source,
                         std::string_view destination) {
  auto* flow = summary->add_value_flows();
  flow->set_source_value_id(core::ToString(FixtureValueId(source)));
  flow->set_destination_value_id(core::ToString(FixtureValueId(destination)));
  flow->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  flow->set_provenance_ref("test:flow");
}

// The fixture program, chosen so that every path the executor has is taken at
// least once:
//
//   * f and g are mutually recursive, so they share one SCC and reaching h from
//     f needs the local transitive rule rather than a direct edge;
//   * g calls "ext", which has no summary, and f has a call the summary cannot
//     resolve, so the component's EDB carries UnmodeledExternal and UnknownCall
//     rows -- relations the reachability program does not register;
//   * f writes a known range and reads an unknown one, g reads the same memory,
//     so the memory component derives both of its relations through a direct
//     and a transitive step, and the EDB carries an `unsigned` size cell;
//   * f flows a value, which only the flow component consumes;
//   * the unresolved call makes the effects component derive both
//     UnknownEffect and its SoundnessCoverage certificate, whose cells include
//     one of every domain a derived relation can carry.
std::vector<summary::SummaryArtifact> BaselineFixtureProgram() {
  auto f = FixtureSummary("f");
  AddFixtureCall(&f, "f", "g", /*resolved=*/true);
  AddFixtureCall(&f, "f", "pthread_mutex_lock", /*resolved=*/false);
  AddFixtureMemoryEffect(&f, "m1", /*is_write=*/true, /*known_range=*/true, 8,
                         16);
  AddFixtureMemoryEffect(&f, "m2", /*is_write=*/false, /*known_range=*/false, 0,
                         0);
  AddFixtureValueFlow(&f, "v1", "v2");

  auto g = FixtureSummary("g");
  AddFixtureCall(&g, "g", "f", /*resolved=*/true);
  AddFixtureCall(&g, "g", "h", /*resolved=*/true);
  AddFixtureCall(&g, "g", "ext", /*resolved=*/true);
  AddFixtureMemoryEffect(&g, "m2", /*is_write=*/false, /*known_range=*/false, 0,
                         0);

  return {f, g, FixtureSummary("h")};
}

StatusOr<WpaLogicalComponentInput> BaselineFixtureInput(
    const std::vector<summary::SummaryArtifact>& artifacts,
    WpaComponentKind component, std::string_view root) {
  auto graph = CallGraph::FromSummaries(artifacts);
  if (!graph.ok()) {
    return graph.status();
  }
  auto scc = SccGraph::Build(*graph);
  if (!scc.ok()) {
    return scc.status();
  }
  auto scc_id = scc->SccForFunction(FixtureFunctionId(root));
  if (!scc_id.ok()) {
    return scc_id.status();
  }

  WpaMaterializationRequest request;
  request.semantics.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  request.semantics.summary_schema_version = "summary.v2";
  request.semantics.relation_schema_version = "relations.v2";
  request.semantics.rule_bundle_version = "rules.v2";
  request.semantics.model_bundle_version = "models.v1";
  request.semantics.svf_configuration_hash = std::string(64, 'a');
  request.semantics.wpa_configuration_hash = std::string(64, 'b');
  request.scc_id = *scc_id;
  request.component = component;
  request.summaries = artifacts;
  return WpaInputMaterializer::Build(request);
}

// One fixture component through the production executor, exactly as the
// orchestrator drives it.
StatusOr<facts::RawWpaEvaluation> ExecuteBaselineFixture(
    WpaComponentKind component, std::string_view root) {
  auto logical =
      BaselineFixtureInput(BaselineFixtureProgram(), component, root);
  if (!logical.ok()) {
    return logical.status();
  }

  facts::AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev", 3)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = facts::EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = "souffle-toolchain";
  auto manifest = facts::MakeAnalysisRun(descriptor);
  if (!manifest.ok()) {
    return manifest.status();
  }

  WpaExecutionEnvelope envelope{*manifest, std::move(*logical)};
  // The worker path is accepted for API compatibility and unused: the engine is
  // linked in-process, so the executor never starts a subprocess.
  const SouffleWpaExecutor executor(std::filesystem::path{}, "souffle-toolchain");
  return executor.Execute(envelope, WpaExecutionLimits{std::chrono::seconds(30), 0, 1});
}

std::string RenderCell(const facts::SemanticCellValue& cell) {
  return std::visit(
      [](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          return core::ToString(value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return value;
        } else if constexpr (std::is_same_v<T, std::int64_t> ||
                             std::is_same_v<T, std::uint64_t>) {
          return std::to_string(value);
        } else {
          // The typed semantic enums travel as their ordinal.
          return std::to_string(static_cast<int>(value));
        }
      },
      cell);
}

// A whole raw evaluation as deterministic text: one line per result row and one
// per witness edge, sorted, so the dump does not depend on the engine's
// iteration order.
std::string RenderRowsSorted(const facts::RawWpaEvaluation& raw) {
  std::vector<std::string> lines;
  lines.reserve(raw.results.size() + raw.witnesses.size());
  for (const auto& row : raw.results) {
    std::string line = "result ";
    line += facts::RelationsV2().Get(row.relation).name;
    for (const auto& cell : row.cells) {
      line.push_back('|');
      line += RenderCell(cell);
    }
    lines.push_back(std::move(line));
  }
  for (const auto& edge : raw.witnesses) {
    std::string line = "witness ";
    line += edge.rule_id;
    line += " ord=";
    line += std::to_string(edge.input_ordinal);
    line += " result=";
    line += facts::EncodeSemanticKey(edge.result.row);
    line += " input=";
    line += facts::EncodeSemanticKey(edge.input.row);
    line += " derivation=";
    line += edge.derivation_key;
    lines.push_back(std::move(line));
  }
  std::sort(lines.begin(), lines.end());
  std::string dump;
  for (const auto& line : lines) {
    dump += line;
    dump.push_back('\n');
  }
  return dump;
}


// Splits a dump into its rows, without a trailing empty line for the trailing
// newline every dump ends with.
std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t next = text.find('\n', start);
    if (next == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, next - start));
    start = next + 1;
  }
  return lines;
}

// Compares a captured dump with the current one and reports the first row that
// differs, so a regression names the row it changed instead of burying the
// difference in a one-thousand-character line.
void ExpectSameRows(const std::string& expected, const std::string& actual,
                    std::string_view label) {
  const std::vector<std::string> expected_lines = SplitLines(expected);
  const std::vector<std::string> actual_lines = SplitLines(actual);
  if (expected_lines.size() != actual_lines.size()) {
    ADD_FAILURE() << label << ": captured " << expected_lines.size()
                  << " rows, evaluated " << actual_lines.size();
    return;
  }
  for (std::size_t i = 0; i < expected_lines.size(); ++i) {
    if (expected_lines[i] != actual_lines[i]) {
      ADD_FAILURE() << label << ": row " << i << " differs\ncaptured: "
                    << expected_lines[i] << "\nactual:   " << actual_lines[i];
      return;
    }
  }
}

// --- The executor's pinned baseline -----------------------------------------
//
// The tests below are the switch-over check for this round. Each one runs the
// fixture component through SouffleWpaExecutor::Execute -- the production
// path, with the production engine and toolchain identity -- and compares the
// whole raw evaluation against a dump captured from the *directory-backed*
// implementation, before the executor moved onto the session.
//
// The dumps are literals captured from that earlier binary, not transcriptions
// of the new one, so the same test passed against the old executor and must
// keep passing against the sessional one: every result row with every cell, and
// every witness edge with its rule, ordinal, both semantic keys, and its
// derivation key. Rows are sorted because the engine's iteration order is not
// part of the contract.

// Captured from the directory-backed executor before the switch.
const std::string kCapturedReachabilityRows =
    R"ROWS(result ReachableCall|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|0
result ReachableCall|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14de|0
result ReachableCall|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123|0
result ReachableCall|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|0
result ReachableCall|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|0
result ReachableCall|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14de|0
result ReachableCall|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123|0
result ReachableCall|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|0
witness wpa.reachability.direct.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0
witness wpa.reachability.direct.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0
witness wpa.reachability.direct.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:0f110c571c8f6f70006347bcb85325962c5272f727420a067fc75221eb6534faI79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:0f110c571c8f6f70006347bcb85325962c5272f727420a067fc75221eb6534faI79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0E1:0
witness wpa.reachability.direct.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:8fbbc6593b1a58e22b595992fca98ae77b9d335f9bfa9c75ab2b93927de3af77I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:8fbbc6593b1a58e22b595992fca98ae77b9d335f9bfa9c75ab2b93927de3af77I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0
witness wpa.reachability.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:aaa9402664f1a41f40ebbc52c9993eb66aeb366602958fdfaa283b71e64db123E1:0
witness wpa.reachability.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 input=veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:ReachableCallU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0
)ROWS";
// Captured from the directory-backed executor before the switch.
const std::string kCapturedMemoryEffectsRows =
    R"ROWS(result MayRead|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13d|0
result MayRead|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13d|0
result MayWrite|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619|0
result MayWrite|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619|0
witness wpa.memory.may_read.direct.v2 ord=0 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S10:DirectReadU1:6I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:1N1:0U1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectReadU1:6I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:1N1:0U1:0E1:0
witness wpa.memory.may_read.direct.v2 ord=0 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S10:DirectReadU1:6I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:1N1:0U1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectReadU1:6I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:1N1:0U1:0E1:0
witness wpa.memory.may_read.transitive.v2 ord=0 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0
witness wpa.memory.may_read.transitive.v2 ord=0 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0
witness wpa.memory.may_read.transitive.v2 ord=1 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0
witness wpa.memory.may_read.transitive.v2 ord=1 result=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 input=veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S7:MayReadU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:29c1b289e7522195b362e44f54e05470b69ad20540ab60a18a05e5bf6951f13dE1:0
witness wpa.memory.may_write.direct.v2 ord=0 result=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 input=veritas.semantic-key.v1S11:DirectWriteU1:6I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0N1:8U2:16E1:0 derivation=veritas.semantic-key.v1S11:DirectWriteU1:6I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0N1:8U2:16E1:0
witness wpa.memory.may_write.transitive.v2 ord=0 result=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0
witness wpa.memory.may_write.transitive.v2 ord=0 result=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0
witness wpa.memory.may_write.transitive.v2 ord=1 result=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 input=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0
witness wpa.memory.may_write.transitive.v2 ord=1 result=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 input=veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S8:MayWriteU1:3I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:memref:sha256:ca0df2c95aa144c1d0ff2ff3c8f967fdc1de9ef0c4120b3726416701b519d619E1:0
)ROWS";
// Captured from the directory-backed executor before the switch.
const std::string kCapturedEffectsRows =
    R"ROWS(result SoundnessCoverage|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|dominating_check_absence|0|0
result SoundnessCoverage|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|dominating_check_absence|0|0
result UnknownEffect|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10|pthread_mutex_lock|0
result UnknownEffect|funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111|funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14de|unmodeled_external|1
result UnknownEffect|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10|pthread_mutex_lock|0
result UnknownEffect|funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29|funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14de|unmodeled_external|1
witness wpa.coverage.incomplete.v2 ord=0 result=veritas.semantic-key.v1S17:SoundnessCoverageU1:4S79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S24:dominating_check_absenceU1:0E1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 derivation=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.coverage.incomplete.v2 ord=0 result=veritas.semantic-key.v1S17:SoundnessCoverageU1:4S79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S24:dominating_check_absenceU1:0E1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 derivation=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
witness wpa.coverage.incomplete.v2 ord=0 result=veritas.semantic-key.v1S17:SoundnessCoverageU1:4S79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S24:dominating_check_absenceU1:0E1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 derivation=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.coverage.incomplete.v2 ord=0 result=veritas.semantic-key.v1S17:SoundnessCoverageU1:4S79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S24:dominating_check_absenceU1:0E1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 derivation=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
witness wpa.effect.unknown.call.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 input=veritas.semantic-key.v1S11:UnknownCallU1:4I80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S18:pthread_mutex_lockE1:0 derivation=veritas.semantic-key.v1S11:UnknownCallU1:4I80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S18:pthread_mutex_lockE1:0
witness wpa.effect.unknown.external.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:0f110c571c8f6f70006347bcb85325962c5272f727420a067fc75221eb6534faI79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:0f110c571c8f6f70006347bcb85325962c5272f727420a067fc75221eb6534faI79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deE1:0E1:0
witness wpa.effect.unknown.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.effect.unknown.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
witness wpa.effect.unknown.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.effect.unknown.transitive.v2 ord=0 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 input=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
witness wpa.effect.unknown.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.effect.unknown.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:94acb8f10bf1a831c1410b5472d3148b2a4830faf5206e5b02212aa4ea132bb4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
witness wpa.effect.unknown.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S79:funcvar:sha256:a7e7e2f59b128bdb0aa60f56f5211efefdf83b92994b8f4a5d2e18126a0a14deS18:unmodeled_externalE1:1
witness wpa.effect.unknown.transitive.v2 ord=1 result=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 input=veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0 derivation=veritas.semantic-key.v1S10:DirectCallU1:5I80:callsite:sha256:bcfdad373cd4bec4caa3df5b3bd25635804d07e8e760d2e61efb6bd7895acee8I79:funcvar:sha256:cd0aa9856147b6c5b4ff2b7dfee5da20aa38253099ef1b4a64aced233c9afe29I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111E1:0E1:0veritas.semantic-key.v1S13:UnknownEffectU1:4I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111S80:callsite:sha256:df0173f8979bda91e5e7a756b765753ef115b38da6ff5fb79991dcfbea1d5a10S18:pthread_mutex_lockE1:0
)ROWS";
// Captured from the directory-backed executor before the switch.
const std::string kCapturedFlowRows =
    R"ROWS(result GlobalFlow|valref:sha256:3bfc269594ef649228e9a74bab00f042efc91d5acc6fbee31a382e80d42388fe|valref:sha256:fb04dcb6970e4c3d1873de51fd5a50d7bb46b3383113602665c350ec40b5f990|0
witness wpa.flow.global.local.v2 ord=0 result=veritas.semantic-key.v1S10:GlobalFlowU1:3I78:valref:sha256:3bfc269594ef649228e9a74bab00f042efc91d5acc6fbee31a382e80d42388feI78:valref:sha256:fb04dcb6970e4c3d1873de51fd5a50d7bb46b3383113602665c350ec40b5f990E1:0 input=veritas.semantic-key.v1S9:LocalFlowU1:5I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:valref:sha256:3bfc269594ef649228e9a74bab00f042efc91d5acc6fbee31a382e80d42388feI78:valref:sha256:fb04dcb6970e4c3d1873de51fd5a50d7bb46b3383113602665c350ec40b5f990S5:localE1:0 derivation=veritas.semantic-key.v1S9:LocalFlowU1:5I79:funcvar:sha256:252f10c83610ebca1a059c0bae8255eba2f95be4d1d7bcfa89d7248a82d9f111I78:valref:sha256:3bfc269594ef649228e9a74bab00f042efc91d5acc6fbee31a382e80d42388feI78:valref:sha256:fb04dcb6970e4c3d1873de51fd5a50d7bb46b3383113602665c350ec40b5f990S5:localE1:0
)ROWS";

TEST(SouffleSessionTest, ExecutorRowsMatchTheCapturedReachabilityBaseline) {
  auto raw = ExecuteBaselineFixture(WpaComponentKind::kReachability, "f");
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  // Non-vacuity: a dump pinned against an empty evaluation would prove nothing.
  ASSERT_FALSE(raw->results.empty());
  ASSERT_FALSE(raw->witnesses.empty());
  ExpectSameRows(kCapturedReachabilityRows, RenderRowsSorted(*raw), "fixture kReachability");
}

TEST(SouffleSessionTest, ExecutorRowsMatchTheCapturedMemoryEffectsBaseline) {
  auto raw = ExecuteBaselineFixture(WpaComponentKind::kMemoryEffects, "f");
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  // Non-vacuity: a dump pinned against an empty evaluation would prove nothing.
  ASSERT_FALSE(raw->results.empty());
  ASSERT_FALSE(raw->witnesses.empty());
  ExpectSameRows(kCapturedMemoryEffectsRows, RenderRowsSorted(*raw), "fixture kMemoryEffects");
}

TEST(SouffleSessionTest, ExecutorRowsMatchTheCapturedEffectsBaseline) {
  auto raw = ExecuteBaselineFixture(WpaComponentKind::kEffects, "f");
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  // Non-vacuity: a dump pinned against an empty evaluation would prove nothing.
  ASSERT_FALSE(raw->results.empty());
  ASSERT_FALSE(raw->witnesses.empty());
  ExpectSameRows(kCapturedEffectsRows, RenderRowsSorted(*raw), "fixture kEffects");
}

TEST(SouffleSessionTest, ExecutorRowsMatchTheCapturedFlowBaseline) {
  auto raw = ExecuteBaselineFixture(WpaComponentKind::kFlow, "f");
  ASSERT_TRUE(raw.ok()) << raw.status().message();
  // Non-vacuity: a dump pinned against an empty evaluation would prove nothing.
  ASSERT_FALSE(raw->results.empty());
  ASSERT_FALSE(raw->witnesses.empty());
  ExpectSameRows(kCapturedFlowRows, RenderRowsSorted(*raw), "fixture kFlow");
}

TEST(SouffleSessionTest, TheFixtureCarriesRelationsItsComponentDoesNotRegister) {
  // Non-vacuity for the skip path, and the reason the executor probes at all. A
  // component's compiled program contains only the relations its rules mention
  // -- Souffle eliminates the rest -- while the logical input carries the whole
  // run's EDB. Three of the four fixture components therefore have EDB rows for
  // a relation their program has no such relation for; an executor that treated
  // that as a failure would fail most of the run rather than a corner of it.
  //
  // An empty insert is the ABI's relation probe: the runner resolves the name
  // before it looks at any row. The expectation below is a snapshot of what
  // each generated bundle eliminates today, not a contract the executor
  // imposes, so a legitimate rule-bundle change shows up here as a named
  // relation rather than as a mystery failure elsewhere.
  const std::vector<summary::SummaryArtifact> artifacts =
      BaselineFixtureProgram();
  const std::vector<std::pair<WpaComponentKind, std::string_view>> components = {
      {WpaComponentKind::kReachability, "reachability"},
      {WpaComponentKind::kMemoryEffects, "memory-effects"},
      {WpaComponentKind::kFlow, "flow"},
      {WpaComponentKind::kEffects, "effects"}};
  const std::map<std::string, std::vector<std::string>> expected = {
      {"reachability", {"UnmodeledExternal", "UnknownCall"}},
      {"memory-effects", {"UnmodeledExternal", "UnknownCall"}},
      {"flow", {"DirectCall", "UnknownCall", "UnmodeledExternal"}},
      {"effects", {}}};

  for (const auto& [component, name] : components) {
    auto logical = BaselineFixtureInput(artifacts, component, "f");
    ASSERT_TRUE(logical.ok()) << name << ": " << logical.status().message();

    VeritasSouffleSession* session = nullptr;
    ASSERT_EQ(
        veritas_souffle_session_open(std::string(name).c_str(), 1, &session),
        0);
    std::vector<std::string> unregistered;
    for (const auto& row : logical->edb) {
      const auto& schema = facts::RelationsV2().Get(row.relation);
      if (std::find(unregistered.begin(), unregistered.end(), schema.name) !=
          unregistered.end()) {
        continue;
      }
      if (veritas_souffle_session_insert(session, schema.name.c_str(), nullptr,
                                         schema.columns.size(), 0) == 3) {
        unregistered.push_back(schema.name);
      }
    }
    veritas_souffle_session_close(session);

    const auto found = expected.find(std::string(name));
    ASSERT_TRUE(found != expected.end());
    std::vector<std::string> expected_names = found->second;
    std::sort(unregistered.begin(), unregistered.end());
    std::sort(expected_names.begin(), expected_names.end());
    EXPECT_EQ(unregistered, expected_names)
        << name << " eliminates a different set of EDB relations than the "
                   "pinned baselines were captured against";
  }
}

}  // namespace
}  // namespace veritas::wpa
