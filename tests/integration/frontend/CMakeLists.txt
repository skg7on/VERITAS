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

add_executable(project_ast_extractor_integration_test
  ProjectAstExtractorTest.cpp
)

target_include_directories(project_ast_extractor_integration_test PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(project_ast_extractor_integration_test PRIVATE
  veritas_frontend_clang
  veritas_build
  veritas_test_support
  GTest::gtest_main
)

add_test(NAME ProjectAstExtractorTest
  COMMAND project_ast_extractor_integration_test)
set_tests_properties(ProjectAstExtractorTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;frontend"
)
