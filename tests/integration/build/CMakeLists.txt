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

add_executable(ProjectManifestLoaderTest ProjectManifestLoaderTest.cpp)
# `LLVMTargetParser` is named directly because `veritas_build` links LLVM
# PRIVATE, so nothing propagates it: the suite asserts that a project naming no
# target records the analysis host's default triple, and the only honest oracle
# for that is `llvm::sys::getDefaultTargetTriple()` itself. The include tree is
# SYSTEM for the same reason it is in src/build — upstream headers are not held
# to this project's warning contract.
target_include_directories(ProjectManifestLoaderTest SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(ProjectManifestLoaderTest PRIVATE
  veritas_build
  veritas_test_support
  GTest::gtest_main
  LLVMTargetParser
)
veritas_add_warnings(ProjectManifestLoaderTest)
gtest_discover_tests(ProjectManifestLoaderTest DISCOVERY_TIMEOUT 60)

# `VeritasBuildAnalyzeCliTest` parses the run-metrics artifact with
# `llvm::json::parse`, which is the only way to assert that the root phase is
# the real folded span rather than the synthetic fallback of the same name.
# Linking `LLVM` brings the library and no include directories, so
# `${LLVM_INCLUDE_DIRS}` is spelled out: without it `<llvm/Support/JSON.h>`
# resolves against whatever other LLVM headers sit on the prefix (Homebrew's
# llvm@17 here) while linking the LLVM this build was configured with. The tree
# is SYSTEM for the same reason it is in src/observability: upstream headers are
# not held to this project's -Werror warning contract.
add_executable(VeritasBuildAnalyzeCliTest VeritasBuildAnalyzeCliTest.cpp)
target_include_directories(VeritasBuildAnalyzeCliTest SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(VeritasBuildAnalyzeCliTest PRIVATE
  veritas_test_support
  LLVM
  GTest::gtest_main
)
target_compile_definitions(VeritasBuildAnalyzeCliTest PRIVATE
  VERITAS_BUILD_BINARY="$<TARGET_FILE:veritas-build>"
)
add_dependencies(VeritasBuildAnalyzeCliTest veritas-build)
veritas_add_warnings(VeritasBuildAnalyzeCliTest)
gtest_discover_tests(VeritasBuildAnalyzeCliTest DISCOVERY_TIMEOUT 60)
