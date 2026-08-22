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

add_executable(AnalysisRunTest AnalysisRunTest.cpp)
target_link_libraries(AnalysisRunTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(AnalysisRunTest)
gtest_discover_tests(AnalysisRunTest DISCOVERY_TIMEOUT 60)

add_executable(RelationSchemaTest RelationSchemaTest.cpp)
target_link_libraries(RelationSchemaTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(RelationSchemaTest)
gtest_discover_tests(RelationSchemaTest DISCOVERY_TIMEOUT 60)

add_executable(AnalysisFactTest AnalysisFactTest.cpp)
target_link_libraries(AnalysisFactTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(AnalysisFactTest)
gtest_discover_tests(AnalysisFactTest DISCOVERY_TIMEOUT 60)

add_executable(FactSchemaTest FactSchemaTest.cpp)
target_link_libraries(FactSchemaTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(FactSchemaTest)
gtest_discover_tests(FactSchemaTest DISCOVERY_TIMEOUT 60)

add_executable(SummaryFactBuilderTest SummaryFactBuilderTest.cpp)
target_link_libraries(SummaryFactBuilderTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(SummaryFactBuilderTest)
gtest_discover_tests(SummaryFactBuilderTest DISCOVERY_TIMEOUT 60)

add_executable(SouffleExporterTest SouffleExporterTest.cpp)
target_link_libraries(SouffleExporterTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(SouffleExporterTest)
gtest_discover_tests(SouffleExporterTest DISCOVERY_TIMEOUT 60)
