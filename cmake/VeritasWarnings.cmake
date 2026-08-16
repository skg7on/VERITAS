# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The VERITAS Authors.
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
