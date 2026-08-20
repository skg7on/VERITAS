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

# src/summary/CMakeLists.txt
#
# Builds `veritas_summary`, the Function Summary IR library.

find_package(Protobuf REQUIRED)

# Create a library for generated protobuf code.
# The proto file uses package veritas.summary.v1, so we route generation
# through protobuf_generate with an explicit IMPORT_DIR set to the proto/
# root. This preserves the directory prefix (veritas/summary/v1/) in the
# generated header path, matching #include "veritas/summary/v1/summary.pb.h".
add_library(veritas_summary_proto OBJECT
  "${CMAKE_SOURCE_DIR}/proto/veritas/summary/v1/summary.proto"
)

target_include_directories(veritas_summary_proto PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(veritas_summary_proto PUBLIC
  protobuf::libprotobuf
  # Protobuf 7.x generated code references Abseil logging/checking symbols
  # directly; CMake's FindProtobuf does not carry these transitively.
  absl::absl_log
  absl::absl_check
)

protobuf_generate(
  TARGET veritas_summary_proto
  IMPORT_DIRS "${CMAKE_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)

# Summary library with component hashing and summary ID computation
add_library(veritas_summary
  ComponentHash.cpp
  FunctionSummary.cpp
  LocalSummaryBuilder.cpp
)

target_include_directories(veritas_summary PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(veritas_summary PUBLIC
  veritas::core
  veritas_summary_proto
)

target_compile_features(veritas_summary PUBLIC cxx_std_20)

veritas_add_warnings(veritas_summary)

add_library(veritas::summary ALIAS veritas_summary)
