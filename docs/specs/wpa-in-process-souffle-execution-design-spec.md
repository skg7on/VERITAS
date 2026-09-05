# WPA In-Process Soufflé Execution — Design Spec

## Status

Draft — precedes the implementation.

## Problem

`SouffleWpaExecutor::Execute` runs the compiled Soufflé program by forking and
exec'ing a separate worker binary for **every** `(SCC, component)` pair:

```cpp
const pid_t child = ::fork();          // SouffleWpaExecutor.cpp:49
::execv(program.c_str(), argv.data()); //   ...:71
::waitpid(child, &status, ...);        //   ...:78
```

The worker itself already runs the Datalog **in-process** — it links the
generated programs (`__EMBEDDED_SOUFFLE__`, registered under `ProgramFactory`)
and calls `souffle::SouffleProgram::runAll(input_dir, output_dir, ...)`
(`SouffleWorkerMain.cpp:97-100`). So the per-component cost is not Soufflé
compilation; it is the fork/exec plus the worker's process startup and dynamic
linking, repeated ~6,786 times for leveldb (~3,393 SCCs × 2 components). This
is the last remaining bottleneck: the WPA stage takes ~4–5 min of the 7.7-min
leveldb run.

## Design

Replace the fork/exec of the worker binary with a direct in-process call into a
Soufflé shared library that exposes the same `runAll` contract through a C ABI.

### 1. A C-ABI Soufflé runner library

Extract `SouffleWorkerMain`'s body into a shared library
(`veritas_souffle_runner`) exposing:

```c
// Runs the named component's compiled program over -F input_dir, writing
// -D output_dir. Returns 0 on success, or a non-zero status. Never throws.
int veritas_souffle_run(const char* component,
                        const char* input_dir,
                        const char* output_dir,
                        unsigned jobs);
```

It links the two generated program objects, `libsouffle`, and the functor
library — exactly what the worker links today — and is compiled with RTTI and
exceptions **enabled** (`-frtti -fexceptions`), matching the worker. The C ABI
is the RTTI/exceptions boundary: `veritas_souffle_run` catches `std::exception`
internally (as `SouffleWorkerMain::main` already does) and returns a status
code, so no Soufflé type or exception crosses into the `-fno-rtti
-fno-exceptions` VERITAS code.

### 2. Call it from the executor

`SouffleWpaExecutor` links `veritas_souffle_runner` and calls
`veritas_souffle_run(...)` directly instead of `RunWorker(...)`. The input and
output directories are written and read exactly as `RelationIo` does today, so
the semantic contract (facts in, results + witnesses out) is unchanged.

### 3. Provenance

`WriteSouffleProvenance.cmake` currently hashes the worker **executable**. It
now hashes the runner **library** (the same `file(SHA256 ...)` over the built
artifact), so the pinned-revision provenance still binds the exact engine
bits. The worker executable can remain for CLI use, or be removed.

## Trade-off: per-component resource limits

The fork/exec also provided per-component **isolation and limits**:

- `memory_mb` — enforced with `setrlimit(RLIMIT_AS, ...)` in the child;
- `timeout` — enforced by `SIGKILL` on the deadline.

Both become unenforceable against an in-process call (there is no child to
limit or kill). The default configuration already sets `memory_mb = 0` (no
limit); the remaining concern is the 30s component timeout.

Two mitigations, chosen during implementation:

1. **Accept the relaxation.** The fixpoint rules are total and the SCC input is
   finite; a hung component would indicate a Soufflé defect, not a data
   condition. Drop the timeout in the in-process path and rely on the
   conformance corpus to keep the engine healthy.
2. **Keep the subprocess path as a guarded fallback** for runs that set a
   non-zero `memory_mb` or a finite timeout, and use the in-process path only
   for the default (unlimited) configuration.

(2) preserves the existing robustness contract at the cost of a second code
path; (1) is simpler. Recommendation: **(1)**, with the timeout kept as a
best-effort deadline check around `runAll` where feasible.

## Component impact

1. `src/wpa/SouffleWorkerMain.cpp` — split into the C-ABI runner body and a
   thin `main` shim (the worker stays for provenance/CLI, or is removed).
2. `include/veritas/wpa/SouffleRunner.h` (new) — the `extern "C"` declaration.
3. `src/wpa/SouffleWpaExecutor.cpp` — replace `RunWorker` with
   `veritas_souffle_run`, and delete the now-unused fork/exec plumbing.
4. `cmake/VeritasSouffle.cmake` — build `veritas_souffle_runner` (shared, RTTI
   + exceptions) and link it into `veritas_wpa`; update the provenance digest.
5. `cmake/WriteSouffleProvenance.cmake` — hash the runner library.

## Conformance

The in-process runner invokes the same `souffle::SouffleProgram::runAll` the
worker invoked, so `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts`
and the `wpa-qualification` corpus remain the gate: the C++ oracle and the
in-process Soufflé engine must still agree on canonical facts, witnesses, and
hashes.

## Verification

- Full build and test suite.
- `WpaExecutorConformanceTest` still passes.
- leveldb end-to-end: the WPA stage drops from ~4–5 min to well under a minute,
  and the whole run still completes with identical output.
