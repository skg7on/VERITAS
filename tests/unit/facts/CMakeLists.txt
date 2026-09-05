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
veritas_m9_entry_test(AnalysisRunTest run-identity)

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
veritas_m9_entry_test(RelationSchemaTest relations-v2)

add_executable(AnalysisFactTest AnalysisFactTest.cpp)
target_link_libraries(AnalysisFactTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(AnalysisFactTest)
gtest_discover_tests(AnalysisFactTest DISCOVERY_TIMEOUT 60)

add_executable(EpistemicTest EpistemicTest.cpp)
target_link_libraries(EpistemicTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(EpistemicTest)
gtest_discover_tests(EpistemicTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(EpistemicTest epistemic-propagation)

add_executable(FactProtoTest FactProtoTest.cpp)
target_link_libraries(FactProtoTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(FactProtoTest)
gtest_discover_tests(FactProtoTest DISCOVERY_TIMEOUT 60)

add_executable(FactStoreTest FactStoreTest.cpp)
target_link_libraries(FactStoreTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(FactStoreTest)
gtest_discover_tests(FactStoreTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(FactStoreTest fact-persistence)

add_executable(ProvenanceStoreTest ProvenanceStoreTest.cpp)
target_link_libraries(ProvenanceStoreTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(ProvenanceStoreTest)
gtest_discover_tests(ProvenanceStoreTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(ProvenanceStoreTest budgeted-explanation)

add_executable(DenseIdMapTest DenseIdMapTest.cpp)
target_link_libraries(DenseIdMapTest PRIVATE
  veritas::facts
  GTest::gtest_main
)
veritas_add_warnings(DenseIdMapTest)
gtest_discover_tests(DenseIdMapTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(DenseIdMapTest stable-identity)

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
veritas_m9_entry_test(ResultCanonicalizerTest witness-closure)

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
veritas_m9_entry_test(AnalysisFactBusTest witness-closure)
