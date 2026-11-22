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

# tests/unit/query/CMakeLists.txt

add_executable(veritas_unit_query_tests QueryPlannerTest.cpp)
target_link_libraries(veritas_unit_query_tests PRIVATE
  veritas::query
  GTest::gtest_main
)
# The query-seed manifest is the human-readable mirror of the compiled
# registry, exactly as relations.v2.manifest mirrors RelationSchema.cpp.
target_compile_definitions(veritas_unit_query_tests PRIVATE
  VERITAS_LOGIC_DIR="${PROJECT_SOURCE_DIR}/logic"
)
veritas_add_warnings(veritas_unit_query_tests)
gtest_discover_tests(veritas_unit_query_tests DISCOVERY_TIMEOUT 60)
