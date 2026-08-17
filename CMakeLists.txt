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

# Top-level CMake file for VERITAS.
#
# Verified Evidence Reasoning IR for Trans-program Analysis and Semantics.
#
# Layout (M0 skeleton — populated milestone-by-milestone):
#   include/veritas/...    public headers (veritas_core, veritas_ir, ...)
#   src/core/              veritas_core library
#   src/tools/             CLI binaries (veritas-build|query|diff|explain)
#   tests/                 unit and integration tests
#   third_party/SVF/       vendored SVF source (required, not a submodule)
#   cmake/                 VERITAS CMake modules (this directory)
#
# Configure examples:
#
#   # Development: reuse a local llvm-project build tree
#   cmake -S . -B build \
#     -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build" \
#     -DZ3_DIR="$(brew --prefix z3)"
#
#   # System-installed LLVM
#   cmake -S . -B build -DLLVM_DIR=/usr/local/lib/cmake/llvm
#
#   # Static build (opt out of default shared libraries)
#   cmake -S . -B build -DBUILD_SHARED_LIBS=OFF

cmake_minimum_required(VERSION 3.23)

project(VERITAS
  VERSION 0.1.0
  DESCRIPTION "Verified Evidence Reasoning IR for Trans-program Analysis and Semantics"
  HOMEPAGE_URL "https://github.com/skg7on/VERITAS"
  LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# Global toolchain settings
# -----------------------------------------------------------------------------

# VERITAS code is C++20. SVF (C++17) and LLVM headers coexist under C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd, veritas-build ingest tests, and
# downstream tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prevent accidental in-source builds; the build tree must live outside src.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "VERITAS: in-source builds are not allowed. Use `cmake --preset default`.")
endif()

# Ninja is the supported default generator for VERITAS. Other generators
# (Make, Xcode, Visual Studio) still work, but only Ninja is exercised in CI
# and by the canonical `cmake --preset default` workflow.
if(NOT CMAKE_GENERATOR STREQUAL "Ninja" AND
   NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
  message(WARNING
    "VERITAS: generator '${CMAKE_GENERATOR}' is not Ninja. "
    "The canonical build uses Ninja and a build tree at <repo>/build. "
    "Run `cmake --preset default` (or pass `-G Ninja -B build`) unless you "
    "have a specific reason to use another generator.")
endif()

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------

# Shared libraries by default. SvfCore / SvfLLVM and every veritas_* target
# uses bare add_library(...) and therefore honors BUILD_SHARED_LIBS.
option(BUILD_SHARED_LIBS   "Build VERITAS and SVF as shared libraries"          ON)

option(VERITAS_BUILD_TESTS "Build VERITAS unit and integration tests"           ON)
option(VERITAS_BUILD_TOOLS "Build VERITAS CLI binaries (veritas-build, ...)"    ON)

# Position-independent code is required when producing shared libraries and
# is harmless for static builds. Set it globally so every target (including
# object libraries linked into shared VERITAS libs) is PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Put all VERITAS executables and libraries under conventional subtrees of
# the top-level build directory. Third-party subprojects (notably SVF) that
# set their own *_OUTPUT_DIRECTORY variables override these locally, which
# is what routes SVF artifacts under build/svf-build/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# -----------------------------------------------------------------------------
# CMake module path
# -----------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# -----------------------------------------------------------------------------
# Toolchain and third-party dependencies
# -----------------------------------------------------------------------------

# Warning contract for VERITAS-owned targets (target-scoped, never applied
# to SVF or LLVM).
include(VeritasWarnings)

# LLVM discovery:
#   * If -DLLVM_PROJECT_BUILD_DIR is passed, derive LLVM_DIR / Clang_DIR from
#     that build tree.
#   * Otherwise fall back to find_package(LLVM/Clang CONFIG) — honors any
#     LLVM_DIR / Clang_DIR the user provided directly, or system paths.
# Must run before VeritasSVF so SVF inherits the cached LLVM_DIR.
include(VeritasLLVM)

# SVF is a required dependency. Verify the vendored source is present,
# route its build tree under ${CMAKE_BINARY_DIR}/svf-build, and expose the
# private veritas_third_party_svf wrapper target.
include(VeritasSVF)

# Remaining VERITAS dependencies: Z3, Protobuf, RocksDB, SQLite3, and
# GoogleTest (when tests are enabled). Souffle is optional at M0 and
# becomes required at M8. Resolved before src/ subdirectories are added.
include(Dependencies)

# -----------------------------------------------------------------------------
# Sub-projects
# -----------------------------------------------------------------------------

# Source subdirectories are added conditionally so this outline configures
# cleanly at M0 before src/core, src/tools, and tests/ are populated. Each
# `add_subdirectory` will be uncommented (or its guard removed) as the
# corresponding milestone lands.

add_subdirectory(src/core)
add_subdirectory(src/build)
add_subdirectory(src/summarydb)

if(VERITAS_BUILD_TOOLS)
  add_subdirectory(src/tools)
endif()

if(VERITAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "VERITAS configuration summary:")
message(STATUS "  Version              : ${PROJECT_VERSION}")
message(STATUS "  Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard         : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Shared libraries     : ${BUILD_SHARED_LIBS}")
message(STATUS "  Position-indep. code : ${CMAKE_POSITION_INDEPENDENT_CODE}")
message(STATUS "  Build tests          : ${VERITAS_BUILD_TESTS}")
message(STATUS "  Build tools          : ${VERITAS_BUILD_TOOLS}")
message(STATUS "  LLVM version         : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Binary root          : ${CMAKE_BINARY_DIR}")
message(STATUS "  SVF binary dir       : ${VERITAS_SVF_BINARY_DIR}")
message(STATUS "")
