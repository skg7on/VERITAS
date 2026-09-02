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

add_library(veritas_analysis
  ProjectAnalyzer.cpp
  ProjectPublicationCoordinator.cpp
)

target_include_directories(veritas_analysis
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)

target_link_libraries(veritas_analysis
  PUBLIC
    veritas_core
  PRIVATE
    veritas_svf_analysis
    veritas_analysis_pipeline
    veritas_analysis_cpg
    veritas_analysis_semantic
    veritas_cpg
    veritas_build
    veritas_summarydb
    veritas_wpa
)

# The versioned external-model bundle is a checked-in source resource. Bake its
# directory in at configure time so the analyzer loads the exact bundle it was
# built against (mirrors VERITAS_TESTS_SOURCE_DIR for test fixtures).
target_compile_definitions(veritas_analysis PRIVATE
  VERITAS_LOGIC_MODELS_DIR="${CMAKE_SOURCE_DIR}/logic/models"
)

# The compiled Souffle worker is the production executor; bake its path in so
# the analyzer can construct the SouffleWpaExecutor. Only defined under the
# souffle engine.
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  target_compile_definitions(veritas_analysis PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
  )
  add_dependencies(veritas_analysis veritas_souffle_worker)
endif()

add_subdirectory(semantic)
add_subdirectory(pipeline)
add_subdirectory(llvm)
add_subdirectory(svf)
add_subdirectory(cpg)
