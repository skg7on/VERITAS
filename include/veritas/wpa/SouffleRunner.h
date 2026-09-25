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

// SouffleRunner.h — the C ABI into the compiled Souffle rule bundles.
//
// The Souffle programs are generated with __EMBEDDED_SOUFFLE__ and linked into
// a shared library that is compiled with RTTI and exceptions enabled (Souffle
// requires both). This C ABI is the boundary to the rest of VERITAS, which is
// compiled with -fno-rtti -fno-exceptions: the functions catch std::exception
// internally and never let a Souffle type or exception cross the boundary.
//
// Two entry points share that boundary. `veritas_souffle_run` is the
// file-backed one-shot form: EDB relations in, derived relations and witnesses
// out, through a directory. The session form below does the same work with no
// directory, no file, and no text: open a component, insert each EDB relation's
// rows, run once, then scan relations back out cell by cell.

#ifndef VERITAS_WPA_SOUFFLE_RUNNER_H_
#define VERITAS_WPA_SOUFFLE_RUNNER_H_

#ifdef __cplusplus
extern "C" {
#endif

// Runs the named component's compiled Souffle program over `input_dir` (the
// -F relation inputs) and writes derived relations and witnesses to
// `output_dir` (the -D destination). `component` is "reachability" or
// "memory-effects". Returns 0 on success and a non-zero status otherwise.
int veritas_souffle_run(const char* component, const char* input_dir,
                        const char* output_dir, unsigned jobs);

// --- In-memory component session -------------------------------------------
//
// A cell is either an interned symbol or a 64-bit number. The tag decides
// which member is meaningful, so no Souffle type crosses the boundary:
// `souffle::tuple`, `souffle::Relation`, and iteration over them require RTTI
// and exceptions, which only SouffleRunner.cpp is allowed to enable.
//
// The engine's own domain is 32 bits wide in this build (RAM_DOMAIN_SIZE), so a
// number cell is narrowed on the way in and widened on the way out, exactly as
// the file-backed path's text parser narrows it.
//
// Every function below returns 0 on success and one of these otherwise:
//
//   1  the Souffle engine refused the call (it raised)
//   2  invalid argument — a null pointer, an unknown component name, an arity
//      that disagrees with the relation, or a cell kind that disagrees with the
//      column's declared primitive type
//   3  no such relation in the component's compiled program (insert, scan), or
//      no registered program for the component's name (open)
//   4  not implemented — `veritas_souffle_session_reset` (Task 3 owns it)
enum VeritasSouffleCellKind {
  VERITAS_SOUFFLE_CELL_SYMBOL = 0,
  VERITAS_SOUFFLE_CELL_NUMBER = 1
};

typedef struct {
  unsigned char kind;         // one of VeritasSouffleCellKind
  unsigned long long number;  // meaningful when kind is ..._NUMBER
  const char* symbol;         // meaningful when kind is ..._SYMBOL
} VeritasSouffleCell;

typedef struct VeritasSouffleSession VeritasSouffleSession;

// Called once per row while scanning. Returns 0 to continue, non-zero to stop
// the scan early (which is not an error). The cell array and every symbol it
// points at are owned by the runner and valid only for the call.
typedef int (*VeritasSouffleRowSink)(void* context,
                                     const VeritasSouffleCell* cells,
                                     unsigned long long arity);

// Opens a session over one component's compiled program. `jobs` is the worker
// thread count handed to the engine. On success `*out_session` owns a program
// instance the caller must release with veritas_souffle_session_close; on
// failure it is set to null.
//
// Only the relations the component's rules actually use are present in the
// compiled program — Souffle eliminates the rest — so a relation the component
// never mentions is reported as unknown rather than as empty.
int veritas_souffle_session_open(const char* component, unsigned jobs,
                                 VeritasSouffleSession** out_session);

// Inserts `row_count` consecutive rows of `arity` cells each into `relation`,
// which must be an EDB relation of the session's component. `arity` must equal
// the relation's column count and each cell's kind must match its column's
// declared primitive type. Duplicate rows are accepted, as they are by the
// engine's own loader.
//
// A rejected batch is not rolled back: cells are checked as they are pushed, so
// a failure on a later row leaves the rows before it in the relation. A caller
// that continues with a session after a failed insert is asking for the rows it
// already handed over.
int veritas_souffle_session_insert(VeritasSouffleSession* session,
                                   const char* relation,
                                   const VeritasSouffleCell* cells,
                                   unsigned long long arity,
                                   unsigned long long row_count);

// Evaluates the program with I/O disabled: no relation is loaded from or
// stored to a file.
int veritas_souffle_session_run(VeritasSouffleSession* session);

// Walks every row of `relation`, in the engine's iteration order, and calls
// `sink` once per row. `cell_kinds[i]` is the VeritasSouffleCellKind of column
// i: a caller passes the column's schema domain, kString mapping to
// VERITAS_SOUFFLE_CELL_SYMBOL and every other domain to
// VERITAS_SOUFFLE_CELL_NUMBER, so the runner never guesses a column's type from
// its bytes.
int veritas_souffle_session_scan(VeritasSouffleSession* session,
                                 const char* relation,
                                 const unsigned char* cell_kinds,
                                 unsigned long long arity,
                                 VeritasSouffleRowSink sink,
                                 void* context);

// Drops every row of every relation so the session can evaluate another
// component. Not implemented yet: Task 3 owns the purge semantics and the
// differential proof that a reset session is indistinguishable from a fresh
// one, so this currently returns 4 rather than pretending to succeed.
int veritas_souffle_session_reset(VeritasSouffleSession* session);

// Releases the session and its program instance. Null is accepted.
void veritas_souffle_session_close(VeritasSouffleSession* session);

#ifdef __cplusplus
}
#endif

#endif  // VERITAS_WPA_SOUFFLE_RUNNER_H_
