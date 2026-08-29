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
# This module is intentionally OFF by default (see VERITAS_BUILD_SOUFFLE in the
# top-level CMakeLists.txt) until M8R.4 makes Souffle the mandatory production
# WPA engine. When enabled it does NOT add Souffle to the default `all` target:
# the `souffle` executable is built on demand via `cmake --build --target
# souffle`, or when a VERITAS target links libsouffle.
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
#   VERITAS_VENDORED_SOUFFLE_EXECUTABLE is a generator expression for the
#   built binary (distinct from the M8-era VERITAS_SOUFFLE_EXECUTABLE that
#   Dependencies.cmake sets via find_program; Task 13 unifies these).
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

# Disable options we do not need for a subproject build. SOUFFLE_GIT in
# particular must be OFF: its `git describe --tags` would otherwise run against
# the parent VERITAS repository (the vendored tree carries no .git) and bake a
# bogus VERITAS tag into Souffle's version output. OpenMP is disabled to avoid a
# macOS toolchain dependency we do not use; SWIG and doxygen are unused.
set(SOUFFLE_GIT OFF CACHE BOOL "Disable git describe against the parent repo" FORCE)
set(SOUFFLE_USE_OPENMP OFF CACHE BOOL "Disable OpenMP" FORCE)
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

# One upstream build-file patch accompanies the vendored tree:
# third_party/Souffle/src/CMakeLists.txt disables Souffle 2.5's Xcode-15 linker
# workaround (`-Wl,-ld_classic` / `target_link_options(... "-ld_classic")`). That
# flag is obsolete on the VERITAS toolchain (llvm@17 clang 17 + macOS SDK 26 +
# -fuse-ld=lld) and lld misreads it as `-l d_classic`, failing the `souffle`
# link. The patch is clearly marked "VERITAS (vendored build integration)" in
# place; everything else in the tree is byte-identical to the pinned revision.

# Generator expression resolving to the built Souffle executable.
set(VERITAS_VENDORED_SOUFFLE_EXECUTABLE "$<TARGET_FILE:souffle>")

# Single private wrapper. VERITAS code links veritas_third_party_souffle and
# never references libsouffle directly, so Souffle headers and types never leak
# into public VERITAS headers.
add_library(veritas_third_party_souffle INTERFACE)
target_link_libraries(veritas_third_party_souffle INTERFACE libsouffle)
