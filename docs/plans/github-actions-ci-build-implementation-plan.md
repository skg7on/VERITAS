# GitHub Actions CI Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an Ubuntu 24.04 GitHub Actions build that source-builds pinned Z3 and LLVM 24 installations once, reuses exact caches thereafter, builds VERITAS and its required SVF targets, and runs CTest.

**Architecture:** A single read-only workflow owns dependency provisioning and project verification. Exact, independently keyed Z3 and LLVM install-tree caches gate source checkout and build steps; a repository CTest contract guards the workflow's runner, pins, cache conditions, and required build commands.

**Tech Stack:** GitHub Actions, Ubuntu 24.04, CMake, Ninja, CTest, LLVM/Clang 24, Z3 4.16.0, vendored SVF, CMake script tests.

**Spec:** `docs/specs/github-actions-ci-build-design.md`

## Global Constraints

- Run on the explicit `ubuntu-24.04` GitHub-hosted runner.
- Pin `actions/checkout` to `3d3c42e5aac5ba805825da76410c181273ba90b1` (`v7.0.1`).
- Pin `actions/cache` to `27d5ce7f107fe9357f9df03efb73ab90386fccae` (`v5.0.5`).
- Pin LLVM 24 source to `860fcb7accb22e57a020a353a39f2fdbd0dc1b44`.
- Pin Z3 source to `ddb49568d3520e99799e364fb22f35fc67d887b1` (`z3-4.16.0`).
- Cache installed dependency trees only; never cache LLVM or Z3 source/build trees.
- Use exact cache keys without `restore-keys`.
- Enable LLVM RTTI and exception handling, Clang, X86, and the shared LLVM library.
- Build the default VERITAS targets plus `SvfCore` and `SvfLLVM`, then run all CTest tests.
- Keep workflow permissions at `contents: read`, cancel superseded branch runs, and use a 180-minute job timeout.
- Apply full Apache-2.0 headers to new CMake files.

---

### Task 1: Add the GitHub Actions CI contract and workflow

**Files:**
- Create: `tests/ci/CMakeLists.txt`
- Create: `tests/ci/ValidateGitHubActionsCI.cmake`
- Modify: `tests/CMakeLists.txt`
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `PROJECT_SOURCE_DIR`, the existing `VERITAS_BUILD_TESTS`/CTest tree, `LLVM_PROJECT_BUILD_DIR`, `Z3_DIR`, and the CMake targets `SvfCore` and `SvfLLVM`.
- Produces: CTest test `veritas_ci_workflow` and GitHub Actions workflow `CI Build`.

- [ ] **Step 1: Register a failing static workflow contract test**

Add `add_subdirectory(ci)` immediately after the existing integration test
subdirectory in `tests/CMakeLists.txt`:

```cmake
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(ci)
```

Create `tests/ci/CMakeLists.txt`:

```cmake
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

add_test(
  NAME veritas_ci_workflow
  COMMAND
    "${CMAKE_COMMAND}"
    "-DVERITAS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/ValidateGitHubActionsCI.cmake"
)

set_tests_properties(veritas_ci_workflow PROPERTIES TIMEOUT 30)
```

Create `tests/ci/ValidateGitHubActionsCI.cmake`:

```cmake
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
```

- [ ] **Step 2: Run the contract test and verify it fails because the workflow is absent**

Run:

```bash
cmake -S . -B build \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
ctest --test-dir build -R veritas_ci_workflow --output-on-failure
```

Expected: CTest discovers `veritas_ci_workflow` and fails with `Missing GitHub
Actions workflow`.

- [ ] **Step 3: Implement the complete pinned workflow**

Create `.github/workflows/ci.yml`:

