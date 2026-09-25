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

// SouffleRunner.cpp — the in-process compiled-Souffle entry point.
//
// This is the only translation unit that instantiates the Souffle programs and
// therefore the only one that must be compiled with RTTI and exceptions. It is
// linked into a shared library and reached through the C ABI in
// SouffleRunner.h, so no Souffle type or exception crosses into VERITAS.
//
// It offers two forms of the same work. `veritas_souffle_run` writes the EDB to
// a directory, runs the program over it, and lets the program write its results
// back out as text. The session form does the work in memory: the caller hands
// rows over as flat cells, the program is evaluated with I/O disabled, and the
// caller scans the relations it wants back. Nothing on the session path touches
// the filesystem.

#include "veritas/wpa/SouffleRunner.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "souffle/SouffleInterface.h"

namespace {

// The status vocabulary shared with SouffleRunner.h.
constexpr int kOk = 0;
constexpr int kEngineFailure = 1;
constexpr int kInvalidArgument = 2;
constexpr int kNotFound = 3;
constexpr int kNotImplemented = 4;

// Maps a component name to the registered program name. These are the output
// filenames the build generates the bundles from (v2_reach.cpp /
// v2_memory_effects.cpp), sanitized by Souffle's synthesiser.
const char* ProgramNameForComponent(std::string_view component) {
  if (component == "reachability") return "v2_reach";
  if (component == "memory-effects") return "v2_memory_effects";
  if (component == "flow") return "v2_global_flow";
  if (component == "effects") return "v2_effects";
  return nullptr;
}

// Pushes one ABI cell onto a tuple under construction, in column order. The
// column's primitive type decides the engine's representation: `number` is
// signed, `unsigned` is not, and the engine asserts on a mismatch rather than
// coercing. RAM_DOMAIN_SIZE is 32 in this build, so the ABI's 64-bit cells are
// narrowed to the engine's RamSigned/RamUnsigned here.
//
// Returns kInvalidArgument when the cell's kind disagrees with the column's
// declared type, or when a symbol cell carries no text.
int PushCell(souffle::tuple* row, const souffle::Relation& relation,
             std::size_t column, const VeritasSouffleCell& cell) {
  const char primitive = relation.getAttrType(column)[0];
  if (cell.kind == VERITAS_SOUFFLE_CELL_SYMBOL) {
    if (primitive != 's' || cell.symbol == nullptr) {
      return kInvalidArgument;
    }
    *row << std::string(cell.symbol);
    return kOk;
  }
  if (cell.kind == VERITAS_SOUFFLE_CELL_NUMBER) {
    if (primitive == 'u') {
      *row << static_cast<souffle::RamUnsigned>(cell.number);
      return kOk;
    }
    if (primitive == 'i' || primitive == 'r' || primitive == '+') {
      *row << static_cast<souffle::RamSigned>(cell.number);
      return kOk;
    }
  }
  return kInvalidArgument;
}

// Reads one column of a relation row into an ABI cell. `kind` is the column's
// kind as the caller's schema declares it, so the runner never infers a
// column's type from its bytes; the engine's own primitive type is then checked
// against it, and a disagreement is an argument error rather than a wrong
// answer.
//
// `symbol` must outlive the cell it fills: the cell points at its buffer.
int ReadCell(souffle::tuple& row, const souffle::Relation& relation,
             std::size_t column, unsigned char kind, std::string* symbol,
             VeritasSouffleCell* cell) {
  const char primitive = relation.getAttrType(column)[0];
  cell->number = 0;
  cell->symbol = nullptr;

  if (kind == VERITAS_SOUFFLE_CELL_SYMBOL) {
    if (primitive != 's') {
      return kInvalidArgument;
    }
    row >> *symbol;
    cell->kind = VERITAS_SOUFFLE_CELL_SYMBOL;
    cell->symbol = symbol->c_str();
    return kOk;
  }
  if (kind == VERITAS_SOUFFLE_CELL_NUMBER) {
    cell->kind = VERITAS_SOUFFLE_CELL_NUMBER;
    if (primitive == 'u') {
      souffle::RamUnsigned value = 0;
      row >> value;
      cell->number = value;
      return kOk;
    }
    if (primitive == 'i' || primitive == 'r' || primitive == '+') {
      souffle::RamSigned value = 0;
      row >> value;
      // Numbers cross the ABI as a 64-bit two's-complement bit pattern, so a
      // negative cell survives the narrowing round trip.
      cell->number =
          static_cast<unsigned long long>(static_cast<std::int64_t>(value));
      return kOk;
    }
  }
  return kInvalidArgument;
}

}  // namespace

// One open component: the program instance plus the component it was opened
// for. The program owns every relation the component's rules use; Souffle
// eliminates relations the rules never mention, so those are absent from the
// relation map rather than present and empty.
struct VeritasSouffleSession {
  std::string component;
  std::unique_ptr<souffle::SouffleProgram> program;
};

