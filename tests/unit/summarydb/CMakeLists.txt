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

# tests/unit/summarydb/CMakeLists.txt
#
# Unit tests for veritas_summarydb.

add_executable(MetadataStoreTest MetadataStoreTest.cpp)
target_link_libraries(MetadataStoreTest PRIVATE
  veritas_summarydb
  GTest::gtest_main
)
veritas_add_warnings(MetadataStoreTest)
gtest_discover_tests(MetadataStoreTest DISCOVERY_TIMEOUT 60)

add_executable(ObjectStoreTest ObjectStoreTest.cpp)
target_link_libraries(ObjectStoreTest PRIVATE
  veritas_summarydb
  GTest::gtest_main
)
veritas_add_warnings(ObjectStoreTest)
gtest_discover_tests(ObjectStoreTest DISCOVERY_TIMEOUT 60)

add_executable(SummaryRepositoryTest SummaryRepositoryTest.cpp)
target_link_libraries(SummaryRepositoryTest PRIVATE
  veritas_summarydb
  veritas::summary
  GTest::gtest_main
)
veritas_add_warnings(SummaryRepositoryTest)
gtest_discover_tests(SummaryRepositoryTest DISCOVERY_TIMEOUT 60)

add_executable(SummaryDeltaTest SummaryDeltaTest.cpp)
target_link_libraries(SummaryDeltaTest PRIVATE
  veritas_summarydb
  veritas::summary
  GTest::gtest_main
)
veritas_add_warnings(SummaryDeltaTest)
gtest_discover_tests(SummaryDeltaTest DISCOVERY_TIMEOUT 60)

add_executable(DependencyIndexTest DependencyIndexTest.cpp)
target_link_libraries(DependencyIndexTest PRIVATE
  veritas_summarydb
  veritas::summary
  GTest::gtest_main
)
veritas_add_warnings(DependencyIndexTest)
gtest_discover_tests(DependencyIndexTest DISCOVERY_TIMEOUT 60)
