# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The VERITAS Authors.
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
# Optional at M0:
#   Souffle (becomes required in M8).
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

# -----------------------------------------------------------------------------
# RocksDB — CAS object store backing the Summary IR.
# -----------------------------------------------------------------------------
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
# Souffle — WPA Datalog engine. Optional at M0, required starting at M8.
# -----------------------------------------------------------------------------
find_package(Souffle QUIET)
if(Souffle_FOUND)
  message(STATUS "VERITAS: Found Souffle (optional at M0)")
else()
  message(STATUS "VERITAS: Souffle not found (optional at M0, required at M8)")
endif()
