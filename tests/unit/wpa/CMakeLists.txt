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

# tests/unit/wpa/CMakeLists.txt

set(veritas_unit_wpa_sources
  WpaInputMaterializerTest.cpp
  CallGraphTest.cpp
  SccGraphTest.cpp
  SccStateRepositoryTest.cpp
  WpaCoordinatorTest.cpp
  WpaExecutorTest.cpp
  CppRuleEvaluatorTest.cpp
  RelationIoTest.cpp
  WpaRunRepositoryTest.cpp
  WpaOrchestratorTest.cpp
)

# The session ABI is exported by the runner library, which veritas_wpa links
# PRIVATE and so does not re-export: a test that names its symbols links it
# itself. The suite exists only under the Souffle engine.
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  list(APPEND veritas_unit_wpa_sources SouffleSessionTest.cpp)
endif()

add_executable(veritas_unit_wpa_tests ${veritas_unit_wpa_sources})
target_link_libraries(veritas_unit_wpa_tests PRIVATE
  veritas::wpa
  GTest::gtest_main
)
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  target_link_libraries(veritas_unit_wpa_tests PRIVATE veritas_souffle_runner)
endif()
veritas_add_warnings(veritas_unit_wpa_tests)
gtest_discover_tests(veritas_unit_wpa_tests DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(WpaInputMaterializerTest relations-v2
  veritas_unit_wpa_tests)
veritas_m9_entry_test(CallGraphTest indirect-calls veritas_unit_wpa_tests)
veritas_m9_entry_test(WpaOrchestratorTest failure-atomicity
  veritas_unit_wpa_tests)
