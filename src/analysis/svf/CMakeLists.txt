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

add_library(veritas_svf_analysis STATIC
  SvfConfig.cpp
  SvfSession.cpp
  SvfFactMapper.cpp
  SvfBudget.cpp
  SvfAnalysisStage.cpp
  SvfMerge.cpp
)

target_link_libraries(veritas_svf_analysis
  PRIVATE
    veritas_third_party_svf
    veritas_analysis_llvm
    veritas_analysis_pipeline
    veritas_analysis_semantic
    veritas_summary
)

target_include_directories(veritas_svf_analysis
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

# SVF headers (included from this target) use LLVM 24-deprecated APIs and
# std::iterator; suppress the deprecation warnings they emit into these TUs.
target_compile_options(veritas_svf_analysis PRIVATE -Wno-deprecated-declarations)
