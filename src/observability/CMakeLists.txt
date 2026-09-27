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

# src/observability/CMakeLists.txt
#
# Builds `veritas_observability`: the run-report model, its two renderers, and
# the store/environment collectors that fill it. The public headers include
# nothing from LLVM, so LLVM is a PRIVATE link — only RunReport.cpp needs
# `llvm::json::OStream`, and a consumer that merely fills a RunReport compiles
# without LLVM headers on its include path. The store connection the collectors
# need is likewise PRIVATE, so a consumer that only renders a report links no
# storage at all.

add_library(veritas_observability
  RunReport.cpp
  StoreSummary.cpp
)

target_include_directories(veritas_observability PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(veritas_observability PUBLIC cxx_std_20)

# The JSON writer is the one member of this library that uses an LLVM header.
# The include tree is SYSTEM so the strict warning contract does not flag
# upstream headers.
target_include_directories(veritas_observability SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(veritas_observability
  PUBLIC
    veritas::core
  PRIVATE
    LLVM
    veritas_summarydb
)

# The environment block carries the build identity, so this library needs its
# own compile definitions. It deliberately does NOT reuse
# `VERITAS_CPP_BUILD_FINGERPRINT` from src/analysis: that string feeds
# `CppToolchainIdentity`, which feeds `engine_toolchain_identity`, which feeds
# `run_id` and every digest chained off it — sharing one spelling would move
# every content-addressed identity in the project.
target_compile_definitions(veritas_observability PRIVATE
  VERITAS_OBSERVABILITY_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  VERITAS_OBSERVABILITY_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
  VERITAS_OBSERVABILITY_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
)

veritas_add_warnings(veritas_observability)

add_library(veritas::observability ALIAS veritas_observability)
