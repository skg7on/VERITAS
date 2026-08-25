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

add_library(veritas_facts
  AnalysisRun.cpp
  AnalysisFact.cpp
  RelationSchema.cpp
  ResultCanonicalizer.cpp
  RuleRegistry.cpp
  SemanticKeyCodec.cpp
  LegacyFactAdapter.cpp
  FactSchema.cpp
  SummaryFactBuilder.cpp
  SouffleExporter.cpp
  Witness.cpp
  SouffleRunner.cpp
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
)
target_link_libraries(veritas_facts PRIVATE
  LLVMSupport
)

target_compile_features(veritas_facts PUBLIC cxx_std_20)
veritas_add_warnings(veritas_facts)

add_library(veritas::facts ALIAS veritas_facts)
