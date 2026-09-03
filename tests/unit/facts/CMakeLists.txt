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
target_compile_definitions(RelationSchemaTest PRIVATE
  VERITAS_LOGIC_DIR="${PROJECT_SOURCE_DIR}/logic"
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

add_executable(DenseIdMapTest DenseIdMapTest.cpp)
target_link_libraries(DenseIdMapTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(DenseIdMapTest)
gtest_discover_tests(DenseIdMapTest DISCOVERY_TIMEOUT 60)

add_executable(LegacyFactAdapterTest LegacyFactAdapterTest.cpp)
target_link_libraries(LegacyFactAdapterTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(LegacyFactAdapterTest)
gtest_discover_tests(LegacyFactAdapterTest DISCOVERY_TIMEOUT 60)

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

add_executable(WitnessTest WitnessTest.cpp)
target_link_libraries(WitnessTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
target_compile_definitions(WitnessTest PRIVATE
  VERITAS_LOGIC_DIR="${PROJECT_SOURCE_DIR}/logic"
)
veritas_add_warnings(WitnessTest)
gtest_discover_tests(WitnessTest DISCOVERY_TIMEOUT 60)

add_executable(ResultCanonicalizerTest ResultCanonicalizerTest.cpp)
target_link_libraries(ResultCanonicalizerTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(ResultCanonicalizerTest)
gtest_discover_tests(ResultCanonicalizerTest DISCOVERY_TIMEOUT 60)

add_executable(SemanticKeyCodecTest SemanticKeyCodecTest.cpp)
target_link_libraries(SemanticKeyCodecTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(SemanticKeyCodecTest)
gtest_discover_tests(SemanticKeyCodecTest DISCOVERY_TIMEOUT 60)

add_executable(AnalysisFactBusTest AnalysisFactBusTest.cpp)
target_link_libraries(AnalysisFactBusTest PRIVATE
  veritas::wpa
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(AnalysisFactBusTest)
gtest_discover_tests(AnalysisFactBusTest DISCOVERY_TIMEOUT 60)
