# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# VeritasSouffle.cmake
#
# Integrates the vendored Souffle source tree at third_party/Souffle as an
# in-tree dependency, mirroring VeritasSVF.cmake. Souffle is committed directly
# into third_party/Souffle/ (not a git submodule) at the pinned revision
# 5682a9f12e2668ecdd26348fe63cc508bc0fcf47 (tag 2.5). Building it from the
# vendored tree, rather than consuming a system package, makes the source
# revision verifiable by construction — the prerequisite the M8R.4 production
# provenance check requires.
#
# The Souffle build tree is placed under ${CMAKE_BINARY_DIR}/souffle-build:
#   build/                  <- VERITAS build tree
#   build/souffle-build/    <- Souffle build tree (libsouffle, souffle binary)
#
# This module is included when VERITAS_WPA_ENGINE=souffle, the production engine.
# It builds the vendored Souffle and derives production provenance: the source
# revision is pinned by construction (the tree is committed at the pinned
# revision), and the executable digest is computed at build time into a
# generated manifest. Souffle is not added to the default `all` target — the
# `souffle` executable builds on demand, or when a VERITAS target links
# libsouffle.
#
# Prerequisites:
#   * Bison >= 3.2 and Flex. macOS system Bison (2.3) is too old; install the
#     Homebrew kegs (`brew install bison flex`). Souffle's own configure finds
#     Homebrew bison/flex kegs when present.
#   * libffi, ncurses, zlib, and sqlite3 are disabled or found by Souffle's
#     own find_library calls; all are present on macOS by default.
#
# Result:
#   Targets libsouffle, souffle, souffleprof, and compiled become available.
#   VERITAS_SOUFFLE_EXECUTABLE is a generator expression for the built binary.
#   VERITAS_SOUFFLE_PINNED_REVISION is the pinned source revision.
#   The generated ${CMAKE_BINARY_DIR}/souffle-provenance.json records the
#   revision and the built executable's SHA-256 for the run manifest.
#   veritas_third_party_souffle is the single private INTERFACE wrapper.

