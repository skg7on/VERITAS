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
# The five WPA qualification aggregates (Task 16), built as one executable.
# Each registers a CTest test named after its suite, carrying both the
# `wpa-qualification` label and its `m9-entry;<criterion>` label. The whole tree
# requires the production Souffle engine, which the enclosing
# tests/CMakeLists.txt gates on.

add_executable(veritas_qualification_wpa_tests
  WpaDifferentialQualificationTest.cpp
  WpaDeterminismQualificationTest.cpp
  WpaFailureQualificationTest.cpp
  WpaMigrationQualificationTest.cpp
  WpaPerformanceQualificationTest.cpp
)
target_link_libraries(veritas_qualification_wpa_tests PRIVATE
  veritas::wpa
  veritas::summarydb
  veritas_analysis
  veritas_test_support
  GTest::gtest_main
)
target_compile_definitions(veritas_qualification_wpa_tests PRIVATE
  VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
)
add_dependencies(veritas_qualification_wpa_tests
  veritas_souffle_worker
  veritas_souffle_provenance
)
veritas_add_warnings(veritas_qualification_wpa_tests)

# Registers one aggregate gate member. The CTest name is the suite name, so the
# M9 entry gate's exact membership is independent of the target layout.
function(veritas_qualification_member NAME CRITERION)
  add_test(NAME ${NAME}
    COMMAND veritas_qualification_wpa_tests --gtest_filter=${NAME}.*)
  set_tests_properties(${NAME} PROPERTIES
    LABELS "wpa-qualification;m9-entry;${CRITERION}"
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
  )
endfunction()

veritas_qualification_member(WpaDifferentialQualificationTest engine-conformance)
veritas_qualification_member(WpaDeterminismQualificationTest relations-v2)
veritas_qualification_member(WpaFailureQualificationTest failure-atomicity)
veritas_qualification_member(WpaMigrationQualificationTest run-identity)

# The performance aggregate drives the full ProjectAnalyzer pipeline rather than
# the low-level WPA seam, so it gets the longer timeout.
veritas_qualification_member(WpaPerformanceQualificationTest souffle-production)
set_tests_properties(WpaPerformanceQualificationTest PROPERTIES TIMEOUT 120)
