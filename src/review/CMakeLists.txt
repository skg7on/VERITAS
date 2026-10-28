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


file(READ "${PROJECT_SOURCE_DIR}/plugins/veritas-evidence-review/schemas/veritas-review-backend-v2.schema.json" REVIEW_SCHEMA)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/plugins/veritas-evidence-review/schemas/veritas-review-backend-v2.schema.json")
configure_file(ReviewSchema.h.in ReviewSchema.h @ONLY)
add_library(veritas_review ReviewTypes.cpp ProgramScope.cpp SnapshotCapture.cpp StructuralIndex.cpp LocalEvidence.cpp)
target_include_directories(veritas_review PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(veritas_review SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_include_directories(veritas_review PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(veritas_review PUBLIC veritas_core PRIVATE clang-cpp LLVM)
veritas_add_warnings(veritas_review)
