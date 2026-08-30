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

add_executable(WpaEndToEndTest WpaEndToEndTest.cpp)
target_link_libraries(WpaEndToEndTest PRIVATE
  veritas::summarydb
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaEndToEndTest)
gtest_discover_tests(WpaEndToEndTest DISCOVERY_TIMEOUT 60)

if(VERITAS_BUILD_SOUFFLE)
  add_executable(WpaExecutorConformanceTest WpaExecutorConformanceTest.cpp)
  target_link_libraries(WpaExecutorConformanceTest PRIVATE
    veritas::wpa
    GTest::gtest_main
  )
  target_compile_definitions(WpaExecutorConformanceTest PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
  )
  add_dependencies(WpaExecutorConformanceTest veritas_souffle_worker)
  veritas_add_warnings(WpaExecutorConformanceTest)
  gtest_discover_tests(WpaExecutorConformanceTest DISCOVERY_TIMEOUT 60)
endif()
