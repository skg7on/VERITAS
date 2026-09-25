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

#include "veritas/wpa/SouffleWpaExecutor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "WitnessKey.h"

#include "veritas/facts/RelationSchema.h"
#include "veritas/wpa/SouffleRunner.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

// The runner's status vocabulary, from SouffleRunner.h: it reports a relation
// the component's compiled program does not contain as "not found".
constexpr int kRunnerNotFound = 3;

// The one relation every bundle emits and the only one that is not a
// relations.v2 registry relation. Witness(result_key, rule_id, derivation_key,
// input_key, input_ordinal): the four keys are semantic keys built by the
// program's own functors, and the ordinal is the rule's argument position.
// Declared in logic/common/semantic_key.dl.
constexpr char kWitnessRelation[] = "Witness";
constexpr std::size_t kWitnessArity = 5;

// Releases the session on every exit path. VERITAS has no exceptions, so normal
// control flow is the only path that reaches this.
struct SessionCleanup {
  VeritasSouffleSession* session;
  ~SessionCleanup() { veritas_souffle_session_close(session); }
};

// --- EDB: semantic input rows into the session ------------------------------

// The ABI cell for one execution cell, in the flat form the runner expects.
//
// The kind comes from the column's schema domain -- kString is an interned
// symbol and every other domain is a number -- not from the cell's own
// alternative, because the runner checks the kind against the column's own
// primitive type (`unsigned` columns included, which a signed push aborts on).
// A column/cell disagreement therefore fails the insert instead of
// mis-encoding a fact.
//
// `text` owns the symbol a SYMBOL cell points at; the caller must keep it
// alive and unmoved.
StatusOr<VeritasSouffleCell> FlatCell(const facts::ExecutionCellValue& cell,
                                      facts::ColumnDomain domain,
                                      std::string* text) {
  VeritasSouffleCell flat{};
  if (domain == facts::ColumnDomain::kString) {
    const auto* symbol = std::get_if<std::string>(&cell);
    if (symbol == nullptr) {
      return Status::InvalidArgument("a symbol column carries a number");
    }
    *text = *symbol;
    flat.kind = VERITAS_SOUFFLE_CELL_SYMBOL;
    flat.symbol = text->c_str();
    return flat;
  }

  flat.kind = VERITAS_SOUFFLE_CELL_NUMBER;
  Status status = Status::Ok();
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
          status = Status::InvalidArgument("a numeric column carries a symbol");
        } else if constexpr (std::is_same_v<T, facts::FunctionId> ||
                             std::is_same_v<T, facts::ValueId> ||
                             std::is_same_v<T, facts::MemoryId> ||
                             std::is_same_v<T, facts::CallSiteId> ||
                             std::is_same_v<T, facts::FactId>) {
          flat.number = value.value;
        } else {
          // Signed, unsigned, and enum cells all cross as the 64-bit pattern
          // the runner narrows to the engine's own primitive type; the typed
          // semantic enums travel as their ordinal, which is the encoding
          // relations.v2.dl declares.
          flat.number = static_cast<unsigned long long>(value);
        }
      },
      cell);
  if (!status.ok()) {
    return status;
  }
  return flat;
}

