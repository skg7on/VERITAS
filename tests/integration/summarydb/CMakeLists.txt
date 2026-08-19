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
