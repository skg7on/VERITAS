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

# tests/CMakeLists.txt
#
# Root of the VERITAS test tree. Only added when VERITAS_BUILD_TESTS=ON, and
# only after enable_testing() has been called from the top-level CMake file.

# Registers one aggregate M9 gate member: a CTest test named after the suite it
# covers, carrying the composite m9-entry label and its criterion label. TARGET
# is the executable that contains the suite; several gate members normally share
# one target now that tests are consolidated per directory.
#
# Remaining arguments are additional gtest filter patterns, for suites that live
# in the same translation unit as NAME but do not name a gate member. When none
# are given, the filter is NAME.*. The CTest test name never changes with the
# target layout, so tools/check_m9_entry.py keeps its exact membership contract.
function(veritas_m9_entry_test NAME CRITERION TARGET)
  set(filters ${ARGN})
  if(NOT filters)
    set(filters "${NAME}.*")
  endif()
  list(JOIN filters ":" filter_expression)
  add_test(NAME ${NAME}
    COMMAND $<TARGET_FILE:${TARGET}> --gtest_filter=${filter_expression})
  set_tests_properties(${NAME} PROPERTIES LABELS "m9-entry;${CRITERION}")
endfunction()

add_subdirectory(support)

add_executable(WpaFixtureHarnessTest
  support/WpaFixtureHarnessTest.cpp
)
target_link_libraries(WpaFixtureHarnessTest
  PRIVATE
    veritas_wpa_fixture_support
    GTest::gtest_main
)
add_test(NAME WpaFixtureHarnessTest COMMAND WpaFixtureHarnessTest)
set_tests_properties(WpaFixtureHarnessTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis;wpa"
)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(ci)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_subdirectory(qualification)
endif()

find_program(VERITAS_BASH_EXECUTABLE bash REQUIRED)

add_test(
  NAME veritas_pre_commit_hook
  COMMAND
    "${VERITAS_BASH_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/pre-commit-hook-test.sh"
    "${PROJECT_SOURCE_DIR}/.githooks/pre-commit"
)

set_tests_properties(veritas_pre_commit_hook PROPERTIES TIMEOUT 30)
