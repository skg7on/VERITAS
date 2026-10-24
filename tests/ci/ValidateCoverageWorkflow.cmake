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
  VERITAS_COVERAGE_WORKFLOW
  "${VERITAS_SOURCE_DIR}/.github/workflows/coverage.yml"
)
set(
  VERITAS_CI_WORKFLOW
  "${VERITAS_SOURCE_DIR}/.github/workflows/ci.yml"
)

if(NOT EXISTS "${VERITAS_COVERAGE_WORKFLOW}")
  message(
    FATAL_ERROR
    "Missing GitHub Actions workflow: ${VERITAS_COVERAGE_WORKFLOW}"
  )
endif()

file(READ "${VERITAS_COVERAGE_WORKFLOW}" VERITAS_COVERAGE_CONTENT)

function(veritas_require_coverage_literal DESCRIPTION LITERAL)
  string(FIND "${VERITAS_COVERAGE_CONTENT}" "${LITERAL}" LITERAL_INDEX)
  if(LITERAL_INDEX EQUAL -1)
    message(
      FATAL_ERROR
      "Coverage workflow is missing ${DESCRIPTION}: ${LITERAL}"
    )
  endif()
endfunction()

function(veritas_forbid_coverage_literal DESCRIPTION LITERAL)
  string(FIND "${VERITAS_COVERAGE_CONTENT}" "${LITERAL}" LITERAL_INDEX)
  if(NOT LITERAL_INDEX EQUAL -1)
    message(
      FATAL_ERROR
      "Coverage workflow contains forbidden ${DESCRIPTION}: ${LITERAL}"
    )
  endif()
endfunction()

veritas_require_coverage_literal("workflow name" "name: Code Coverage")
veritas_require_coverage_literal("push trigger" "push:")
veritas_require_coverage_literal("pull-request trigger" "pull_request:")
veritas_require_coverage_literal("manual trigger" "workflow_dispatch:")
veritas_require_coverage_literal("main branch restriction" "branches: [main]")
veritas_require_coverage_literal("read-only default" "contents: read")
veritas_require_coverage_literal(
  "badge publication permission"
  "contents: write"
)
veritas_require_coverage_literal(
  "concurrency cancellation"
  "cancel-in-progress: true"
)
veritas_require_coverage_literal(
  "Ubuntu 24.04 runner"
  "runs-on: ubuntu-24.04"
)
veritas_require_coverage_literal(
  "checkout pin"
  "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
)
veritas_require_coverage_literal(
  "cache restore pin"
  "actions/cache/restore@27d5ce7f107fe9357f9df03efb73ab90386fccae"
)
veritas_require_coverage_literal(
  "cache save pin"
  "actions/cache/save@27d5ce7f107fe9357f9df03efb73ab90386fccae"
)
veritas_require_coverage_literal(
  "artifact upload pin"
  "actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9"
)
# The cached LLVM has no compiler-rt, so coverage builds the profile runtime
# for the same pinned revision and validates it through the clang driver.
veritas_require_coverage_literal(
  "compiler-rt cache key"
  "veritas-ubuntu-24.04-x86_64-compiler-rt-f9bda52e57a759d20224cd581f73f61ee3220e74-r1"
)
veritas_require_coverage_literal(
  "profile runtime build"
  "-DCOMPILER_RT_BUILD_PROFILE=ON"
)
veritas_require_coverage_literal(
  "profile runtime install layout"
  "-DCOMPILER_RT_INSTALL_PATH="
)
veritas_require_coverage_literal(
  "single-target runtime"
  "-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON"
)
veritas_require_coverage_literal(
  "compiler-rt target triple"
  "-DCMAKE_C_COMPILER_TARGET="
)
veritas_require_coverage_literal(
  "runtime directory validation"
  "-print-runtime-dir"
)
veritas_require_coverage_literal(
  "LLVM source pin"
  "f9bda52e57a759d20224cd581f73f61ee3220e74"
)
veritas_require_coverage_literal("LLVM RTTI" "-DLLVM_ENABLE_RTTI=ON")
veritas_require_coverage_literal("LLVM exceptions" "-DLLVM_ENABLE_EH=ON")
veritas_require_coverage_literal(
  "LLVM monolithic DYLIB"
  "-DLLVM_BUILD_LLVM_DYLIB=ON"
)
veritas_require_coverage_literal("LLVM DYLIB linking" "-DLLVM_LINK_LLVM_DYLIB=ON")
veritas_require_coverage_literal("Clang DYLIB" "-DCLANG_LINK_CLANG_DYLIB=ON")
veritas_require_coverage_literal(
  "VERITAS LLVM configuration"
  "-DLLVM_PROJECT_BUILD_DIR=\"$RUNNER_TEMP/llvm-install\""
)
veritas_require_coverage_literal(
  "compile-time profile generation"
  "-fprofile-instr-generate -fcoverage-mapping"
)
veritas_require_coverage_literal(
  "link-time profile runtime"
  "-DCMAKE_EXE_LINKER_FLAGS=\"-fprofile-instr-generate\""
)
veritas_require_coverage_literal(
  "per-process profile files"
  "LLVM_PROFILE_FILE: \${{ github.workspace }}/coverage/profiles/%p-%m.profraw"
)
veritas_require_coverage_literal(
  "profile merge"
  "llvm-profdata\" merge"
)
veritas_require_coverage_literal("lcov export" "-format=lcov")
veritas_require_coverage_literal("summary export" "--summary-only")
veritas_require_coverage_literal(
  "CTest execution"
  "ctest --test-dir build --output-on-failure"
)
# The performance gate's ceiling is calibrated for an optimized,
# uninstrumented build; the regular CI job remains its owner.
veritas_require_coverage_literal(
  "performance-test exclusion"
  "-E '^WpaPerformanceQualificationTest$'"
)
veritas_require_coverage_literal(
  "pull-request publication guard"
  "github.event_name != 'pull_request'"
)
veritas_require_coverage_literal("badge branch" "HEAD:refs/heads/badges")
veritas_forbid_coverage_literal("partial cache restore" "restore-keys:")
veritas_forbid_coverage_literal("privileged PR trigger" "pull_request_target:")

