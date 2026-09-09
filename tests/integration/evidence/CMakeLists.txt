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

add_executable(OverflowEvidenceFixtureTest OverflowEvidenceFixtureTest.cpp)
target_link_libraries(OverflowEvidenceFixtureTest PRIVATE
  veritas::evidence
  veritas_wpa_fixture_support
  GTest::gtest_main
)
veritas_add_warnings(OverflowEvidenceFixtureTest)
gtest_discover_tests(OverflowEvidenceFixtureTest DISCOVERY_TIMEOUT 120
  PROPERTIES LABELS "evidence-integration")

add_executable(EvidenceHandoffIntegrationTest EvidenceHandoffIntegrationTest.cpp)
target_link_libraries(EvidenceHandoffIntegrationTest PRIVATE
  veritas::evidence
  veritas_wpa_fixture_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceHandoffIntegrationTest)
gtest_discover_tests(EvidenceHandoffIntegrationTest DISCOVERY_TIMEOUT 120
  PROPERTIES LABELS "evidence-integration")
