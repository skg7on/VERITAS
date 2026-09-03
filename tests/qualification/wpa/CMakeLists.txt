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

# tests/qualification/wpa/CMakeLists.txt
#
# The five WPA qualification aggregates (Task 16). Each registers a CTest test
# named after its executable, carrying both the `wpa-qualification` label and
# its `m9-entry;<criterion>` label. The whole tree requires the production
# Souffle engine, which the enclosing tests/CMakeLists.txt gates on.

function(veritas_qualification_test NAME CRITERION)
  add_executable(${NAME} ${NAME}.cpp)
  target_link_libraries(${NAME} PRIVATE
    veritas::wpa
    GTest::gtest_main
  )
  target_compile_definitions(${NAME} PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
  )
  add_dependencies(${NAME}
    veritas_souffle_worker
    veritas_souffle_provenance
  )
  veritas_add_warnings(${NAME})

  add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
  set_tests_properties(${NAME} PROPERTIES
    LABELS "wpa-qualification;m9-entry;${CRITERION}"
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
  )
endfunction()

veritas_qualification_test(WpaDifferentialQualificationTest engine-conformance)
veritas_qualification_test(WpaDeterminismQualificationTest relations-v2)
veritas_qualification_test(WpaFailureQualificationTest failure-atomicity)
veritas_qualification_test(WpaMigrationQualificationTest run-identity)

# The migration test reads and writes the version-aware SummaryDB.
target_link_libraries(WpaMigrationQualificationTest PRIVATE veritas::summarydb)

# The performance test drives the full ProjectAnalyzer pipeline rather than the
# low-level WPA seam.
add_executable(WpaPerformanceQualificationTest WpaPerformanceQualificationTest.cpp)
target_link_libraries(WpaPerformanceQualificationTest PRIVATE
  veritas_analysis
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(WpaPerformanceQualificationTest)
add_test(NAME WpaPerformanceQualificationTest
  COMMAND $<TARGET_FILE:WpaPerformanceQualificationTest>)
set_tests_properties(WpaPerformanceQualificationTest PROPERTIES
  LABELS "wpa-qualification;m9-entry;souffle-production"
  SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
  TIMEOUT 120
)
