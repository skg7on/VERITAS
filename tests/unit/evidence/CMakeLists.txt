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

add_executable(EvidenceContractTest EvidenceContractTest.cpp)
target_link_libraries(EvidenceContractTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceContractTest)
gtest_discover_tests(EvidenceContractTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-contract")

add_executable(EvidenceQueryServiceTest EvidenceQueryServiceTest.cpp)
target_link_libraries(EvidenceQueryServiceTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceQueryServiceTest)
gtest_discover_tests(EvidenceQueryServiceTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit")

add_executable(EvidenceHandoffTest EvidenceHandoffTest.cpp)
target_link_libraries(EvidenceHandoffTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceHandoffTest)
gtest_discover_tests(EvidenceHandoffTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-contract")
