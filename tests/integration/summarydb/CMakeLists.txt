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

add_executable(veritas_integration_summarydb_tests
  ManifestPersistenceTest.cpp
  VeritasDiffTest.cpp
  VeritasStoreDiffTest.cpp
  SummaryVersionCompatibilityTest.cpp
)
target_link_libraries(veritas_integration_summarydb_tests PRIVATE
  veritas_summarydb
  veritas::summary
  veritas_build
  veritas_test_support
  SQLite::SQLite3
  GTest::gtest_main
)
target_compile_definitions(veritas_integration_summarydb_tests PRIVATE
  VERITAS_DIFF_BINARY="$<TARGET_FILE:veritas-diff>"
  VERITAS_STORE_DIFF_BINARY="$<TARGET_FILE:veritas-store-diff>"
)
add_dependencies(veritas_integration_summarydb_tests
  veritas-diff
  veritas-store-diff
)
veritas_add_warnings(veritas_integration_summarydb_tests)

# `veritas-store-diff`'s exit-code contract (0 equivalent / 1 different /
# 2 could not compare) is what M14's harness branches on, so it is asserted
# against the built binary rather than trusted to a transcript.
#
# An explicit `add_test` rather than discovery: the suite spawns subprocesses
# and needs a per-test timeout, which discovery cannot give. Discovery excludes
# that suite so the two registrations cannot overlap.
gtest_discover_tests(veritas_integration_summarydb_tests DISCOVERY_TIMEOUT 60
  TEST_FILTER "ManifestPersistenceTest.*:VeritasDiffTest.*:SummaryVersionCompatibilityTest.*"
)
add_test(NAME VeritasStoreDiffTest
  COMMAND veritas_integration_summarydb_tests
    --gtest_filter=VeritasStoreDiffTest.*
)
set_tests_properties(VeritasStoreDiffTest PROPERTIES
  TIMEOUT 120
  LABELS "integration;summarydb;store-diff-cli"
)

veritas_m9_entry_test(SummaryVersionCompatibilityTest summary-v2
  veritas_integration_summarydb_tests)
