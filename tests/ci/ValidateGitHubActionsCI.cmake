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

if(NOT DEFINED VERITAS_SOURCE_DIR)
  message(FATAL_ERROR "VERITAS_SOURCE_DIR is required")
endif()

set(
  VERITAS_CI_WORKFLOW
  "${VERITAS_SOURCE_DIR}/.github/workflows/ci.yml"
)

if(NOT EXISTS "${VERITAS_CI_WORKFLOW}")
  message(FATAL_ERROR "Missing GitHub Actions workflow: ${VERITAS_CI_WORKFLOW}")
endif()

file(READ "${VERITAS_CI_WORKFLOW}" VERITAS_CI_CONTENT)

function(veritas_require_ci_literal DESCRIPTION LITERAL)
  string(FIND "${VERITAS_CI_CONTENT}" "${LITERAL}" LITERAL_INDEX)
  if(LITERAL_INDEX EQUAL -1)
    message(FATAL_ERROR "CI workflow is missing ${DESCRIPTION}: ${LITERAL}")
  endif()
endfunction()

function(veritas_forbid_ci_literal DESCRIPTION LITERAL)
  string(FIND "${VERITAS_CI_CONTENT}" "${LITERAL}" LITERAL_INDEX)
  if(NOT LITERAL_INDEX EQUAL -1)
    message(FATAL_ERROR "CI workflow contains forbidden ${DESCRIPTION}: ${LITERAL}")
  endif()
endfunction()

veritas_require_ci_literal("workflow name" "name: CI Build")
veritas_require_ci_literal("push trigger" "push:")
veritas_require_ci_literal("pull-request trigger" "pull_request:")
veritas_require_ci_literal("manual trigger" "workflow_dispatch:")
veritas_require_ci_literal("main branch restriction" "branches: [main]")
veritas_require_ci_literal("read-only permission" "contents: read")
veritas_require_ci_literal("concurrency cancellation" "cancel-in-progress: true")
veritas_require_ci_literal("Ubuntu 24.04 runner" "runs-on: ubuntu-24.04")
veritas_require_ci_literal("job timeout" "timeout-minutes: 180")
veritas_require_ci_literal(
  "checkout pin"
  "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
)
veritas_require_ci_literal(
  "cache pin"
  "actions/cache@27d5ce7f107fe9357f9df03efb73ab90386fccae"
)
veritas_require_ci_literal(
  "Z3 source pin"
  "ddb49568d3520e99799e364fb22f35fc67d887b1"
)
veritas_require_ci_literal(
  "LLVM source pin"
  "860fcb7accb22e57a020a353a39f2fdbd0dc1b44"
)
veritas_require_ci_literal(
  "Z3 cache gate"
  "steps.z3-cache.outputs.cache-hit != 'true'"
)
veritas_require_ci_literal(
  "LLVM cache gate"
  "steps.llvm-cache.outputs.cache-hit != 'true'"
)
veritas_require_ci_literal("LLVM RTTI" "-DLLVM_ENABLE_RTTI=ON")
veritas_require_ci_literal("LLVM exceptions" "-DLLVM_ENABLE_EH=ON")
veritas_require_ci_literal("LLVM shared library" "-DLLVM_BUILD_LLVM_DYLIB=ON")
veritas_require_ci_literal("LLVM shared linking" "-DLLVM_LINK_LLVM_DYLIB=ON")
veritas_require_ci_literal(
  "VERITAS LLVM configuration"
  "-DLLVM_PROJECT_BUILD_DIR=\"$RUNNER_TEMP/llvm-install\""
)
veritas_require_ci_literal(
  "CMake config package preference"
  "-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON"
)
veritas_require_ci_literal("SVF build" "--target SvfCore SvfLLVM")
veritas_require_ci_literal(
  "CTest execution"
  "ctest --test-dir build --output-on-failure"
)
veritas_forbid_ci_literal("partial cache restore" "restore-keys:")

string(
  REGEX MATCHALL
  "uses: [^ \r\n]+"
  VERITAS_ACTION_REFERENCES
  "${VERITAS_CI_CONTENT}"
)
list(LENGTH VERITAS_ACTION_REFERENCES VERITAS_ACTION_REFERENCE_COUNT)
if(NOT VERITAS_ACTION_REFERENCE_COUNT EQUAL 5)
  message(
    FATAL_ERROR
    "Expected five pinned Action references, found ${VERITAS_ACTION_REFERENCE_COUNT}"
  )
endif()

foreach(VERITAS_ACTION_REFERENCE IN LISTS VERITAS_ACTION_REFERENCES)
  if(
    NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
    AND NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/cache@27d5ce7f107fe9357f9df03efb73ab90386fccae"
  )
    message(
      FATAL_ERROR
      "Action reference is not an approved immutable pin: ${VERITAS_ACTION_REFERENCE}"
    )
  endif()
endforeach()
