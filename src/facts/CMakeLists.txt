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

find_package(Protobuf REQUIRED)

# Generated protobuf code for the M9 fact/provenance wire contract. Mirrors
# src/summary: IMPORT_DIRS points at proto/ so the generated header path keeps
# the veritas/fact/v1/ prefix, matching #include "veritas/fact/v1/fact.pb.h".
add_library(veritas_fact_proto OBJECT
  "${CMAKE_SOURCE_DIR}/proto/veritas/fact/v1/fact.proto"
)

target_include_directories(veritas_fact_proto PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(veritas_fact_proto PUBLIC
  protobuf::libprotobuf
)

# Protobuf 22.0+ generated code references Abseil logging/checking symbols
# directly; link them when present (see cmake/Dependencies.cmake).
if(TARGET absl::absl_log)
  target_link_libraries(veritas_fact_proto PUBLIC
    absl::absl_log
    absl::absl_check
  )
endif()

protobuf_generate(
  TARGET veritas_fact_proto
  IMPORT_DIRS "${CMAKE_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)

add_library(veritas_facts
  AnalysisRun.cpp
  AnalysisFact.cpp
  RelationSchema.cpp
  ResultCanonicalizer.cpp
  RuleRegistry.cpp
  SemanticKeyCodec.cpp
  Witness.cpp
  AnalysisFactBus.cpp
  Epistemic.cpp
  FactProto.cpp
  FactStore.cpp
  ProvenanceStore.cpp
)

target_include_directories(veritas_facts PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(veritas_facts SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(veritas_facts PUBLIC
  veritas::core
  veritas::summary
  veritas_analysis_semantic
  veritas_fact_proto
)
target_link_libraries(veritas_facts PRIVATE
  LLVMSupport
  veritas::summarydb
)

target_compile_features(veritas_facts PUBLIC cxx_std_20)
veritas_add_warnings(veritas_facts)

add_library(veritas::facts ALIAS veritas_facts)
