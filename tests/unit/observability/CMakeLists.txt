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

# tests/unit/observability/CMakeLists.txt
#
# `RunReportTest` reads the artifact back with `llvm::json::parse`, which is the
# only way to assert that what the renderer wrote is a JSON document rather than
# merely a string that looks like one. `veritas_observability` links LLVM
# PRIVATE, so nothing propagates to this target and the dependency is named
# here.
#
# `${LLVM_INCLUDE_DIRS}` is spelled out because the LLVM imported targets carry
# no include directories of their own: linking `LLVM` brings the library and
# nothing else. Without this line `<llvm/Support/JSON.h>` still resolves — but
# against whatever other LLVM headers sit on the prefix (Homebrew's LLVM
# here) while linking the LLVM this build was configured with. The tree is
# SYSTEM for the same reason it is in src/observability: upstream headers are
# not held to this project's -Werror warning contract.
#
# `veritas_summarydb` is named directly for `StoreSummaryTest`, which opens a
# store directly in three of its cases. `veritas_build` and
# `veritas_test_support` are for the case that builds a manifest through the
# real M1 ingestion and the `multiple_tus` fixture. `veritas_analysis` is not
# needed: `ProjectAnalysisRequest` is a header-only aggregate and this test
# never runs the analyzer, so no SVF and no WPA are linked or executed.

add_executable(veritas_unit_observability_tests
  RunReportTest.cpp
  StoreSummaryTest.cpp
)
target_include_directories(veritas_unit_observability_tests SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(veritas_unit_observability_tests PRIVATE
  veritas_observability
  veritas_summarydb
  veritas_build
  veritas_test_support
  LLVM
  GTest::gtest_main
)
veritas_add_warnings(veritas_unit_observability_tests)
gtest_discover_tests(veritas_unit_observability_tests DISCOVERY_TIMEOUT 60
  NO_PRETTY_VALUES
  PROPERTIES LABELS "observability-unit")
