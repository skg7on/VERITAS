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

# tests/qualification/CMakeLists.txt
#
# The M8R.5 qualification corpus. Each aggregate gate member is a CTest test
# whose name is exactly the executable it runs; `check_no_skips.py` validates
# that the aggregate members for a label set run with no skips, disablement,
# or membership drift.

find_program(PYTHON3_EXECUTABLE python3 REQUIRED)

# Unit tests for the executable M9 entry gate itself. Not a gate member.
add_test(NAME M9EntryGateTest
  COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/M9EntryGateTest.py)
set_tests_properties(M9EntryGateTest PROPERTIES TIMEOUT 60)

# Criterion 10: the canonical documentation, provenance, and compiled targets
# agree on engine ownership and current behavior.
add_test(NAME M9DocumentationConsistencyTest
  COMMAND ${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/M9DocumentationConsistencyTest.py)
set_tests_properties(M9DocumentationConsistencyTest PROPERTIES
  LABELS "m9-entry;documentation-consistency"
  ENVIRONMENT "VERITAS_BUILD_DIR=${CMAKE_BINARY_DIR}"
  TIMEOUT 60
)

add_subdirectory(wpa)
