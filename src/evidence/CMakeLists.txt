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

# src/evidence/CMakeLists.txt
#
# Builds `veritas_evidence`, the M10B evidence slice, completion-fact, and
# diagnostic-JSON contract shared by the query service and the CLI.

add_library(veritas_evidence
  SliceTypes.cpp
  QueryCompletion.cpp
)

target_include_directories(veritas_evidence
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(veritas_evidence
  PUBLIC
    veritas::core
    veritas::facts
    veritas::cpg
)

target_compile_features(veritas_evidence PUBLIC cxx_std_20)

veritas_add_warnings(veritas_evidence)

add_library(veritas::evidence ALIAS veritas_evidence)