```yaml
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

name: CI Build

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build:
    name: Ubuntu 24.04 / LLVM 24
    runs-on: ubuntu-24.04
    timeout-minutes: 180

    steps:
      - name: Checkout VERITAS
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          fetch-depth: 1
          persist-credentials: false

      - name: Install system dependencies
        run: |
          sudo apt-get update
          sudo apt-get install --yes --no-install-recommends \
            build-essential \
            cmake \
            git \
            libedit-dev \
            libffi-dev \
            libgtest-dev \
            libprotobuf-dev \
            librocksdb-dev \
            libsqlite3-dev \
            libtinfo-dev \
            libxml2-dev \
            libzstd-dev \
            ninja-build \
            pkg-config \
            protobuf-compiler \
            python3 \
            zlib1g-dev

      - name: Restore Z3 cache
        id: z3-cache
        uses: actions/cache@27d5ce7f107fe9357f9df03efb73ab90386fccae # v5.0.5
        with:
          path: ${{ runner.temp }}/z3-install
          key: veritas-ubuntu-24.04-x86_64-z3-ddb49568d3520e99799e364fb22f35fc67d887b1-r1

      - name: Checkout Z3 source
        if: steps.z3-cache.outputs.cache-hit != 'true'
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          repository: Z3Prover/z3
          ref: ddb49568d3520e99799e364fb22f35fc67d887b1
          path: z3
          fetch-depth: 1
          persist-credentials: false

      - name: Build and install Z3
        if: steps.z3-cache.outputs.cache-hit != 'true'
        run: |
          cmake -S z3 -B "$RUNNER_TEMP/z3-build" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$RUNNER_TEMP/z3-install" \
            -DZ3_BUILD_LIBZ3_SHARED=ON \
            -DZ3_BUILD_PYTHON_BINDINGS=OFF \
            -DZ3_BUILD_DOCUMENTATION=OFF
          cmake --build "$RUNNER_TEMP/z3-build" --target install --parallel 4
          cmake -E remove_directory "$GITHUB_WORKSPACE/z3"
          cmake -E remove_directory "$RUNNER_TEMP/z3-build"

      - name: Validate Z3 installation
        run: |
          test -f "$RUNNER_TEMP/z3-install/include/z3++.h"
          test -f "$RUNNER_TEMP/z3-install/lib/libz3.so"
          test -f "$RUNNER_TEMP/z3-install/lib/cmake/z3/Z3Config.cmake"

      - name: Restore LLVM cache
        id: llvm-cache
        uses: actions/cache@27d5ce7f107fe9357f9df03efb73ab90386fccae # v5.0.5
        with:
          path: ${{ runner.temp }}/llvm-install
          key: veritas-ubuntu-24.04-x86_64-llvm-860fcb7accb22e57a020a353a39f2fdbd0dc1b44-r1

      - name: Checkout LLVM source
        if: steps.llvm-cache.outputs.cache-hit != 'true'
        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1
        with:
          repository: llvm/llvm-project
          ref: 860fcb7accb22e57a020a353a39f2fdbd0dc1b44
          path: llvm-project
          fetch-depth: 1
          persist-credentials: false

      - name: Configure LLVM
        if: steps.llvm-cache.outputs.cache-hit != 'true'
        run: |
          cmake -S llvm-project/llvm -B "$RUNNER_TEMP/llvm-build" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$RUNNER_TEMP/llvm-install" \
            -DLLVM_ENABLE_PROJECTS=clang \
            -DLLVM_TARGETS_TO_BUILD=X86 \
            -DLLVM_ENABLE_RTTI=ON \
            -DLLVM_ENABLE_EH=ON \
            -DLLVM_BUILD_LLVM_DYLIB=ON \
            -DLLVM_LINK_LLVM_DYLIB=ON \
            -DLLVM_INCLUDE_TESTS=OFF \
            -DCLANG_INCLUDE_TESTS=OFF \
            -DLLVM_INCLUDE_EXAMPLES=OFF \
            -DLLVM_INCLUDE_BENCHMARKS=OFF \
            -DLLVM_INCLUDE_DOCS=OFF \
            -DLLVM_ENABLE_BINDINGS=OFF

      - name: Build and install LLVM
        if: steps.llvm-cache.outputs.cache-hit != 'true'
        run: |
          cmake --build "$RUNNER_TEMP/llvm-build" --target install --parallel 4
          cmake -E remove_directory "$GITHUB_WORKSPACE/llvm-project"
          cmake -E remove_directory "$RUNNER_TEMP/llvm-build"

      - name: Validate LLVM installation
        env:
          LD_LIBRARY_PATH: ${{ runner.temp }}/llvm-install/lib
        run: |
          test -x "$RUNNER_TEMP/llvm-install/bin/llvm-config"
          test -x "$RUNNER_TEMP/llvm-install/bin/clang"
          test -f "$RUNNER_TEMP/llvm-install/lib/cmake/llvm/LLVMConfig.cmake"
          test -f "$RUNNER_TEMP/llvm-install/lib/cmake/clang/ClangConfig.cmake"
          test "$("$RUNNER_TEMP/llvm-install/bin/llvm-config" --version)" = "24.0.0git"
          "$RUNNER_TEMP/llvm-install/bin/clang" --version

      - name: Configure VERITAS
        env:
          LD_LIBRARY_PATH: ${{ runner.temp }}/llvm-install/lib:${{ runner.temp }}/z3-install/lib
        run: |
          cmake -S . -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$RUNNER_TEMP/llvm-install/bin/clang" \
            -DCMAKE_CXX_COMPILER="$RUNNER_TEMP/llvm-install/bin/clang++" \
            -DLLVM_PROJECT_BUILD_DIR="$RUNNER_TEMP/llvm-install" \
            -DZ3_DIR="$RUNNER_TEMP/z3-install/lib/cmake/z3" \
            -DVERITAS_BUILD_TESTS=ON \
            -DVERITAS_BUILD_TOOLS=ON

      - name: Build VERITAS
        env:
          LD_LIBRARY_PATH: ${{ runner.temp }}/llvm-install/lib:${{ runner.temp }}/z3-install/lib
        run: cmake --build build --parallel 4

      - name: Build SVF
        env:
          LD_LIBRARY_PATH: ${{ runner.temp }}/llvm-install/lib:${{ runner.temp }}/z3-install/lib
        run: cmake --build build --target SvfCore SvfLLVM --parallel 4

      - name: Run tests
        env:
          LD_LIBRARY_PATH: ${{ runner.temp }}/llvm-install/lib:${{ runner.temp }}/z3-install/lib
        run: ctest --test-dir build --output-on-failure
```

