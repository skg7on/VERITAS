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

# Registers one aggregate M9 gate member: a CTest test named after its
# executable, carrying the composite m9-entry label and its criterion label.
# The executable must already exist (created via add_executable).
function(veritas_m9_entry_test NAME CRITERION)
  add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
  set_tests_properties(${NAME} PROPERTIES LABELS "m9-entry;${CRITERION}")
endfunction()

add_subdirectory(support)
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
