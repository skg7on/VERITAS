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

add_executable(EvidenceCaseTest EvidenceCaseTest.cpp)
target_link_libraries(EvidenceCaseTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceCaseTest)
gtest_discover_tests(EvidenceCaseTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-contract")

add_executable(EvidenceCanonicalizerTest EvidenceCanonicalizerTest.cpp)
target_link_libraries(EvidenceCanonicalizerTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceCanonicalizerTest)
gtest_discover_tests(EvidenceCanonicalizerTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-contract")

add_executable(EvidenceValidatorTest EvidenceValidatorTest.cpp)
target_link_libraries(EvidenceValidatorTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceValidatorTest)
gtest_discover_tests(EvidenceValidatorTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-contract")

add_executable(EvidenceProtoTest EvidenceProtoTest.cpp)
target_link_libraries(EvidenceProtoTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EvidenceProtoTest)
gtest_discover_tests(EvidenceProtoTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-roundtrip")

add_executable(EirLexerTest EirLexerTest.cpp)
# The token model and the lexer are private to src/evidence; only
# `EirParseError` is installed under include/.
target_include_directories(EirLexerTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(EirLexerTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EirLexerTest)
gtest_discover_tests(EirLexerTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-eir-text")

add_executable(EirParserTest EirParserTest.cpp)
# The token model, the lexer, and the parser are private to src/evidence; only
# `EirParseError` is installed under include/. The integer-range test drives
# `EirParser` from a lexed token vector, so the test needs the same private
# include the lexer suite uses.
target_include_directories(EirParserTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(EirParserTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EirParserTest)
gtest_discover_tests(EirParserTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-eir-text")

add_executable(EirWriterTest EirWriterTest.cpp)
# The canonical writer consumes only public headers: `ParseEirText`,
# `WriteEirText`, the validator, and the canonicalizer. The lexer's token model
# is deliberately not part of the contract this suite asserts against.
target_link_libraries(EirWriterTest PRIVATE
  veritas::evidence
  veritas::test_support
  GTest::gtest_main
)
veritas_add_warnings(EirWriterTest)
gtest_discover_tests(EirWriterTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "evidence-unit\;evidence-eir-text")
