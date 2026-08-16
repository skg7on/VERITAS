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

# src/build/CMakeLists.txt
#
# Builds `veritas_build`, the M1 project-ingestion library. Depends on
# veritas_core for Status/StatusOr and — starting with the manifest loader —
# on Clang LibTooling for compilation-database parsing.

add_library(veritas_build
  ProjectInput.cpp
)

target_include_directories(veritas_build PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(veritas_build PUBLIC veritas_core)

target_compile_features(veritas_build PUBLIC cxx_std_20)

veritas_add_warnings(veritas_build)

add_library(veritas::build ALIAS veritas_build)
