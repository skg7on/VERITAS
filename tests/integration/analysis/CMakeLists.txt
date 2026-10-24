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

# tests/integration/analysis/CMakeLists.txt

set(veritas_integration_analysis_sources
  ProjectAnalyzerTest.cpp
  LocalAnalysisStageTest.cpp
  SummaryV2PipelineTest.cpp
  SouffleProvenanceTest.cpp
  WpaEmergencyModeTest.cpp
  SemanticMemorySvfTest.cpp
  SemanticMemorySummaryDbTest.cpp
  SemanticDispatchSvfTest.cpp
  SemanticZooFixtureTest.cpp
  PhaseObservabilityIdentityTest.cpp
)
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  list(APPEND veritas_integration_analysis_sources ProjectAnalyzerWpaTest.cpp)
endif()

add_executable(veritas_integration_analysis_tests
  ${veritas_integration_analysis_sources}
)
target_include_directories(veritas_integration_analysis_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
# `veritas_summarydb` is named explicitly because the published-table digest
# case opens `<output_root>/metadata.db` itself to dump the four published
# tables; `veritas_analysis` links it PRIVATE, so it is not reachable through it.
target_link_libraries(veritas_integration_analysis_tests
  PRIVATE
    veritas_analysis
    veritas_analysis_pipeline
    veritas_build
    veritas_summarydb
    veritas_wpa
    veritas_wpa_fixture_support
    veritas_test_support
    veritas_unit_test_support
)
veritas_add_warnings(veritas_integration_analysis_tests)

# Suites that were registered through discovery keep one CTest entry per case.
# The whole-file suites below are excluded here and registered explicitly so
# their per-suite timeout and labels stay intact.
# SemanticDispatchSvfTest.cpp also defines the CallGraphTest and SccGraphTest
# suites; they were discovered by its old per-file binary and must stay
# registered here.
gtest_discover_tests(veritas_integration_analysis_tests DISCOVERY_TIMEOUT 180
  TEST_FILTER "SouffleProvenanceTest.*:SemanticMemorySvfTest.*:SemanticMemorySummaryDbTest.*:SemanticDispatchSvfTest.*:SemanticZooFixtureTest.*:CallGraphTest.*:SccGraphTest.*"
)

add_test(NAME ProjectAnalyzerTest
  COMMAND veritas_integration_analysis_tests
    --gtest_filter=ProjectAnalyzerTest.*)
set_tests_properties(ProjectAnalyzerTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;analysis"
)

add_test(NAME LocalAnalysisStageTest
  COMMAND veritas_integration_analysis_tests
    --gtest_filter=LocalAnalysisStageTest.*)
set_tests_properties(LocalAnalysisStageTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;analysis"
)

add_test(NAME SummaryV2PipelineTest
  COMMAND veritas_integration_analysis_tests
    --gtest_filter=SummaryV2PipelineTest.*)
set_tests_properties(SummaryV2PipelineTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis"
)

add_test(NAME WpaEmergencyModeTest
  COMMAND veritas_integration_analysis_tests
    --gtest_filter=WpaEmergencyModeTest.*)
set_tests_properties(WpaEmergencyModeTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis;wpa"
)

add_test(NAME PhaseObservabilityIdentityTest
  COMMAND veritas_integration_analysis_tests
    --gtest_filter=PhaseObservabilityIdentityTest.*)
set_tests_properties(PhaseObservabilityIdentityTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis"
)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_test(NAME ProjectAnalyzerWpaTest
    COMMAND veritas_integration_analysis_tests
      --gtest_filter=ProjectAnalyzerWpaTest.*)
  set_tests_properties(ProjectAnalyzerWpaTest PROPERTIES
    TIMEOUT 120
    LABELS "integration;analysis;m9-entry;souffle-production"
  )
endif()

add_subdirectory(svf)
add_subdirectory(cpg)