# Verify the vendored Souffle source tree is present.
set(VERITAS_SOUFFLE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/Souffle")
if(NOT EXISTS "${VERITAS_SOUFFLE_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "VERITAS: vendored Souffle source not found at:\n"
    "  ${VERITAS_SOUFFLE_SOURCE_DIR}/CMakeLists.txt\n"
    "Souffle is committed directly into the VERITAS repository. Restore the\n"
    "third_party/Souffle/ tree from a clean checkout of the VERITAS main branch.")
endif()

# Route Souffle's build artifacts under ${CMAKE_BINARY_DIR}/souffle-build.
set(VERITAS_SOUFFLE_BINARY_DIR "${CMAKE_BINARY_DIR}/souffle-build")

message(STATUS "VERITAS: Souffle source dir = ${VERITAS_SOUFFLE_SOURCE_DIR}")
message(STATUS "VERITAS: Souffle binary dir = ${VERITAS_SOUFFLE_BINARY_DIR}")

# The pinned source revision (tag 2.5). The vendored tree is committed directly
# at this revision, so provenance is verifiable by construction: there is no
# external install whose revision must be discovered and cross-checked.
set(VERITAS_SOUFFLE_PINNED_REVISION "5682a9f12e2668ecdd26348fe63cc508bc0fcf47")
message(STATUS "VERITAS: Souffle revision = ${VERITAS_SOUFFLE_PINNED_REVISION}")

# Disable options we do not need for a subproject build. SOUFFLE_GIT in
# particular must be OFF: its `git describe --tags` would otherwise run against
# the parent VERITAS repository (the vendored tree carries no .git) and bake a
# bogus VERITAS tag into Souffle's version output. OpenMP is disabled to avoid a
# macOS toolchain dependency we do not use; SWIG and doxygen are unused.
set(SOUFFLE_GIT OFF CACHE BOOL "Disable git describe against the parent repo" FORCE)
set(SOUFFLE_USE_OPENMP OFF CACHE BOOL "Disable OpenMP" FORCE)
# Souffle defaults to libc++ when the compiler is Clang, but VERITAS itself uses
# the compiler's default standard library. Forcing libc++ breaks the Ubuntu CI
# (which has no libc++) and diverges from the rest of the build; leave it off so
# Souffle uses the platform default (libstdc++ on Linux, libc++ on macOS).
set(SOUFFLE_USE_LIBCPP OFF CACHE BOOL "Use the platform default standard library" FORCE)
set(SOUFFLE_SWIG OFF CACHE BOOL "Disable SWIG" FORCE)
set(SOUFFLE_SWIG_PYTHON OFF CACHE BOOL "Disable Python SWIG" FORCE)
set(SOUFFLE_SWIG_JAVA OFF CACHE BOOL "Disable Java SWIG" FORCE)
set(SOUFFLE_BASH_COMPLETION OFF CACHE BOOL "Disable bash completion" FORCE)
set(SOUFFLE_GENERATE_DOXYGEN "" CACHE STRING "Disable doxygen" FORCE)

# EXCLUDE_FROM_ALL keeps Souffle out of the default `all` target; the `souffle`
# executable and libsouffle build on demand.
add_subdirectory(
  "${VERITAS_SOUFFLE_SOURCE_DIR}"
  "${VERITAS_SOUFFLE_BINARY_DIR}"
  EXCLUDE_FROM_ALL
)

# VERITAS builds with RTTI and exceptions disabled (-fno-rtti -fno-exceptions),
# applied globally via add_compile_options in VeritasLLVM.cmake to match the LLVM
# libraries. Souffle is an upstream compiler that uses C++ exceptions
# (std::runtime_error for parse/semantic errors) and RTTI, so re-enable both for
# Souffle's own targets only. The later -frtti/-fexceptions override the earlier
# -fno-* flags on the command line; VERITAS-owned targets are unaffected.
foreach(_souffle_target IN ITEMS libsouffle souffle souffleprof compiled)
  if(TARGET ${_souffle_target})
    target_compile_options(${_souffle_target} PRIVATE -frtti -fexceptions)
  endif()
endforeach()

# Two upstream build-file patches accompany the vendored tree, both clearly
# marked "VERITAS (vendored build integration)" in place; everything else is
# byte-identical to the pinned revision:
#   * third_party/Souffle/src/CMakeLists.txt disables Souffle 2.5's Xcode-15
#     linker workaround (`-Wl,-ld_classic` / `target_link_options(... "-ld_classic")`),
#     which lld misreads as `-l d_classic`.
#   * third_party/Souffle/CMakeLists.txt disables the forced `-fuse-ld=lld`,
#     which fails on the CI's clang-only LLVM install (no lld).

# Generator expression resolving to the built Souffle executable.
set(VERITAS_SOUFFLE_EXECUTABLE "$<TARGET_FILE:souffle>")

# Derive production provenance at build time: the pinned revision and the built
# executable's SHA-256. The manifest is consumed by Task 15 to populate the run
# manifest's engine/toolchain identity.
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/souffle-provenance.json"
  COMMAND ${CMAKE_COMMAND}
          "-DVERITAS_SOUFFLE_EXECUTABLE=$<TARGET_FILE:souffle>"
          "-DVERITAS_SOUFFLE_REVISION=${VERITAS_SOUFFLE_PINNED_REVISION}"
          "-DVERITAS_SOUFFLE_PROVENANCE_OUTPUT=${CMAKE_BINARY_DIR}/souffle-provenance.json"
          -P "${CMAKE_SOURCE_DIR}/cmake/WriteSouffleProvenance.cmake"
  DEPENDS souffle
  COMMENT "Deriving Souffle provenance"
  VERBATIM
)
add_custom_target(veritas_souffle_provenance ALL
  DEPENDS "${CMAKE_BINARY_DIR}/souffle-provenance.json"
)

# Single private wrapper. VERITAS code links veritas_third_party_souffle and
# never references libsouffle directly, so Souffle headers and types never leak
# into public VERITAS headers.
add_library(veritas_third_party_souffle INTERFACE)
target_link_libraries(veritas_third_party_souffle INTERFACE libsouffle)

