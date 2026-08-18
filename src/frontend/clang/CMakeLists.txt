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

add_library(veritas_frontend_clang
  ProjectAstExtractor.cpp
  SourceAnchorBuilder.cpp
)

target_include_directories(veritas_frontend_clang
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_link_libraries(veritas_frontend_clang
  PUBLIC
    veritas_core
    veritas_build
    clangTooling
    clangFrontend
    clangSerialization
    clangAST
    clangBasic
  PRIVATE
    LLVMSupport
)

set_target_properties(veritas_frontend_clang PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
