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

# tests/support/CMakeLists.txt
#
# Shared test-only helpers. `VERITAS_TESTS_SOURCE_DIR` is baked in as a
# compile definition so fixtures can be located regardless of CWD.

add_library(veritas_test_support STATIC
  ProjectFixture.cpp
)

target_include_directories(veritas_test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(veritas_test_support PRIVATE
  VERITAS_TESTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}/tests"
)

target_compile_features(veritas_test_support PUBLIC cxx_std_20)

veritas_add_warnings(veritas_test_support)

add_library(veritas::test_support ALIAS veritas_test_support)

add_library(veritas_wpa_fixture_support STATIC
  WpaFixtureHarness.cpp
)

target_include_directories(veritas_wpa_fixture_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(veritas_wpa_fixture_support
  PUBLIC
    veritas_analysis
    veritas_svf_analysis
    veritas_analysis_pipeline
    veritas_build
    veritas_summarydb
    veritas_test_support
)

target_compile_features(veritas_wpa_fixture_support PUBLIC cxx_std_20)

veritas_add_warnings(veritas_wpa_fixture_support)
