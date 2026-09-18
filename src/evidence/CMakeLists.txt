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
# Builds `veritas_evidence`, the M10B evidence slice, completion-fact,
# diagnostic-JSON, and Evidence IR contract shared by the query service and the
# CLI.

find_package(Protobuf REQUIRED)

# Generated protobuf code for the `eir.v1` Evidence IR wire contract. Mirrors
# src/summary and src/facts: IMPORT_DIRS points at proto/ so the generated
# header path keeps the veritas/evidence/v1/ prefix, matching
# #include "veritas/evidence/v1/evidence.pb.h".
add_library(veritas_evidence_proto OBJECT
  "${CMAKE_SOURCE_DIR}/proto/veritas/evidence/v1/evidence.proto"
)

target_include_directories(veritas_evidence_proto PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(veritas_evidence_proto PUBLIC
  protobuf::libprotobuf
)

# Protobuf 22.0+ generated code references Abseil logging/checking symbols
# directly; link them when present (see cmake/Dependencies.cmake).
if(TARGET absl::absl_log)
  target_link_libraries(veritas_evidence_proto PUBLIC
    absl::absl_log
    absl::absl_check
  )
endif()

protobuf_generate(
  TARGET veritas_evidence_proto
  IMPORT_DIRS "${CMAKE_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(veritas_evidence
  SliceTypes.cpp
  QueryCompletion.cpp
  EvidenceQueryService.cpp
  EvidenceCase.cpp
  EvidencePredicateMapper.cpp
  EvidenceCaseBuilder.cpp
  EvidenceValidator.cpp
  EvidenceCanonicalizer.cpp
  EvidenceProto.cpp
  EirLexer.cpp
  EirParser.cpp
  EirWriter.cpp
)

target_include_directories(veritas_evidence
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}"
  PRIVATE
    # EirSyntax.h — the token model and lexer — is private to src/evidence.
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(veritas_evidence
  PUBLIC
    veritas::core
    veritas::facts
    veritas::cpg
    veritas_evidence_proto
)

target_compile_features(veritas_evidence PUBLIC cxx_std_20)

veritas_add_warnings(veritas_evidence)

add_library(veritas::evidence ALIAS veritas_evidence)
