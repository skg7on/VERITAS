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

# tests/unit/runtime/CMakeLists.txt
#
# Unit tests for veritas_runtime.

add_executable(WorklistSchedulerTest WorklistSchedulerTest.cpp)
target_link_libraries(WorklistSchedulerTest PRIVATE
  veritas_runtime
  GTest::gtest_main
)
veritas_add_warnings(WorklistSchedulerTest)
gtest_discover_tests(WorklistSchedulerTest DISCOVERY_TIMEOUT 60)
