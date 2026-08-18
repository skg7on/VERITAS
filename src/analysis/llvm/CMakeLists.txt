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

add_library(veritas_analysis_llvm
  OriginMap.cpp
  ProjectIrBuilder.cpp
  CallGraphExtractor.cpp
  MemoryAccessExtractor.cpp
  ValueFlowExtractor.cpp
)

target_include_directories(veritas_analysis_llvm
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(veritas_analysis_llvm
  PUBLIC
    LLVMCore
    LLVMSupport
    LLVMLinker
    clangCodeGen
    clangFrontend
    clangTooling
)
