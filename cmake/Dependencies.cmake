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
find_package(Z3 REQUIRED CONFIG)
if(Z3_VERSION_STRING)
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
# RocksDB requires zstd::zstd, but the package may provide zstd::libzstd_shared
# or zstd::libzstd_static instead. Create the alias if needed.
if(NOT TARGET zstd::zstd)
  if(TARGET zstd::libzstd_shared)
    add_library(zstd::zstd ALIAS zstd::libzstd_shared)
    message(STATUS "VERITAS: Created zstd::zstd alias for zstd::libzstd_shared")
  elseif(TARGET zstd::libzstd_static)
    add_library(zstd::zstd ALIAS zstd::libzstd_static)
    message(STATUS "VERITAS: Created zstd::zstd alias for zstd::libzstd_static")
  else()
    # zstd not found yet, try to find it
    find_package(zstd REQUIRED CONFIG)
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
# Souffle — optional WPA Datalog execution engine.
# -----------------------------------------------------------------------------
set(VERITAS_HAS_SOUFFLE OFF)
if(VERITAS_ENABLE_SOUFFLE)
  find_program(VERITAS_SOUFFLE_EXECUTABLE NAMES souffle)
  if(VERITAS_SOUFFLE_EXECUTABLE)
    set(VERITAS_HAS_SOUFFLE ON)
    message(STATUS "VERITAS: Found Souffle ${VERITAS_SOUFFLE_EXECUTABLE}")
  else()
    message(STATUS "VERITAS: Souffle not found; C++ WPA remains enabled")
  endif()
else()
  message(STATUS "VERITAS: Souffle execution disabled")
endif()
