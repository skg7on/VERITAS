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

# src/cpg/CMakeLists.txt
#
# Builds `veritas_cpg`, the thin in-memory Code Property Graph and its
# deterministic canonicalizer.

add_library(veritas_cpg
  ThinCpg.cpp
  CpgCanonicalizer.cpp
  CpgRepository.cpp
)

target_include_directories(veritas_cpg
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(veritas_cpg
  PUBLIC
    veritas::core
  PRIVATE
    veritas_summarydb
)

target_compile_features(veritas_cpg PUBLIC cxx_std_20)

veritas_add_warnings(veritas_cpg)

add_library(veritas::cpg ALIAS veritas_cpg)