# The private semantic-key functor library. It exports only the six extern "C"
# stateful functor symbols declared in logic/common/semantic_key.dl, with the
# signature the generated programs call:
#
#   souffle::RamDomain name(souffle::SymbolTable*, souffle::RecordTable*, ...)
#
# It recompiles SemanticKeyCodec.cpp directly (rather than linking veritas_facts)
# so the library is self-contained: it carries no VERITAS -fno-exceptions /
# -fno-rtti settings, and needs no VERITAS library at runtime. The generated
# Souffle program and veritas-souffle-worker link against it via pinned
# -L/-l arguments; no Souffle type or functor ABI is exposed under include/.
add_library(veritas_souffle_functors SHARED
  ${CMAKE_SOURCE_DIR}/src/facts/SouffleSemanticKeyFunctor.cpp
  ${CMAKE_SOURCE_DIR}/src/facts/SemanticKeyCodec.cpp
)
set_target_properties(veritas_souffle_functors PROPERTIES
  OUTPUT_NAME "veritas-souffle-functors"
)
target_include_directories(veritas_souffle_functors PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${VERITAS_SOUFFLE_SOURCE_DIR}/src/include
)
# Stays at VERITAS's C++20: it compiles SemanticKeyCodec.cpp (C++20 spaceship
# operator) and does not instantiate the Souffle WriteStream template whose
# tcb::make_span backport is C++17-only. Only the generated bundles below need
# C++17.
# Re-enable RTTI and exceptions for the functor library too: it links against
# the Souffle ABI (SymbolTable/RecordTable), which VERITAS's global
# -fno-rtti -fno-exceptions must not reach.
target_compile_options(veritas_souffle_functors PRIVATE -frtti -fexceptions)

# ---------------------------------------------------------------------------
# Compiled rule bundles and the worker
# ---------------------------------------------------------------------------
#
# Each V2 rule bundle is generated with `souffle -g` into the build tree and
# compiled with __EMBEDDED_SOUFFLE__: that suppresses the generated `main` and
# registers a ProgramFactory under a name derived from the output filename
# (v2_reach / v2_maywrite). The worker links both bundles, libsouffle, and the
# functor library, and selects a program by --component.

set(VERITAS_SOUFFLE_GEN_DIR "${CMAKE_BINARY_DIR}/souffle-gen")
file(MAKE_DIRECTORY "${VERITAS_SOUFFLE_GEN_DIR}")

function(veritas_generate_souffle_program NAME NAMESPACE PROGRAM SOURCE)
  set(_gen_cpp "${VERITAS_SOUFFLE_GEN_DIR}/${PROGRAM}.cpp")
  add_custom_command(
    OUTPUT "${_gen_cpp}"
    COMMAND "${VERITAS_SOUFFLE_EXECUTABLE}" -g "${_gen_cpp}"
            -N "${NAMESPACE}"
            -I "${CMAKE_SOURCE_DIR}/logic/common"
            -I "${CMAKE_SOURCE_DIR}/logic/schema"
            "${SOURCE}"
    DEPENDS
      "${SOURCE}"
      "${CMAKE_SOURCE_DIR}/logic/common/semantic_key.dl"
      "${CMAKE_SOURCE_DIR}/logic/common/epistemic.dl"
      "${CMAKE_SOURCE_DIR}/logic/schema/relations.v2.dl"
    COMMENT "Generating compiled Souffle program ${PROGRAM}"
    VERBATIM
  )
  # Each bundle is compiled as C++17 (the generated code's WriteStream uses the
  # C++17 tcb::make_span backport) and wrapped in its own namespace so two
  # bundles can be linked into one worker without symbol collisions.
  add_library(${NAME} OBJECT "${_gen_cpp}")
  target_include_directories(${NAME} PRIVATE
    ${VERITAS_SOUFFLE_SOURCE_DIR}/src/include
  )
  set_target_properties(${NAME} PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_definitions(${NAME} PRIVATE __EMBEDDED_SOUFFLE__)
  target_compile_options(${NAME} PRIVATE -frtti -fexceptions)
endfunction()

veritas_generate_souffle_program(ReachabilityV2 veritas_reachability v2_reach
  ${CMAKE_SOURCE_DIR}/logic/reachability/reachability.v2.dl)
veritas_generate_souffle_program(MayWriteV2 veritas_maywrite v2_maywrite
  ${CMAKE_SOURCE_DIR}/logic/memory_effects/may_write.v2.dl)

add_executable(veritas_souffle_worker
  ${CMAKE_SOURCE_DIR}/src/wpa/SouffleWorkerMain.cpp
)
set_target_properties(veritas_souffle_worker PROPERTIES
  OUTPUT_NAME "veritas-souffle-worker"
)
target_include_directories(veritas_souffle_worker PRIVATE
  ${VERITAS_SOUFFLE_SOURCE_DIR}/src/include
)
# Stays at C++20 (SouffleWorkerMain.cpp uses std::string_view::starts_with); it
# links the C++17-generated bundles and libsouffle, whose ABI is identical on the
# same toolchain, and it does not itself instantiate the C++17-only templates.
target_compile_options(veritas_souffle_worker PRIVATE -frtti -fexceptions)
target_link_libraries(veritas_souffle_worker PRIVATE
  ReachabilityV2
  MayWriteV2
  libsouffle
  veritas_souffle_functors
)