// Inserts one relation's rows as a single batch.
//
// The logical input carries the whole run's EDB, while a component's compiled
// program contains only the relations its rules mention -- Souffle eliminates
// the rest -- so an EDB relation this component never uses has to be skipped,
// not inserted and not treated as a failure.
//
// An empty insert is the ABI's only relation-existence probe: the runner
// resolves the name before it looks at any row, so a batch of zero rows is
// refused with "not found" exactly when the program does not register the
// relation, and is otherwise accepted. That probe is the *only* place "not
// found" means "this component does not use this relation". A "not found" from
// the real insert below would mean the relation exists and vanished between two
// adjacent calls, so it is reported as the failure it is, as is any other
// non-zero status on either call.
Status InsertRelation(VeritasSouffleSession* session,
                      const facts::RelationSchema& schema,
                      const std::vector<const facts::ExecutionRow*>& rows) {
  const auto arity = static_cast<unsigned long long>(schema.columns.size());

  const int probe = veritas_souffle_session_insert(
      session, schema.name.c_str(), /*cells=*/nullptr, arity, /*row_count=*/0);
  if (probe == kRunnerNotFound) {
    return Status::Ok();
  }
  if (probe != 0) {
    return Status::Internal("Souffle session refused an empty batch for " +
                            schema.name + " with code " +
                            std::to_string(probe));
  }
  if (rows.empty()) {
    return Status::Ok();
  }

  // One flat, row-major buffer for the whole relation, plus the symbol store
  // its cells point into. The store is reserved to the full cell count first,
  // so no push_back can move a string a cell has already taken the address of.
  std::vector<VeritasSouffleCell> cells;
  std::vector<std::string> symbols;
  const std::size_t total = rows.size() * schema.columns.size();
  cells.reserve(total);
  symbols.reserve(total);

  for (const facts::ExecutionRow* row : rows) {
    if (row->cells.size() != schema.columns.size()) {
      return Status::InvalidArgument("execution row does not match its schema");
    }
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
      auto flat =
          FlatCell(row->cells[i], schema.columns[i].domain, &symbols.emplace_back());
      if (!flat.ok()) {
        return flat.status();
      }
      cells.push_back(*flat);
    }
  }

  const int status = veritas_souffle_session_insert(
      session, schema.name.c_str(), cells.data(), arity,
      static_cast<unsigned long long>(rows.size()));
  if (status != 0) {
    return Status::Internal("Souffle session refused " +
                            std::to_string(rows.size()) + " rows for " +
                            schema.name + " with code " +
                            std::to_string(status));
  }
  return Status::Ok();
}

