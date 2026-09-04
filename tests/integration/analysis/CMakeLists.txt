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

add_executable(project_analyzer_integration_test
  ProjectAnalyzerTest.cpp
)

target_link_libraries(project_analyzer_integration_test
  PRIVATE
    veritas_analysis
    veritas_test_support
    veritas_unit_test_support
)

add_test(NAME ProjectAnalyzerTest COMMAND project_analyzer_integration_test)
set_tests_properties(ProjectAnalyzerTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;analysis"
)

add_executable(local_analysis_stage_integration_test
  LocalAnalysisStageTest.cpp
)

target_link_libraries(local_analysis_stage_integration_test
  PRIVATE
    veritas_analysis_pipeline
    veritas_build
    veritas_test_support
    GTest::gtest_main
)

add_test(NAME LocalAnalysisStageTest
  COMMAND local_analysis_stage_integration_test)
set_tests_properties(LocalAnalysisStageTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;analysis"
)

add_executable(summary_v2_pipeline_integration_test
  SummaryV2PipelineTest.cpp
)

target_link_libraries(summary_v2_pipeline_integration_test
  PRIVATE
    veritas_analysis
    veritas_summarydb
    veritas_test_support
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME SummaryV2PipelineTest
  COMMAND summary_v2_pipeline_integration_test)
set_tests_properties(SummaryV2PipelineTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis"
)

add_subdirectory(svf)
add_subdirectory(cpg)

add_executable(WpaEmergencyModeTest
  WpaEmergencyModeTest.cpp
)
target_link_libraries(WpaEmergencyModeTest
  PRIVATE
    veritas_analysis
    veritas_wpa
    veritas_test_support
    veritas_unit_test_support
)
add_test(NAME WpaEmergencyModeTest COMMAND WpaEmergencyModeTest)
set_tests_properties(WpaEmergencyModeTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis;wpa"
)

add_executable(SemanticMemorySvfTest
  SemanticMemorySvfTest.cpp
)
target_link_libraries(SemanticMemorySvfTest
  PRIVATE
    veritas_wpa_fixture_support
    veritas_test_support
    GTest::gtest_main
)
veritas_add_warnings(SemanticMemorySvfTest)
gtest_discover_tests(SemanticMemorySvfTest DISCOVERY_TIMEOUT 180)

add_executable(SemanticMemorySummaryDbTest
  SemanticMemorySummaryDbTest.cpp
)
target_link_libraries(SemanticMemorySummaryDbTest
  PRIVATE
    veritas_wpa_fixture_support
    veritas_test_support
    GTest::gtest_main
)
veritas_add_warnings(SemanticMemorySummaryDbTest)
gtest_discover_tests(SemanticMemorySummaryDbTest DISCOVERY_TIMEOUT 180)

add_executable(SemanticDispatchSvfTest
  SemanticDispatchSvfTest.cpp
)
target_link_libraries(SemanticDispatchSvfTest
  PRIVATE
    veritas_wpa_fixture_support
    veritas_wpa
    veritas_test_support
    GTest::gtest_main
)
target_include_directories(SemanticDispatchSvfTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
veritas_add_warnings(SemanticDispatchSvfTest)
gtest_discover_tests(SemanticDispatchSvfTest DISCOVERY_TIMEOUT 180)

add_executable(SemanticZooFixtureTest
  SemanticZooFixtureTest.cpp
)
target_include_directories(SemanticZooFixtureTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(SemanticZooFixtureTest
  PRIVATE
    veritas_analysis_pipeline
    veritas_build
    veritas_test_support
    GTest::gtest_main
)
veritas_add_warnings(SemanticZooFixtureTest)
gtest_discover_tests(SemanticZooFixtureTest DISCOVERY_TIMEOUT 180)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_executable(project_analyzer_wpa_integration_test
    ProjectAnalyzerWpaTest.cpp
  )

  target_link_libraries(project_analyzer_wpa_integration_test
    PRIVATE
      veritas_analysis
      veritas_test_support
      veritas_unit_test_support
  )

  add_test(NAME ProjectAnalyzerWpaTest
    COMMAND project_analyzer_wpa_integration_test)
  set_tests_properties(ProjectAnalyzerWpaTest PROPERTIES
    TIMEOUT 120
    LABELS "integration;analysis;m9-entry;souffle-production"
  )
endif()
