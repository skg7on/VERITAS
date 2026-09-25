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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "veritas/facts/RelationSchema.h"
#include "veritas/wpa/RelationIo.h"
#include "veritas/wpa/SouffleRunner.h"
#include "veritas/wpa/WpaComponent.h"

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

}  // namespace
}  // namespace veritas::wpa
