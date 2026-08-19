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

# tests/integration/build/CMakeLists.txt
#
# Integration tests for the M1 project manifest loader. These tests read
# real fixtures under tests/fixtures/projects and exercise the Clang
# LibTooling compilation-database code path.

add_executable(ProjectManifestLoaderTest ProjectManifestLoaderTest.cpp)
target_link_libraries(ProjectManifestLoaderTest PRIVATE
  veritas_build
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(ProjectManifestLoaderTest)
gtest_discover_tests(ProjectManifestLoaderTest DISCOVERY_TIMEOUT 60)

add_executable(VeritasBuildAnalyzeCliTest VeritasBuildAnalyzeCliTest.cpp)
target_link_libraries(VeritasBuildAnalyzeCliTest PRIVATE
  veritas_test_support
  GTest::gtest_main
)
target_compile_definitions(VeritasBuildAnalyzeCliTest PRIVATE
  VERITAS_BUILD_BINARY="$<TARGET_FILE:veritas-build>"
)
add_dependencies(VeritasBuildAnalyzeCliTest veritas-build)
veritas_add_warnings(VeritasBuildAnalyzeCliTest)
gtest_discover_tests(VeritasBuildAnalyzeCliTest DISCOVERY_TIMEOUT 60)
