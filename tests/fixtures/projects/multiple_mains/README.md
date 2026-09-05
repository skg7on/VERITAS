## multiple_mains fixture

Two well-formed C++ translation units, `a.cpp` and `b.cpp`, each defining its
own `main`. Together they model a `compile_commands.json` exported from a
multi-target build (library + executables + tests). Whole-program analysis
requires a single program, so analysis must reject this input with a clear
error rather than emitting the raw LLVM "symbol multiply defined" failure.
