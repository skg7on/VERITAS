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

# Dependencies.cmake
#
# Discovers VERITAS's non-LLVM, non-SVF third-party dependencies. Included
# from the top-level CMakeLists.txt after VeritasLLVM and VeritasSVF so
# every dependency is resolved before src/ subdirectories are added.
#
# Required at M0 (per issue #3 acceptance criteria):
#   Z3, Protobuf, RocksDB, SQLite3, and (when VERITAS_BUILD_TESTS)
#   GoogleTest.
#
# Optional:
#   Souffle executable for M8 Datalog rule execution.
#
# Nothing in this module directly consumes the discovered packages — target
# linking happens in the leaf CMakeLists.txt files that actually need each
# library. This module exists so a missing dependency fails the top-level
# configure with a clear, single message rather than a cryptic link error
# five milestones from now.

# -----------------------------------------------------------------------------
# Z3 — SMT solver, required by SVF (transitively) and by future proof engines.
# -----------------------------------------------------------------------------
# Prefer the upstream CONFIG package (honors -DZ3_DIR / CMAKE_PREFIX_PATH); it
# defines the z3::libz3 imported target and the Z3_* version variables.
find_package(Z3 CONFIG QUIET)

# Fall back to a manual header + library search for platforms whose Z3 package
# ships no Z3Config.cmake (e.g. Ubuntu's libz3-dev provides z3.h and libz3.so
# but no CMake config), where find_package(Z3 CONFIG) fails.
if(NOT Z3_FOUND)
  find_path(VERITAS_Z3_INCLUDE_DIR NAMES z3.h z3++.h
            HINTS ${Z3_DIR} $ENV{Z3_HOME}
            PATH_SUFFIXES include)
  find_library(VERITAS_Z3_LIBRARY NAMES z3 libz3
               HINTS ${Z3_DIR} $ENV{Z3_HOME}
               PATH_SUFFIXES lib lib64 bin)

  if(VERITAS_Z3_INCLUDE_DIR AND VERITAS_Z3_LIBRARY)
    if(NOT TARGET z3::libz3)
      add_library(z3::libz3 UNKNOWN IMPORTED)
      set_target_properties(z3::libz3 PROPERTIES
        IMPORTED_LOCATION "${VERITAS_Z3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${VERITAS_Z3_INCLUDE_DIR}")
    endif()

    # Best-effort version for the summary message; not required for linking.
    set(VERITAS_Z3_VERSION_STRING "unknown")
    if(EXISTS "${VERITAS_Z3_INCLUDE_DIR}/z3_version.h")
      file(READ "${VERITAS_Z3_INCLUDE_DIR}/z3_version.h" _z3_ver_h)
      string(REGEX MATCH "#define Z3_MAJOR_VERSION[ \t]+([0-9]+)" _ "${_z3_ver_h}")
      set(_z3_major "${CMAKE_MATCH_1}")
      string(REGEX MATCH "#define Z3_MINOR_VERSION[ \t]+([0-9]+)" _ "${_z3_ver_h}")
      set(_z3_minor "${CMAKE_MATCH_1}")
      string(REGEX MATCH "#define Z3_BUILD_NUMBER[ \t]+([0-9]+)" _ "${_z3_ver_h}")
      set(_z3_patch "${CMAKE_MATCH_1}")
      if(_z3_major AND _z3_minor AND _z3_patch)
        set(VERITAS_Z3_VERSION_STRING "${_z3_major}.${_z3_minor}.${_z3_patch}")
      endif()
    endif()

    set(Z3_VERSION_STRING "${VERITAS_Z3_VERSION_STRING}")
    set(Z3_FOUND TRUE)
    set(VERITAS_Z3_FOUND_MANUALLY TRUE)
  endif()
endif()

if(NOT Z3_FOUND)
  message(FATAL_ERROR
    "VERITAS: Z3 not found. Install Z3 with CMake config support (a source\n"
    "build, Homebrew, or vcpkg), pass -DZ3_DIR=<dir containing Z3Config.cmake>,\n"
    "or install the system libz3-dev package.")
endif()

if(VERITAS_Z3_FOUND_MANUALLY)
  message(STATUS "VERITAS: Found Z3 ${Z3_VERSION_STRING} (manual search: ${VERITAS_Z3_LIBRARY})")
elseif(Z3_VERSION_STRING)
  message(STATUS "VERITAS: Found Z3 ${Z3_VERSION_STRING}")
else()
  message(STATUS "VERITAS: Found Z3")
endif()

