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


add_executable(veritas_integration_review_tests SnapshotCaptureTest.cpp LocalEvidenceTest.cpp AsanReplayTest.cpp ReviewBackendTest.cpp)
target_link_libraries(veritas_integration_review_tests PRIVATE veritas_review GTest::gtest_main)
veritas_add_warnings(veritas_integration_review_tests)
gtest_discover_tests(veritas_integration_review_tests DISCOVERY_TIMEOUT 60)

target_compile_definitions(veritas_integration_review_tests PRIVATE VERITAS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
