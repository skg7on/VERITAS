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

# tests/integration/analysis/svf/CMakeLists.txt
#
# One executable for the SVF integration suites. Each former binary keeps its
# CTest test name, filter, timeout, and labels.

add_executable(veritas_integration_analysis_svf_tests
  SvfSessionTest.cpp
  SvfFactMapperTest.cpp
  SvfIndirectCallTest.cpp
  SvfAliasKindsTest.cpp
  SvfTruncationTest.cpp
  RepeatedProjectAnalysisTest.cpp
  RequiredSvfBoundaryTest.cpp
)

target_link_libraries(veritas_integration_analysis_svf_tests
  PRIVATE
    veritas_svf_analysis
    veritas_analysis_pipeline
    veritas_build
    veritas_test_support
    veritas_unit_test_support
    LLVM
)

target_include_directories(veritas_integration_analysis_svf_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

add_test(NAME SvfSessionTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=SvfSessionTest.*)
set_tests_properties(SvfSessionTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

# `SvfMergeTest` lives in SvfFactMapperTest.cpp and was covered by this test
# when it ran the whole binary, so the filter names both suites.
add_test(NAME SvfFactMapperTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=SvfFactMapperTest.*:SvfMergeTest.*)
set_tests_properties(SvfFactMapperTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf;m9-entry;indirect-calls"
)

add_test(NAME SvfIndirectCallTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=SvfIndirectCallTest.*)
set_tests_properties(SvfIndirectCallTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

add_test(NAME SvfAliasKindsTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=SvfAliasKindsTest.*)
set_tests_properties(SvfAliasKindsTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

add_test(NAME SvfTruncationTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=SvfTruncationTest.*)
set_tests_properties(SvfTruncationTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

add_test(NAME RepeatedProjectAnalysisTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=RepeatedProjectAnalysisTest.*)
set_tests_properties(RepeatedProjectAnalysisTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

add_test(NAME RequiredSvfBoundaryTest
  COMMAND veritas_integration_analysis_svf_tests
    --gtest_filter=RequiredSvfBoundaryTest.*)
set_tests_properties(RequiredSvfBoundaryTest PROPERTIES
  TIMEOUT 10
  LABELS "integration;svf;boundary"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
