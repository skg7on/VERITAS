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

# tests/integration/analysis/cpg/CMakeLists.txt

add_executable(veritas_integration_analysis_cpg_tests
  CpgEndToEndTest.cpp
  CpgProjectionStageTest.cpp
)
target_include_directories(veritas_integration_analysis_cpg_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(veritas_integration_analysis_cpg_tests PRIVATE
  veritas_analysis
  veritas_analysis_cpg
  veritas_analysis_pipeline
  veritas_build
  veritas_test_support
  veritas::cpg
  veritas::core
  GTest::gtest_main
)

add_test(NAME CpgEndToEndTest
  COMMAND veritas_integration_analysis_cpg_tests
    --gtest_filter=CpgEndToEndTest.*)
set_tests_properties(CpgEndToEndTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis"
)

add_test(NAME CpgProjectionStageTest
  COMMAND veritas_integration_analysis_cpg_tests
    --gtest_filter=CpgProjectionStageTest.*)
set_tests_properties(CpgProjectionStageTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;analysis"
)
