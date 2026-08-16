# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The VERITAS Authors.
# VeritasLLVM.cmake
#
# Configures LLVM and Clang dependencies for VERITAS and the SVF submodule.
#
# Accepts LLVM_PROJECT_BUILD_DIR to point to a local LLVM build directory,
# and derives LLVM_DIR and Clang_DIR from it. Both VERITAS and the pinned
# SVF submodule at third_party/SVF will use the same LLVM installation.
#
# Usage:
#   cmake -S . -B build -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"
#
# If LLVM_PROJECT_BUILD_DIR is not set, falls back to standard find_package
# behavior (uses LLVM_DIR and Clang_DIR directly, or searches system paths).
#
# IMPORTANT: This module must be included BEFORE add_subdirectory(third_party/SVF)
# so that SVF inherits the cached LLVM_DIR variable.

if(LLVM_PROJECT_BUILD_DIR)
  message(STATUS "VERITAS: Using local LLVM build at ${LLVM_PROJECT_BUILD_DIR}")

  # Derive LLVM_DIR and Clang_DIR from LLVM_PROJECT_BUILD_DIR
  set(LLVM_DIR "${LLVM_PROJECT_BUILD_DIR}/lib/cmake/llvm" CACHE PATH "Path to LLVMConfig.cmake")
  set(Clang_DIR "${LLVM_PROJECT_BUILD_DIR}/lib/cmake/clang" CACHE PATH "Path to ClangConfig.cmake")

  # Verify the paths exist
  if(NOT EXISTS "${LLVM_DIR}/LLVMConfig.cmake")
    message(FATAL_ERROR
      "LLVM_PROJECT_BUILD_DIR is set but LLVMConfig.cmake not found at:\n"
      "  ${LLVM_DIR}/LLVMConfig.cmake\n"
      "Ensure LLVM_PROJECT_BUILD_DIR points to a valid LLVM build directory.")
  endif()

  if(NOT EXISTS "${Clang_DIR}/ClangConfig.cmake")
    message(FATAL_ERROR
      "LLVM_PROJECT_BUILD_DIR is set but ClangConfig.cmake not found at:\n"
      "  ${Clang_DIR}/ClangConfig.cmake\n"
      "Ensure LLVM_PROJECT_BUILD_DIR points to a valid LLVM build with Clang enabled.")
  endif()

  message(STATUS "VERITAS: LLVM_DIR set to ${LLVM_DIR}")
  message(STATUS "VERITAS: Clang_DIR set to ${Clang_DIR}")
else()
  message(STATUS "VERITAS: LLVM_PROJECT_BUILD_DIR not set, using standard LLVM discovery")
  message(STATUS "VERITAS: Set LLVM_DIR and Clang_DIR, or let CMake search system paths")
endif()

# Find LLVM package.
# CLAUDE.md requires LLVM 22 or newer. LLVMConfigVersion.cmake marks only
# the exact matching major.minor as compatible (no range support), so we
# ask for the package unversioned and validate LLVM_PACKAGE_VERSION below.
find_package(LLVM REQUIRED CONFIG)

if(LLVM_PACKAGE_VERSION VERSION_LESS "22.0")
  message(FATAL_ERROR
    "VERITAS requires LLVM 22 or newer, but found ${LLVM_PACKAGE_VERSION}\n"
    "from ${LLVM_DIR}. Point LLVM_PROJECT_BUILD_DIR (or LLVM_DIR / Clang_DIR)\n"
    "at an LLVM/Clang 22+ build.")
endif()

message(STATUS "VERITAS: Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "VERITAS: LLVM definitions: ${LLVM_DEFINITIONS}")
message(STATUS "VERITAS: LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "VERITAS: LLVM library dirs: ${LLVM_LIBRARY_DIRS}")

# Find Clang package. Same rationale as LLVM above; Clang ships alongside
# LLVM in the monorepo, so the version check on LLVM_PACKAGE_VERSION covers it.
find_package(Clang REQUIRED CONFIG)

message(STATUS "VERITAS: Found Clang ${CLANG_VERSION}")
