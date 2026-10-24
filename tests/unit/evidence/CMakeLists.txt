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

# tests/unit/evidence/CMakeLists.txt
#
# One executable for the evidence suites. Discovery is split into one call per
# label set so each case keeps the labels it had when every suite had its own
# binary; TEST_FILTER keeps the calls disjoint.

add_executable(veritas_unit_evidence_tests
  EvidenceContractTest.cpp
  EvidenceQueryServiceTest.cpp
  EvidenceHandoffTest.cpp
  EvidenceCaseTest.cpp
  EvidenceCanonicalizerTest.cpp
  EvidenceValidatorTest.cpp
  EvidenceProtoTest.cpp
  EirLexerTest.cpp
  EirParserTest.cpp
  EirWriterTest.cpp
  EvidencePredicateMapperTest.cpp
  EvidenceCaseBuilderTest.cpp
  EvidenceJsonTest.cpp
)
# The token model and the lexer/parser are private to src/evidence; only
# `EirParseError` is installed under include/.
target_include_directories(veritas_unit_evidence_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
# `LLVMSupport` is named directly because `veritas_evidence` links it PRIVATE:
# EvidenceJsonTest calls `llvm::json::parse` to read the document back. The
# include tree is SYSTEM for the same reason it is in src/evidence — upstream
# headers are not held to this project's warning contract.
target_include_directories(veritas_unit_evidence_tests SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(veritas_unit_evidence_tests PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
  LLVMSupport
)
veritas_add_warnings(veritas_unit_evidence_tests)

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  # EvidenceContractTest.cpp also instantiates the value-parameterized
  # InvalidQueryResultMetadataTest suite under the InvalidMetadata prefix.
  TEST_FILTER "EvidenceContractTest.*:InvalidMetadata/*"
  PROPERTIES LABELS "evidence-contract")

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  TEST_FILTER "EvidenceQueryServiceTest.*"
  PROPERTIES LABELS "evidence-unit")

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  TEST_FILTER "EvidenceHandoffTest.*:EvidenceCaseTest.*:EvidenceCanonicalizerTest.*:EvidenceValidatorTest.*:EvidencePredicateMapperTest.*:EvidenceCaseBuilderTest.*"
  PROPERTIES LABELS "evidence-unit\;evidence-contract")

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  TEST_FILTER "EvidenceProtoTest.*"
  PROPERTIES LABELS "evidence-unit\;evidence-roundtrip")

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  TEST_FILTER "EirLexerTest.*:EirParserTest.*:EirWriterTest.*"
  PROPERTIES LABELS "evidence-unit\;evidence-eir-text")

gtest_discover_tests(veritas_unit_evidence_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  TEST_FILTER "EvidenceJsonTest.*"
  PROPERTIES LABELS "evidence-unit\;evidence-json")
