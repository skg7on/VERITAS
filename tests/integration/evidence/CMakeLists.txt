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

# tests/integration/evidence/CMakeLists.txt
#
# Real-pipeline evidence integration: run the full M1->M6->M9->M10A pipeline on
# the overflow fixtures, then query the durable FactStore/ProvenanceStore and
# CPG through the concrete FactStoreEvidenceBackend.
#
# Discovery is split per label set so each case keeps the labels it had when
# every suite had its own binary; TEST_FILTER keeps the calls disjoint.

add_executable(veritas_integration_evidence_tests
  OverflowEvidenceFixtureTest.cpp
  EvidenceHandoffIntegrationTest.cpp
  VeritasQueryEvidenceTest.cpp
  VeritasQueryEirTest.cpp
)
target_link_libraries(veritas_integration_evidence_tests PRIVATE
  veritas::evidence
  veritas_wpa_fixture_support
  GTest::gtest_main
)
target_compile_definitions(veritas_integration_evidence_tests PRIVATE
  VERITAS_QUERY_BINARY="$<TARGET_FILE:veritas-query>"
  VERITAS_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/evidence"
)
add_dependencies(veritas_integration_evidence_tests veritas-query)
veritas_add_warnings(veritas_integration_evidence_tests)

gtest_discover_tests(veritas_integration_evidence_tests DISCOVERY_TIMEOUT 120
  TEST_FILTER "OverflowEvidenceFixtureTest.*:EvidenceHandoffIntegrationTest.*"
  PROPERTIES LABELS "evidence-integration")

gtest_discover_tests(veritas_integration_evidence_tests DISCOVERY_TIMEOUT 120
  TEST_FILTER "VeritasQueryEvidenceTest.*"
  PROPERTIES LABELS "evidence-integration\;evidence-cli")

gtest_discover_tests(veritas_integration_evidence_tests DISCOVERY_TIMEOUT 120
  TEST_FILTER "VeritasQueryEirTest.*"
  PROPERTIES LABELS "evidence-integration\;evidence-cli\;evidence-roundtrip")