// Inserts the logical input's EDB, one relation at a time. Grouping makes each
// relation a single batch and keeps the insert order independent of hashing.
Status InsertEdb(VeritasSouffleSession* session,
                 const WpaLogicalComponentInput& input) {
  std::map<facts::RelationId, std::vector<const facts::ExecutionRow*>> grouped;
  for (const facts::ExecutionRow& row : input.edb) {
    if (static_cast<std::size_t>(row.relation) >= facts::kRelationCountV2) {
      return Status::InvalidArgument("execution row names an unknown relation");
    }
    grouped[row.relation].push_back(&row);
  }

  for (const auto& [relation, rows] : grouped) {
    Status status =
        InsertRelation(session, facts::RelationsV2().Get(relation), rows);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

// --- Results: session cells back into semantic rows -------------------------

StatusOr<facts::SemanticRow> ResultRowFromCells(
    const WpaLogicalComponentInput& input, facts::RelationId relation,
    const VeritasSouffleCell* cells, unsigned long long arity) {
  const auto& schema = facts::RelationsV2().Get(relation);
  if (arity != schema.columns.size()) {
    return Status::InvalidArgument("result row does not match its schema");
  }

  facts::SemanticRow row;
  row.relation = relation;
  row.cells.reserve(schema.columns.size());
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    const facts::ColumnDomain domain = schema.columns[i].domain;
    const VeritasSouffleCell& cell = cells[i];
    if (domain == facts::ColumnDomain::kString) {
      if (cell.kind != VERITAS_SOUFFLE_CELL_SYMBOL || cell.symbol == nullptr) {
        return Status::InvalidArgument("a symbol column arrived as a number");
      }
      row.cells.push_back(std::string(cell.symbol));
      continue;
    }
    if (cell.kind != VERITAS_SOUFFLE_CELL_NUMBER) {
      return Status::InvalidArgument("a numeric column arrived as a symbol");
    }

    // Results arrive dense. A dense id means nothing outside its run, so every
    // one is mapped back through the input before it can become a fact.
    const auto dense = static_cast<std::uint32_t>(cell.number);
    switch (domain) {
    case facts::ColumnDomain::kFunctionId: {
      auto stable = input.mappings.functions.ToStable(facts::FunctionId{dense});
      if (!stable.ok()) {
        return stable.status();
      }
      row.cells.push_back(*stable);
      break;
    }
    case facts::ColumnDomain::kMemoryId: {
      auto stable = input.mappings.memories.ToStable(facts::MemoryId{dense});
      if (!stable.ok()) {
        return stable.status();
      }
      row.cells.push_back(*stable);
      break;
    }
    case facts::ColumnDomain::kValueId: {
      auto stable = input.mappings.values.ToStable(facts::ValueId{dense});
      if (!stable.ok()) {
        return stable.status();
      }
      row.cells.push_back(*stable);
      break;
    }
    case facts::ColumnDomain::kUint64:
      row.cells.push_back(static_cast<std::uint64_t>(cell.number));
      break;
    case facts::ColumnDomain::kEpistemic:
      row.cells.push_back(static_cast<sem::EpistemicState>(cell.number));
      break;
    default:
      // No derived relation of any component carries another domain, so a
      // result column that does is rule or schema drift. Coercing it into an
      // epistemic state -- which is what the file path's text reader did with
      // an unexpected domain -- would invent a fact instead of reporting one.
      return Status::InvalidArgument(
          "result column has a domain these rules cannot produce");
    }
  }

  auto valid = facts::ValidateSemanticRow(row);
  if (!valid.ok()) {
    return valid;
  }
  return row;
}

// The context one result scan collects into. The ABI sink cannot return a
// Status, so it carries the first failure out through `status` and stops the
// scan by returning non-zero.
struct ResultScan {
  const WpaLogicalComponentInput* input = nullptr;
  facts::RelationId relation{};
  std::vector<facts::SemanticRow>* rows = nullptr;
  Status status = Status::Ok();
};

int CollectResultRow(void* context, const VeritasSouffleCell* cells,
                     unsigned long long arity) {
  auto* scan = static_cast<ResultScan*>(context);
  auto row = ResultRowFromCells(*scan->input, scan->relation, cells, arity);
  if (!row.ok()) {
    scan->status = row.status();
    return 1;
  }
  scan->rows->push_back(std::move(*row));
  return 0;
}

// --- Witnesses: semantic keys back into semantic rows -----------------------

// Witness keys arrive as text, built by the program's own codec functors, so
// they are decoded against the schema each key names. That decode is not
// duplicated here: it lives in WitnessKey.cpp, shared with the file-backed
// reader, because a witness key is durable evidence and two copies of its
// decoder would be kept in step by a test rather than by the compiler.
struct WitnessScan {
  std::vector<facts::WitnessEdge>* edges = nullptr;
  Status status = Status::Ok();
};

int CollectWitnessRow(void* context, const VeritasSouffleCell* cells,
                      unsigned long long arity) {
  auto* scan = static_cast<WitnessScan*>(context);
  if (arity != kWitnessArity) {
    scan->status = Status::InvalidArgument("witness row must have five columns");
    return 1;
  }
  for (std::size_t i = 0; i < kWitnessArity - 1; ++i) {
    if (cells[i].kind != VERITAS_SOUFFLE_CELL_SYMBOL ||
        cells[i].symbol == nullptr) {
      scan->status =
          Status::InvalidArgument("witness key column arrived as a number");
      return 1;
    }
  }
  if (cells[4].kind != VERITAS_SOUFFLE_CELL_NUMBER) {
    scan->status =
        Status::InvalidArgument("witness ordinal arrived as a symbol");
    return 1;
  }

  auto result_row = RowFromKey(cells[0].symbol);
  if (!result_row.ok()) {
    scan->status = result_row.status();
    return 1;
  }
  auto input_row = RowFromKey(cells[3].symbol);
  if (!input_row.ok()) {
    scan->status = input_row.status();
    return 1;
  }

  scan->edges->push_back(facts::WitnessEdge{
      .result = facts::SemanticKey{std::move(*result_row)},
      .rule_id = cells[1].symbol,
      .derivation_key = cells[2].symbol,
      .input = facts::SemanticKey{std::move(*input_row)},
      .input_ordinal = static_cast<std::uint32_t>(cells[4].number)});
  return 0;
}

// --- Output: scan the component's own relations back out --------------------

// The cell kinds a relation's columns arrive as: its schema domain decides,
// exactly as it does on the way in.
std::vector<unsigned char> CellKindsFor(const facts::RelationSchema& schema) {
  std::vector<unsigned char> kinds;
  kinds.reserve(schema.columns.size());
  for (const auto& column : schema.columns) {
    kinds.push_back(column.domain == facts::ColumnDomain::kString
                        ? VERITAS_SOUFFLE_CELL_SYMBOL
                        : VERITAS_SOUFFLE_CELL_NUMBER);
  }
  return kinds;
}

Status ScanOutput(VeritasSouffleSession* session,
                  const WpaLogicalComponentInput& input,
                  facts::RawWpaEvaluation* raw) {
  // Every derived relation this component claims is an output of its program,
  // so the engine keeps it and a scan that cannot find it is a real failure
  // rather than a relation the rules dropped.
  for (const auto& domain : ComponentDomains(input.component)) {
    const auto& schema = facts::RelationsV2().Get(domain.derived);
    const std::vector<unsigned char> kinds = CellKindsFor(schema);
    ResultScan scan{&input, domain.derived, &raw->results};
    const int status = veritas_souffle_session_scan(
        session, schema.name.c_str(), kinds.data(), kinds.size(),
        CollectResultRow, &scan);
    if (status != 0) {
      return Status::Internal("Souffle session could not scan " +
                              schema.name + " with code " +
                              std::to_string(status));
    }
    if (!scan.status.ok()) {
      return scan.status;
    }
  }

  static constexpr std::array<unsigned char, kWitnessArity> kWitnessCellKinds = {
      VERITAS_SOUFFLE_CELL_SYMBOL, VERITAS_SOUFFLE_CELL_SYMBOL,
      VERITAS_SOUFFLE_CELL_SYMBOL, VERITAS_SOUFFLE_CELL_SYMBOL,
      VERITAS_SOUFFLE_CELL_NUMBER};
  WitnessScan witness_scan{&raw->witnesses};
  const int status = veritas_souffle_session_scan(
      session, kWitnessRelation, kWitnessCellKinds.data(), kWitnessCellKinds.size(),
      CollectWitnessRow, &witness_scan);
  if (status != 0) {
    return Status::Internal("Souffle session could not scan Witness with code " +
                            std::to_string(status));
  }
  return witness_scan.status;
}

}  // namespace

