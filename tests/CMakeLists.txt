# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The VERITAS Authors.
# tests/CMakeLists.txt
#
# Root of the VERITAS test tree. Only added when VERITAS_BUILD_TESTS=ON, and
# only after enable_testing() has been called from the top-level CMake file.

add_subdirectory(unit)
add_subdirectory(integration)