# The coverage job restores the toolchain caches that ci.yml owns. Every cache
# key in ci.yml must therefore also appear here, so a key bump cannot silently
# leave coverage rebuilding LLVM from source forever.
if(NOT EXISTS "${VERITAS_CI_WORKFLOW}")
  message(FATAL_ERROR "Missing GitHub Actions workflow: ${VERITAS_CI_WORKFLOW}")
endif()
file(READ "${VERITAS_CI_WORKFLOW}" VERITAS_CI_CONTENT)
string(
  REGEX MATCHALL
  "veritas-ubuntu-24[.]04-x86_64-[a-z0-9]+-[A-Za-z0-9.]+-r[0-9]+"
  VERITAS_CI_CACHE_KEYS
  "${VERITAS_CI_CONTENT}"
)
list(REMOVE_DUPLICATES VERITAS_CI_CACHE_KEYS)
list(LENGTH VERITAS_CI_CACHE_KEYS VERITAS_CI_CACHE_KEY_COUNT)
if(VERITAS_CI_CACHE_KEY_COUNT EQUAL 0)
  message(FATAL_ERROR "No toolchain cache keys found in ci.yml")
endif()
foreach(VERITAS_CI_CACHE_KEY IN LISTS VERITAS_CI_CACHE_KEYS)
  veritas_require_coverage_literal(
    "cache key shared with ci.yml"
    "${VERITAS_CI_CACHE_KEY}"
  )
endforeach()

string(
  REGEX MATCHALL
  "uses: [^ \r\n]+"
  VERITAS_ACTION_REFERENCES
  "${VERITAS_COVERAGE_CONTENT}"
)
list(LENGTH VERITAS_ACTION_REFERENCES VERITAS_ACTION_REFERENCE_COUNT)
if(VERITAS_ACTION_REFERENCE_COUNT EQUAL 0)
  message(FATAL_ERROR "Coverage workflow contains no Action references")
endif()

foreach(VERITAS_ACTION_REFERENCE IN LISTS VERITAS_ACTION_REFERENCES)
  if(
    NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"
    AND NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/cache/restore@27d5ce7f107fe9357f9df03efb73ab90386fccae"
    AND NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/cache/save@27d5ce7f107fe9357f9df03efb73ab90386fccae"
    AND NOT VERITAS_ACTION_REFERENCE STREQUAL
        "uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9"
  )
    message(
      FATAL_ERROR
      "Action reference is not an approved immutable pin: ${VERITAS_ACTION_REFERENCE}"
    )
  endif()
endforeach()
