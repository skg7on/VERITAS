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

add_executable(SccGraphTest SccGraphTest.cpp)
target_link_libraries(SccGraphTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(SccGraphTest)
gtest_discover_tests(SccGraphTest DISCOVERY_TIMEOUT 60)

add_executable(FixpointEngineTest FixpointEngineTest.cpp)
target_link_libraries(FixpointEngineTest PRIVATE
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(FixpointEngineTest)
gtest_discover_tests(FixpointEngineTest DISCOVERY_TIMEOUT 60)

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