- [ ] **Step 4: Run the focused test and verify it passes**

Run:

```bash
cmake -S . -B build \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
ctest --test-dir build -R veritas_ci_workflow --output-on-failure
```

Expected: `veritas_ci_workflow` passes.

- [ ] **Step 5: Parse the YAML locally**

Run:

```bash
ruby -e 'require "yaml"; YAML.safe_load(File.read(".github/workflows/ci.yml"), aliases: true)'
```

Expected: exit code 0 with no parser error.

- [ ] **Step 6: Commit the workflow and its contract test**

```bash
git add .github/workflows/ci.yml \
  tests/CMakeLists.txt \
  tests/ci/CMakeLists.txt \
  tests/ci/ValidateGitHubActionsCI.cmake
git diff --cached --check
git commit -m "ci: add cached LLVM GitHub build"
```

---

### Task 2: Verify the complete local build contract

**Files:**
- Verify: `.github/workflows/ci.yml`
- Verify: `tests/ci/ValidateGitHubActionsCI.cmake`

**Interfaces:**
- Consumes: the local LLVM 24 build at `/Users/skg7on/Workspace/Projects/llvm-project/build` and the configured worktree `build` directory.
- Produces: evidence that the workflow contract, default VERITAS build, explicit SVF targets, and all CTest tests pass before push.

- [ ] **Step 1: Build the normal VERITAS targets**

Run:

```bash
cmake --build build -j2
```

Expected: all normal VERITAS libraries, tools, and test binaries build.

- [ ] **Step 2: Build the explicit vendored SVF targets**

Run:

```bash
cmake --build build --target SvfCore SvfLLVM -j2
```

Expected: `SvfCore` and `SvfLLVM` build successfully.

- [ ] **Step 3: Run the complete CTest suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass, including `veritas_pre_commit_hook` and
`veritas_ci_workflow`.

- [ ] **Step 4: Inspect repository state**

Run:

```bash
git diff --check
git status --short --branch
git log --oneline --decorate -3
```

Expected: no unstaged changes, no untracked implementation files, and separate
design-spec and workflow commits on `claude/ci-build-workflow`.

---

### Task 3: Push, open the pull request, and prove both cache paths

**Files:**
- Remote workflow: `.github/workflows/ci.yml`
- Pull request source branch: `claude/ci-build-workflow`

**Interfaces:**
- Consumes: a clean, verified local branch and authenticated `git`/GitHub CLI access.
- Produces: a pull request whose first CI attempt proves the source-build path and whose rerun proves the exact LLVM cache-hit path.

- [ ] **Step 1: Push the branch and create the pull request**

Run:

```bash
git push -u origin claude/ci-build-workflow
gh pr create \
  --base main \
  --head claude/ci-build-workflow \
  --title "ci: add cached LLVM GitHub build" \
  --body "Build VERITAS and required SVF targets on Ubuntu 24.04 with pinned, source-built Z3 and LLVM 24 install caches; run the complete CTest suite."
```

Expected: GitHub returns the new pull-request URL and schedules `CI Build`.

- [ ] **Step 2: Watch the initial cache-miss run**

Run:

```bash
CI_RUN_ID="$(gh run list \
  --branch claude/ci-build-workflow \
  --workflow ci.yml \
  --limit 1 \
  --json databaseId \
  --jq '.[0].databaseId')"
gh run watch "$CI_RUN_ID" --exit-status
gh run view "$CI_RUN_ID" --json jobs \
  --jq '.jobs[].steps[] | select(.name | contains("LLVM")) | [.name, .conclusion] | @tsv'
```

Expected: the LLVM cache restore succeeds as a miss, LLVM checkout/configure/
build steps succeed, validation succeeds, and the workflow completes.

- [ ] **Step 3: Rerun the workflow and prove the exact LLVM cache hit**

Run:

```bash
gh run rerun "$CI_RUN_ID"
gh run watch "$CI_RUN_ID" --exit-status
gh run view "$CI_RUN_ID" --json jobs \
  --jq '.jobs[].steps[] | select(.name | contains("LLVM")) | [.name, .conclusion] | @tsv'
```

Expected: `Restore LLVM cache` and `Validate LLVM installation` succeed while
`Checkout LLVM source`, `Configure LLVM`, and `Build and install LLVM` are
skipped.

- [ ] **Step 4: Inspect the pull request and final checks**

Run:

```bash
gh pr view --web=false
gh pr checks
```

Expected: the pull request is open, all checks pass, and the branch is ready for
review. If the hosted environment exposes an unexpected package, disk, or build
failure, use `superpowers:systematic-debugging`, make the smallest workflow or
contract-test correction, rerun Task 2, commit, push, and repeat this task.