# -----------------------------------------------------------------------------
# Protobuf — Function Summary IR / Evidence IR wire format.
# -----------------------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "VERITAS: Found Protobuf ${Protobuf_VERSION}")

# Protobuf 22.0+ (the upb-based rewrite) generates .pb.cc code that
# references Abseil logging/checking symbols directly (ABSL_CHECK / ABSL_LOG).
# CMake's FindProtobuf module does not propagate Protobuf's Abseil dependency
# onto `protobuf::libprotobuf`, so targets that link generated .pb.cc files
# must link the Abseil log/check libraries themselves (see
# src/summary/CMakeLists.txt).
#
# Older Protobuf (3.x, e.g. Ubuntu 24.04's 3.21.12) does not reference Abseil,
# so discover Abseil only when present rather than requiring it
# unconditionally. Do NOT gate this on Protobuf_VERSION: FindProtobuf decodes
# the GOOGLE_PROTOBUF_VERSION macro with the pre-22.0 scheme, so protobuf
# 35.1 reports as "7.35.1", which would compare spuriously below any 22.0
# threshold. src/summary/CMakeLists.txt links the absl targets only when they
# exist, so the absence of Abseil is harmless for Protobuf 3.x.
find_package(absl CONFIG QUIET)
if(absl_FOUND)
  message(STATUS "VERITAS: Found Abseil ${absl_VERSION} (Protobuf transitive dependency)")
else()
  message(STATUS "VERITAS: Abseil not found (not required for Protobuf 3.x)")
endif()

# -----------------------------------------------------------------------------
# RocksDB — CAS object store backing the Summary IR.
# -----------------------------------------------------------------------------
# RocksDB links zstd::zstd, but the zstd CONFIG package may instead provide
# zstd::libzstd_shared or zstd::libzstd_static. Find zstd first, then create the
# zstd::zstd alias if needed.
#
# Guard the find_package: LLVM's own config already calls find_package(zstd)
# and imports the zstd targets, and re-including a partially-imported export
# set is a hard CMake error, so only search when no zstd target exists yet.
if(NOT TARGET zstd::zstd
   AND NOT TARGET zstd::libzstd_shared
   AND NOT TARGET zstd::libzstd_static)
  find_package(zstd CONFIG QUIET)
endif()

if(NOT TARGET zstd::zstd)
  if(TARGET zstd::libzstd_shared)
    add_library(zstd::zstd ALIAS zstd::libzstd_shared)
    message(STATUS "VERITAS: Created zstd::zstd alias for zstd::libzstd_shared")
  elseif(TARGET zstd::libzstd_static)
    add_library(zstd::zstd ALIAS zstd::libzstd_static)
    message(STATUS "VERITAS: Created zstd::zstd alias for zstd::libzstd_static")
  else()
    message(FATAL_ERROR
      "VERITAS: zstd not found. RocksDB requires zstd::zstd; install zstd with\n"
      "CMake config support (Homebrew, vcpkg, or a source build) or pass\n"
      "-Dzstd_DIR=<dir containing zstdConfig.cmake>.")
  endif()
endif()

find_package(RocksDB REQUIRED CONFIG)
message(STATUS "VERITAS: Found RocksDB")

# -----------------------------------------------------------------------------
# SQLite3 — metadata / fact store.
# -----------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)
message(STATUS "VERITAS: Found SQLite3 ${SQLite3_VERSION}")

# -----------------------------------------------------------------------------
# GoogleTest — unit and integration test framework.
# -----------------------------------------------------------------------------
if(VERITAS_BUILD_TESTS)
  find_package(GTest REQUIRED CONFIG)
  include(GoogleTest)
  message(STATUS "VERITAS: Found GTest ${GTest_VERSION}")
endif()

# -----------------------------------------------------------------------------
# Souffle — the production recursive WPA engine (selected by VERITAS_WPA_ENGINE).
# -----------------------------------------------------------------------------
# The vendored Souffle is built under the `souffle` engine (VeritasSouffle.cmake
# sets VERITAS_SOUFFLE_EXECUTABLE to the built binary). Under `cpp-emergency`
# there is no Souffle and VERITAS_HAS_SOUFFLE stays off, so the M8 comparison
# test and any Souffle-dependent path are skipped.
set(VERITAS_HAS_SOUFFLE OFF)
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  set(VERITAS_HAS_SOUFFLE ON)
  message(STATUS "VERITAS: Souffle is the production WPA engine")
else()
  message(STATUS "VERITAS: Souffle disabled (cpp-emergency WPA)")
endif()
