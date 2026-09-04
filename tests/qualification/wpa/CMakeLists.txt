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
# The five M8R.5 qualification executables. Each is one aggregate CTest test
# carrying `wpa-qualification`, `m9-entry`, and its criterion label; individual
# GoogleTest cases deliberately do not expand either set.

if(NOT VERITAS_WPA_ENGINE STREQUAL "souffle")
  return()
endif()

set(_qual_common_libs
  veritas_wpa_fixture_support
  veritas::wpa
  GTest::gtest_main
)

# engine-conformance
add_executable(WpaDifferentialQualificationTest
  WpaDifferentialQualificationTest.cpp
)
target_link_libraries(WpaDifferentialQualificationTest PRIVATE ${_qual_common_libs})
target_compile_definitions(WpaDifferentialQualificationTest PRIVATE
  VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
)
add_dependencies(WpaDifferentialQualificationTest
  veritas_souffle_worker
  veritas_souffle_provenance
)
veritas_add_warnings(WpaDifferentialQualificationTest)
add_test(NAME WpaDifferentialQualificationTest COMMAND WpaDifferentialQualificationTest)
set_tests_properties(WpaDifferentialQualificationTest PROPERTIES
  TIMEOUT 300
  LABELS "wpa-qualification;m9-entry;engine-conformance"
)

# relations-v2
add_executable(WpaDeterminismQualificationTest
  WpaDeterminismQualificationTest.cpp
)
target_link_libraries(WpaDeterminismQualificationTest PRIVATE ${_qual_common_libs})
veritas_add_warnings(WpaDeterminismQualificationTest)
add_test(NAME WpaDeterminismQualificationTest COMMAND WpaDeterminismQualificationTest)
set_tests_properties(WpaDeterminismQualificationTest PROPERTIES
  TIMEOUT 300
  LABELS "wpa-qualification;m9-entry;relations-v2"
)

# failure-atomicity
add_executable(WpaFailureQualificationTest
  WpaFailureQualificationTest.cpp
)
target_link_libraries(WpaFailureQualificationTest PRIVATE ${_qual_common_libs})
target_compile_definitions(WpaFailureQualificationTest PRIVATE
  VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
)
add_dependencies(WpaFailureQualificationTest
  veritas_souffle_worker
  veritas_souffle_provenance
)
veritas_add_warnings(WpaFailureQualificationTest)
add_test(NAME WpaFailureQualificationTest COMMAND WpaFailureQualificationTest)
set_tests_properties(WpaFailureQualificationTest PROPERTIES
  TIMEOUT 300
  LABELS "wpa-qualification;m9-entry;failure-atomicity"
)

# run-identity
add_executable(WpaMigrationQualificationTest
  WpaMigrationQualificationTest.cpp
)
target_link_libraries(WpaMigrationQualificationTest PRIVATE ${_qual_common_libs})
veritas_add_warnings(WpaMigrationQualificationTest)
add_test(NAME WpaMigrationQualificationTest COMMAND WpaMigrationQualificationTest)
set_tests_properties(WpaMigrationQualificationTest PROPERTIES
  TIMEOUT 300
  LABELS "wpa-qualification;m9-entry;run-identity"
)

# souffle-production
add_executable(WpaPerformanceQualificationTest
  WpaPerformanceQualificationTest.cpp
)
target_link_libraries(WpaPerformanceQualificationTest PRIVATE ${_qual_common_libs})
veritas_add_warnings(WpaPerformanceQualificationTest)
add_test(NAME WpaPerformanceQualificationTest COMMAND WpaPerformanceQualificationTest)
set_tests_properties(WpaPerformanceQualificationTest PROPERTIES
  TIMEOUT 300
  LABELS "wpa-qualification;m9-entry;souffle-production"
)
