# Soufflé Third-Party Dependency Documentation

**Path:** `third_party/Souffle`

**Upstream:** https://github.com/souffle-lang/souffle.git

**Revision:** `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` (tag `2.5`)

**License:** UPL-1.0 (Universal Permissive License, Oracle and/or its affiliates)

See `third_party/Souffle/LICENSE` for the full upstream license text.

**Purpose:** Soufflé is a Datalog compiler and interpreter. VERITAS uses it as the production recursive whole-program-analysis (WPA) engine — the M8R.4 deliverable that replaces the C++ fixpoint evaluator as the normal execution path. C++ remains only a differential conformance oracle and an explicitly selected `cpp-emergency` engine.

**Integration:** Soufflé is vendored directly in the VERITAS repository (not a git submodule), mirroring the SVF precedent. The source tree at `third_party/Souffle/` excludes the upstream `tests/` directory (58 MB of Soufflé's own suite) and `.git` metadata. Three patches accompany the tree, all clearly marked `VERITAS (vendored build integration)` in place: `src/CMakeLists.txt` disables Soufflé 2.5's obsolete Xcode-15 linker workaround (`-ld_classic`), the top-level `CMakeLists.txt` disables the forced `-fuse-ld=lld` (which fails on the CI's clang-only LLVM install), and `src/interpreter/Index.h` replaces an `std::atomic` copy-assignment with store/load (which libstdc++ 14 rejects). Everything else is byte-identical to the pinned revision. Building from the vendored tree — rather than consuming a system package — makes the source revision verifiable by construction, which the M8R.4 production provenance check requires. The Homebrew `souffle` bottle records no source revision (`tap_git_head: null`), so it cannot satisfy that check.

**Toolchain Requirements:**
- CMake 3.15+
- C++17 compiler
- Bison >= 3.2 and Flex (macOS system Bison 2.3 is too old; `brew install bison flex`)
- libffi, ncurses, zlib, and sqlite3 (all present on macOS by default)

**Build Configuration:**
- `cmake/VeritasSouffle.cmake` adds Soufflé via `add_subdirectory` with `EXCLUDE_FROM_ALL`, routing artifacts under `${CMAKE_BINARY_DIR}/souffle-build/`.
- The `souffle` executable and `libsouffle` build on demand (`cmake --build --target souffle`); they do not join the default `all` target.
- Gated behind `VERITAS_BUILD_SOUFFLE` (default `OFF`) until M8R.4 makes Soufflé the mandatory production engine.
- `SOUFFLE_GIT` is disabled so its `git describe --tags` does not run against the parent VERITAS repository. OpenMP, SWIG, and Doxygen are disabled.
- VERITAS applies `-fno-rtti -fno-exceptions` globally; `VeritasSouffle.cmake` re-enables both (`-frtti -fexceptions`) for Soufflé's own targets only, since Soufflé uses C++ exceptions and RTTI.

**Initialization:**
Soufflé is committed directly into `third_party/Souffle/`. A clean VERITAS checkout includes the full vendored source tree. No separate initialization step is required.

**Version Selection:**
Revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` is pinned because it is the exact tag `2.5` revision the M8R architecture specification requires (full 40-character source revision, not a version substring). It is the reference against which production provenance is verified.

**Licensing Considerations:**
Soufflé is licensed under UPL-1.0, a permissive license compatible with VERITAS's Apache-2.0. VERITAS links to Soufflé's generated programs and functor library as a separate dependency, preserving the distinction between VERITAS code and the Soufflé library.

**References:**
- Soufflé project: https://souffle-lang.github.io
- Soufflé repository: https://github.com/souffle-lang/souffle
- VERITAS toolchain contract: `docs/third_party/LLVM.md`
- VERITAS SVF integration: `docs/third_party/SVF.md`
- M8R remediation: `docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md`
