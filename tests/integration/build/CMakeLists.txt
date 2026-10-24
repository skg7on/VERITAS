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

# tests/integration/build/CMakeLists.txt
#
# Integration tests for the M1 project manifest loader. These tests read
# real fixtures under tests/fixtures/projects and exercise the Clang
# LibTooling compilation-database code path.

add_executable(veritas_integration_build_tests
  ProjectManifestLoaderTest.cpp
  VeritasBuildAnalyzeCliTest.cpp
)
# `LLVMTargetParser` is named directly because `veritas_build` links LLVM
# PRIVATE, so nothing propagates it: ProjectManifestLoaderTest asserts that a
# project naming no target records the analysis host's default triple, and the
# only honest oracle for that is `llvm::sys::getDefaultTargetTriple()` itself.
#
# `LLVM` and `${LLVM_INCLUDE_DIRS}` are for `VeritasBuildAnalyzeCliTest`, which
# parses the run-metrics artifact with `llvm::json::parse` — the only way to
# assert that the root phase is the real folded span rather than the synthetic
# fallback of the same name. Linking `LLVM` brings the library and no include
# directories, so the tree is spelled out: without it `<llvm/Support/JSON.h>`
# resolves against whatever LLVM headers sit on the prefix (Homebrew's LLVM
# here) while linking the LLVM this build was configured with. Both include
# trees are SYSTEM for the same reason they are in src/build and
# src/observability: upstream headers are not held to this project's -Werror
# warning contract.
target_include_directories(veritas_integration_build_tests SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(veritas_integration_build_tests PRIVATE
  veritas_build
  veritas_test_support
  LLVM
  LLVMTargetParser
  GTest::gtest_main
)
target_compile_definitions(veritas_integration_build_tests PRIVATE
  VERITAS_BUILD_BINARY="$<TARGET_FILE:veritas-build>"
)
add_dependencies(veritas_integration_build_tests veritas-build)
veritas_add_warnings(veritas_integration_build_tests)
gtest_discover_tests(veritas_integration_build_tests DISCOVERY_TIMEOUT 60)
