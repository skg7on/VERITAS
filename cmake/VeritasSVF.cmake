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

# VeritasSVF.cmake
#
# Integrates the vendored SVF source tree at third_party/SVF as a required
# in-tree dependency. SVF is always built as part of the VERITAS default
# configuration; there is no VERITAS_ENABLE_SVF toggle.
#
# The SVF build tree is placed under ${CMAKE_BINARY_DIR}/svf-build so that a
# top-level configure with `-B build` produces:
#   build/                <- VERITAS build tree
#   build/svf-build/      <- SVF build tree (libs, extapi.bc, cmake exports)
#
# Prerequisites (must be satisfied before this module is included):
#   * VeritasLLVM.cmake has already run (LLVM_DIR / Clang_DIR are cached).
#   * BUILD_SHARED_LIBS is set to the desired value (SVF honors it since
#     SvfCore / SvfLLVM are declared without STATIC/SHARED).
#
# Result:
#   Targets SvfCore, SvfLLVM, and SvfFlags are available in the current
#   directory scope. This module also declares veritas_third_party_svf as
#   the single private wrapper VERITAS code links against; no other VERITAS
#   target should reference SVF targets directly.

# Verify the vendored SVF source tree is present. SVF is committed directly
# into third_party/SVF/ (not a git submodule), so a missing CMakeLists.txt
# indicates an incomplete checkout or a manually pruned tree.
set(VERITAS_SVF_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/SVF")
if(NOT EXISTS "${VERITAS_SVF_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "VERITAS: vendored SVF source not found at:\n"
    "  ${VERITAS_SVF_SOURCE_DIR}/CMakeLists.txt\n"
    "SVF is committed directly into the VERITAS repository. Restore the\n"
    "third_party/SVF/ tree from a clean checkout of the VERITAS main branch.")
endif()

# Route SVF's build artifacts (libs, generated headers, extapi.bc, cmake
# exports) under ${CMAKE_BINARY_DIR}/svf-build. This is passed as the
# `binary_dir` argument to add_subdirectory, which SVF picks up as
# SVF_BINARY_DIR internally.
set(VERITAS_SVF_BINARY_DIR "${CMAKE_BINARY_DIR}/svf-build")

message(STATUS "VERITAS: SVF source dir  = ${VERITAS_SVF_SOURCE_DIR}")
message(STATUS "VERITAS: SVF binary dir  = ${VERITAS_SVF_BINARY_DIR}")
message(STATUS "VERITAS: SVF shared libs = ${BUILD_SHARED_LIBS}")

# EXCLUDE_FROM_ALL keeps SVF's stand-alone front-end binaries (wpa, ae, dvf,
# saber, svf-ex, llvm2svf, ...) out of the default `all` target. SvfCore and
# SvfLLVM will still be built on demand because veritas_third_party_svf (and
# transitively the VERITAS libraries and CLIs) link against them.
add_subdirectory(
  "${VERITAS_SVF_SOURCE_DIR}"
  "${VERITAS_SVF_BINARY_DIR}"
  EXCLUDE_FROM_ALL
)

# Single private wrapper. VERITAS code links veritas_third_party_svf and
# never references SvfCore / SvfLLVM directly, so SVF headers and types
# never leak into public VERITAS headers.
add_library(veritas_third_party_svf INTERFACE)
target_link_libraries(veritas_third_party_svf INTERFACE SvfCore SvfLLVM)
