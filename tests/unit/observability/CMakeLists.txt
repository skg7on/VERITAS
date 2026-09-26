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

# `RunReportTest` reads the artifact back with `llvm::json::parse`, which is the
# only way to assert that what the renderer wrote is a JSON document rather than
# merely a string that looks like one. `veritas_observability` links LLVM
# PRIVATE, so nothing propagates to this target and the dependency is named
# here. The include tree is SYSTEM for the same reason it is in
# src/observability: upstream headers are not held to this project's -Werror
# warning contract, and LLVM's imported targets carry no include directories of
# their own, so `${LLVM_INCLUDE_DIRS}` is spelled out.
add_executable(RunReportTest RunReportTest.cpp)
target_include_directories(RunReportTest SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(RunReportTest PRIVATE
  veritas_observability
  LLVM
  GTest::gtest_main
)
veritas_add_warnings(RunReportTest)
gtest_discover_tests(RunReportTest DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "observability-unit")
