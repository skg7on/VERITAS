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
  ProgramIr.cpp
  OriginMap.cpp
  ProjectIrBuilder.cpp
  CallGraphExtractor.cpp
  MemoryAccessExtractor.cpp
  ValueFlowExtractor.cpp
  StableValueMapper.cpp
  AbstractMemoryBuilder.cpp
  LocalFactExtractor.cpp
)

target_include_directories(veritas_analysis_llvm
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_link_libraries(veritas_analysis_llvm
  PUBLIC
    LLVM
    clang-cpp
    veritas_build
    veritas_summary
)
