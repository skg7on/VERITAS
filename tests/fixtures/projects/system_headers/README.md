## system_headers fixture

A single C++ translation unit, `system_headers.cpp`, that includes a clang
builtin header (`<stdint.h>`) and a POSIX SDK header (`<sys/types.h>`). The
compile command carries no `-resource-dir` and no `-isysroot`, mirroring what
CMake emits for a normal macOS build.

VERITAS links a Clang built with `CLANG_USE_XCSELECT=OFF`, so analysis must
inject both the clang resource directory (for the builtin headers) and the
macOS SDK sysroot (for the system headers). The fixture fails analysis when
either injection is missing and passes when both are present.
