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

add_executable(WpaInputMaterializerTest WpaInputMaterializerTest.cpp)
target_link_libraries(WpaInputMaterializerTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaInputMaterializerTest)
gtest_discover_tests(WpaInputMaterializerTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(WpaInputMaterializerTest relations-v2)

add_executable(CallGraphTest CallGraphTest.cpp)
target_link_libraries(CallGraphTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(CallGraphTest)
gtest_discover_tests(CallGraphTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(CallGraphTest indirect-calls)

add_executable(SccGraphTest SccGraphTest.cpp)
target_link_libraries(SccGraphTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(SccGraphTest)
gtest_discover_tests(SccGraphTest DISCOVERY_TIMEOUT 60)

add_executable(SccStateRepositoryTest SccStateRepositoryTest.cpp)
target_link_libraries(SccStateRepositoryTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(SccStateRepositoryTest)
gtest_discover_tests(SccStateRepositoryTest DISCOVERY_TIMEOUT 60)

add_executable(WpaCoordinatorTest WpaCoordinatorTest.cpp)
target_link_libraries(WpaCoordinatorTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaCoordinatorTest)
gtest_discover_tests(WpaCoordinatorTest DISCOVERY_TIMEOUT 60)

add_executable(WpaExecutorTest WpaExecutorTest.cpp)
target_link_libraries(WpaExecutorTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaExecutorTest)
gtest_discover_tests(WpaExecutorTest DISCOVERY_TIMEOUT 60)

add_executable(CppRuleEvaluatorTest CppRuleEvaluatorTest.cpp)
target_link_libraries(CppRuleEvaluatorTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(CppRuleEvaluatorTest)
gtest_discover_tests(CppRuleEvaluatorTest DISCOVERY_TIMEOUT 60)

add_executable(RelationIoTest RelationIoTest.cpp)
target_link_libraries(RelationIoTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(RelationIoTest)
gtest_discover_tests(RelationIoTest DISCOVERY_TIMEOUT 60)

add_executable(WpaRunRepositoryTest WpaRunRepositoryTest.cpp)
target_link_libraries(WpaRunRepositoryTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaRunRepositoryTest)
gtest_discover_tests(WpaRunRepositoryTest DISCOVERY_TIMEOUT 60)

add_executable(WpaOrchestratorTest WpaOrchestratorTest.cpp)
target_link_libraries(WpaOrchestratorTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaOrchestratorTest)
gtest_discover_tests(WpaOrchestratorTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(WpaOrchestratorTest failure-atomicity)
