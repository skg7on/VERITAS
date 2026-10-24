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

# tests/unit/facts/CMakeLists.txt

add_executable(veritas_unit_facts_tests
  AnalysisRunTest.cpp
  RelationSchemaTest.cpp
  AnalysisFactTest.cpp
  EpistemicTest.cpp
  FactProtoTest.cpp
  FactStoreTest.cpp
  ProvenanceStoreTest.cpp
  DenseIdMapTest.cpp
  WitnessTest.cpp
  ResultCanonicalizerTest.cpp
  SemanticKeyCodecTest.cpp
  AnalysisFactBusTest.cpp
)
target_link_libraries(veritas_unit_facts_tests PRIVATE
  veritas::facts
  veritas::wpa
  veritas::summarydb
  GTest::gtest_main
)
target_compile_definitions(veritas_unit_facts_tests PRIVATE
  VERITAS_LOGIC_DIR="${PROJECT_SOURCE_DIR}/logic"
)
veritas_add_warnings(veritas_unit_facts_tests)
gtest_discover_tests(veritas_unit_facts_tests DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(AnalysisRunTest run-identity veritas_unit_facts_tests)
veritas_m9_entry_test(RelationSchemaTest relations-v2 veritas_unit_facts_tests
  RelationSchemaTest.* RelationSchemaManifestTest.*)
veritas_m9_entry_test(EpistemicTest epistemic-propagation
  veritas_unit_facts_tests)
veritas_m9_entry_test(FactStoreTest fact-persistence veritas_unit_facts_tests)
veritas_m9_entry_test(ProvenanceStoreTest budgeted-explanation
  veritas_unit_facts_tests)
veritas_m9_entry_test(DenseIdMapTest stable-identity veritas_unit_facts_tests)
veritas_m9_entry_test(ResultCanonicalizerTest witness-closure
  veritas_unit_facts_tests)
veritas_m9_entry_test(AnalysisFactBusTest witness-closure veritas_unit_facts_tests)
