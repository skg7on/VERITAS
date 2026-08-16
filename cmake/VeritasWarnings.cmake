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

# VeritasWarnings.cmake
#
# Provides veritas_add_warnings(<target>) — the single place VERITAS-owned
# targets pick up their warning contract. Applied per-target so it never
# leaks onto SVF, LLVM, or other third-party code, whose header hygiene we
# do not control.
#
# Contract (Clang / GCC):
#   -Wall -Wextra -Wpedantic -Werror
#   -Wno-unused-parameter        # noisy in ports and stub CLIs
#   -Wno-missing-field-initializers  # tolerated for aggregate-init POD
#
# Contract (MSVC):
#   /W4 /WX

function(veritas_add_warnings target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR
      "veritas_add_warnings: '${target}' is not a target")
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR
     CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
      -Wno-unused-parameter
      -Wno-missing-field-initializers
    )
  elseif(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /WX
    )
  endif()
endfunction()
