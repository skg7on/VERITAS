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

# tests/unit/build/CMakeLists.txt
#
# Unit tests for the veritas_build library.

add_executable(ProjectInputTest ProjectInputTest.cpp)
target_link_libraries(ProjectInputTest PRIVATE
  veritas_build
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(ProjectInputTest)
gtest_discover_tests(ProjectInputTest DISCOVERY_TIMEOUT 60)

add_executable(AnalysisManifestTest AnalysisManifestTest.cpp)
target_link_libraries(AnalysisManifestTest PRIVATE
  veritas_build
  GTest::gtest_main
)
veritas_add_warnings(AnalysisManifestTest)
gtest_discover_tests(AnalysisManifestTest DISCOVERY_TIMEOUT 60)
