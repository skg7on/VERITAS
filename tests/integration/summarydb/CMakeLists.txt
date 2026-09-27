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

# tests/integration/summarydb/CMakeLists.txt
#
# Integration tests that exercise MetadataStore against real fixture
# manifests loaded through the M1 pipeline.

add_executable(ManifestPersistenceTest ManifestPersistenceTest.cpp)
target_link_libraries(ManifestPersistenceTest PRIVATE
  veritas_summarydb
  veritas_build
  veritas_test_support
  SQLite::SQLite3
  GTest::gtest_main
)
veritas_add_warnings(ManifestPersistenceTest)
gtest_discover_tests(ManifestPersistenceTest DISCOVERY_TIMEOUT 60)

add_executable(VeritasDiffTest VeritasDiffTest.cpp)
target_link_libraries(VeritasDiffTest PRIVATE
  veritas_summarydb
  veritas::summary
  GTest::gtest_main
)
target_compile_definitions(VeritasDiffTest PRIVATE
  VERITAS_DIFF_BINARY="$<TARGET_FILE:veritas-diff>"
)
add_dependencies(VeritasDiffTest veritas-diff)
veritas_add_warnings(VeritasDiffTest)
gtest_discover_tests(VeritasDiffTest DISCOVERY_TIMEOUT 60)

# `veritas-store-diff`'s exit-code contract (0 equivalent / 1 different /
# 2 could not compare) is what M14's harness branches on, so it is asserted
# against the built binary rather than trusted to a transcript.
#
# An explicit `add_test` rather than `gtest_discover_tests`: the latter
# registers each discovered case with the default timeout and offers no
# per-test one, and this binary spawns subprocesses. The two registrations
# would also double-register the binary, so only one of them is used.
add_executable(VeritasStoreDiffTest VeritasStoreDiffTest.cpp)
target_link_libraries(VeritasStoreDiffTest PRIVATE
  veritas_summarydb
  GTest::gtest_main
)
target_compile_definitions(VeritasStoreDiffTest PRIVATE
  VERITAS_STORE_DIFF_BINARY="$<TARGET_FILE:veritas-store-diff>"
)
add_dependencies(VeritasStoreDiffTest veritas-store-diff)
veritas_add_warnings(VeritasStoreDiffTest)
add_test(NAME VeritasStoreDiffTest COMMAND VeritasStoreDiffTest)
set_tests_properties(VeritasStoreDiffTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;summarydb;store-diff-cli"
)

add_executable(SummaryVersionCompatibilityTest SummaryVersionCompatibilityTest.cpp)
target_link_libraries(SummaryVersionCompatibilityTest PRIVATE
  veritas_summarydb
  veritas::summary
  GTest::gtest_main
)
veritas_add_warnings(SummaryVersionCompatibilityTest)
gtest_discover_tests(SummaryVersionCompatibilityTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(SummaryVersionCompatibilityTest summary-v2)
