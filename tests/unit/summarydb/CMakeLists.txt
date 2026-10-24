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

add_executable(veritas_unit_summarydb_tests
  MetadataStoreTest.cpp
  ObjectStoreTest.cpp
  SummaryRepositoryTest.cpp
  SummaryDeltaTest.cpp
  DependencyIndexTest.cpp
  StoreEquivalenceTest.cpp
  StoreEquivalenceEndToEndTest.cpp
)
target_link_libraries(veritas_unit_summarydb_tests PRIVATE
  veritas_summarydb
  veritas::summary
  veritas_analysis
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(veritas_unit_summarydb_tests)

# The end-to-end case runs two full M1->M4->M5->M3 analyses in-process, so it
# needs a per-test timeout. `gtest_discover_tests` registers its cases with the
# default timeout, hence the explicit `add_test` + `set_tests_properties` form
# below. Discovery excludes that suite so the two registrations cannot overlap.
gtest_discover_tests(veritas_unit_summarydb_tests DISCOVERY_TIMEOUT 60
  TEST_FILTER "MetadataStoreTest.*:ObjectStoreTest.*:SummaryRepositoryTest.*:SummaryDeltaTest.*:DependencyIndexTest.*:StoreEquivalenceTest.*"
)

add_test(NAME StoreEquivalenceEndToEndTest
  COMMAND veritas_unit_summarydb_tests
    --gtest_filter=StoreEquivalenceEndToEndTest.*
)
set_tests_properties(StoreEquivalenceEndToEndTest PROPERTIES
  TIMEOUT 300
  LABELS "integration;analysis;summarydb"
)
