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

# tests/unit/analysis/CMakeLists.txt

add_executable(veritas_unit_analysis_tests
  SemanticTypesTest.cpp
  ModelBundleTest.cpp
  AnalysisConfigTest.cpp
)
target_link_libraries(veritas_unit_analysis_tests PRIVATE
  veritas_analysis_semantic
  veritas_analysis
  veritas::core
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(veritas_unit_analysis_tests)
gtest_discover_tests(veritas_unit_analysis_tests DISCOVERY_TIMEOUT 60)

add_subdirectory(llvm)
add_subdirectory(svf)