int veritas_souffle_run(const char* component, const char* input_dir,
                        const char* output_dir, unsigned jobs) {
  const char* program_name = ProgramNameForComponent(component);
  if (program_name == nullptr) {
    return 2;
  }

  souffle::SouffleProgram* program =
      souffle::ProgramFactory::newInstance(program_name);
  if (program == nullptr) {
    return 3;
  }

  program->setNumThreads(jobs);
  try {
    program->runAll(input_dir, output_dir, /*performIO=*/true,
                    /*pruneImdtRels=*/true);
  } catch (const std::exception&) {
    delete program;
    return 1;
  }
  delete program;
  return 0;
}

int veritas_souffle_session_open(const char* component, unsigned jobs,
                                 VeritasSouffleSession** out_session) {
  if (component == nullptr || out_session == nullptr) {
    return kInvalidArgument;
  }
  *out_session = nullptr;

  const char* program_name = ProgramNameForComponent(component);
  if (program_name == nullptr) {
    return kInvalidArgument;
  }

  try {
    std::unique_ptr<souffle::SouffleProgram> program(
        souffle::ProgramFactory::newInstance(program_name));
    if (program == nullptr) {
      return kNotFound;
    }
    program->setNumThreads(jobs);

    auto session = std::make_unique<VeritasSouffleSession>();
    session->component = component;
    session->program = std::move(program);
    *out_session = session.release();
  } catch (const std::exception&) {
    return kEngineFailure;
  }
  return kOk;
}

int veritas_souffle_session_insert(VeritasSouffleSession* session,
                                   const char* relation,
                                   const VeritasSouffleCell* cells,
                                   unsigned long long arity,
                                   unsigned long long row_count) {
  if (session == nullptr || relation == nullptr) {
    return kInvalidArgument;
  }
  if (cells == nullptr && row_count != 0) {
    return kInvalidArgument;
  }
  souffle::Relation* target = session->program->getRelation(relation);
  if (target == nullptr) {
    return kNotFound;
  }
  if (arity != target->getArity()) {
    return kInvalidArgument;
  }

  try {
    // One tuple, rewound per row: insert copies its cells into the relation, so
    // there is no reason to allocate a tuple per row.
    souffle::tuple row(target);
    for (unsigned long long r = 0; r < row_count; ++r) {
      row.rewind();
      for (unsigned long long c = 0; c < arity; ++c) {
        const int status =
            PushCell(&row, *target, static_cast<std::size_t>(c),
                     cells[r * arity + c]);
        if (status != kOk) {
          return status;
        }
      }
      target->insert(row);
    }
  } catch (const std::exception&) {
    return kEngineFailure;
  }
  return kOk;
}

int veritas_souffle_session_run(VeritasSouffleSession* session) {
  if (session == nullptr) {
    return kInvalidArgument;
  }
  try {
    session->program->runAll(/*inputDirectory=*/"", /*outputDirectory=*/"",
                             /*performIO=*/false, /*pruneImdtRels=*/true);
  } catch (const std::exception&) {
    return kEngineFailure;
  }
  return kOk;
}

int veritas_souffle_session_scan(VeritasSouffleSession* session,
                                 const char* relation,
                                 const unsigned char* cell_kinds,
                                 unsigned long long arity,
                                 VeritasSouffleRowSink sink, void* context) {
  if (session == nullptr || relation == nullptr || cell_kinds == nullptr ||
      sink == nullptr) {
    return kInvalidArgument;
  }
  souffle::Relation* target = session->program->getRelation(relation);
  if (target == nullptr) {
    return kNotFound;
  }
  if (arity != target->getArity()) {
    return kInvalidArgument;
  }

  try {
    const std::size_t width = target->getArity();
    // One cell per column, plus one symbol buffer per column so that two symbol
    // columns in the same row cannot share (and overwrite) one buffer.
    std::vector<VeritasSouffleCell> cells(width);
    std::vector<std::string> symbols(width);
    for (souffle::tuple& row : *target) {
      for (std::size_t column = 0; column < width; ++column) {
        const int status =
            ReadCell(row, *target, column, cell_kinds[column],
                     &symbols[column], &cells[column]);
        if (status != kOk) {
          return status;
        }
      }
      if (sink(context, cells.data(), arity) != 0) {
        break;  // The caller asked to stop; that is not a failure.
      }
    }
  } catch (const std::exception&) {
    return kEngineFailure;
  }
  return kOk;
}

int veritas_souffle_session_reset(VeritasSouffleSession* session) {
  if (session == nullptr) {
    return kInvalidArgument;
  }
  // A deliberate standing decision, not pending work: the design that would
  // have called this (one program instance reused across a run's components,
  // reset between them) was measured at 0.218 s across all 13,716 components --
  // 0.037 % of the run -- and dropped (design spec sections 3.2, 7.2, 9.6).
  // The purge that makes reuse safe (purgeInputRelations, purgeOutputRelations,
  // purgeInternalRelations) has to be proven indistinguishable from a fresh
  // instance, and it is unverified in the pinned revision, so it is not guessed
  // at here. The stub is kept, and keeps reporting failure, because a caller
  // that reached it believing it worked would be reasoning about a session that
  // was never actually reset.
  return kNotImplemented;
}

void veritas_souffle_session_close(VeritasSouffleSession* session) {
  delete session;
}