SouffleWpaExecutor::SouffleWpaExecutor(std::filesystem::path /*worker*/,
                                       std::string toolchain_identity)
    : toolchain_identity_(std::move(toolchain_identity)) {}

facts::EngineIdentity SouffleWpaExecutor::identity() const {
  return facts::EngineIdentity::kSouffle;
}

std::string_view SouffleWpaExecutor::toolchain_identity() const {
  return toolchain_identity_;
}

StatusOr<facts::RawWpaEvaluation> SouffleWpaExecutor::Execute(
    const WpaExecutionEnvelope& input, const WpaExecutionLimits& limits) const {
  if (input.run.engine != facts::EngineIdentity::kSouffle) {
    return Status::InvalidArgument(
        "envelope engine identity does not match this executor");
  }
  if (input.run.engine_toolchain_identity != toolchain_identity_) {
    return Status::InvalidArgument(
        "envelope toolchain identity does not match this executor");
  }
  if (limits.threads != 1) {
    return Status::InvalidArgument(
        "the Souffle WPA executor requires exactly one worker thread");
  }
  // The per-component timeout and memory limits were enforced by the subprocess
  // worker; the in-process runner cannot kill or resource-limit the host, so
  // they are intentionally ignored here.

  // The component runs in memory: the EDB is handed over as cells, the program
  // is evaluated with I/O disabled, and the relations this component owns are
  // scanned back out. No directory, no relation file, no text round trip.
  const std::string component(ComponentKindName(input.logical.component));
  VeritasSouffleSession* session = nullptr;
  const int open_status = veritas_souffle_session_open(
      component.c_str(), static_cast<unsigned>(limits.threads), &session);
  if (open_status != 0 || session == nullptr) {
    return Status::Internal("Souffle session did not open for component " +
                            component + " with code " +
                            std::to_string(open_status));
  }
  SessionCleanup cleanup{session};

  Status inserted = InsertEdb(session, input.logical);
  if (!inserted.ok()) {
    return inserted;
  }

  const int run_status = veritas_souffle_session_run(session);
  if (run_status != 0) {
    return Status::Internal("Souffle evaluation failed with code " +
                            std::to_string(run_status));
  }

  facts::RawWpaEvaluation raw;
  Status scanned = ScanOutput(session, input.logical, &raw);
  if (!scanned.ok()) {
    return scanned;
  }
  return raw;
}

}  // namespace veritas::wpa
