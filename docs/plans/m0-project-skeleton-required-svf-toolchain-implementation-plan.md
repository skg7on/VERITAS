# M0 Project Skeleton and Required SVF Toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a buildable C++20 foundation in which the pinned SVF source tree is a required third-party Git submodule and LLVM/Clang/SVF share one verified toolchain contract.

**Architecture:** VERITAS requires LLVM/Clang 22+, CMake 3.23+, and a pinned SVF Git submodule at `third_party/SVF`. SVF is mandatory for the standard build; there is no enable/disable flag. M0 produces four CLI binaries (`veritas-build`, `veritas-query`, `veritas-diff`, `veritas-explain`), each implementing `--version`, and provides minimal `Status`/`StatusOr`/`Version` APIs for later milestones.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22+, pinned SVF submodule at commit `18fb5650600530a54f0afc22f4df1a10b03d3c02`, Z3, Protobuf, RocksDB, SQLite, GoogleTest. Souffle remains optional until M8.

**Spec:** Section 4 of `docs/specs/veritas-backbone-milestones-and-implementation-plan.md`

## Global Constraints

- SVF is required by the standard VERITAS build; there is no `VERITAS_ENABLE_SVF`, `FindSVF.cmake`, or arbitrary system-SVF substitution.
- A missing submodule fails configuration with the recovery command `git submodule update --init --recursive third_party/SVF`.
- M0 establishes the dependency and build boundary; production analysis first invokes SVF in M5.
- Dependency adapters have explicit private CMake boundaries and do not leak native types into public headers.
- VERITAS and SVF must use compatible LLVM version, RTTI, exception, target, and ABI settings.
- All four CLI binaries print the same version format with `--version` and exit successfully.

---

## Task 1: Add SVF Git Submodule

**Files:**
- Create: `.gitmodules`
- Add: `third_party/SVF` Git submodule pinned to `18fb5650600530a54f0afc22f4df1a10b03d3c02`

**Interfaces:**
- None (Git submodule only)

- [ ] **Step 1: Add SVF as a Git submodule**

```bash
git submodule add https://github.com/SVF-tools/SVF.git third_party/SVF
```

- [ ] **Step 2: Pin SVF to the exact required commit**

```bash
git -C third_party/SVF checkout 18fb5650600530a54f0afc22f4df1a10b03d3c02
```

- [ ] **Step 3: Stage the submodule**

```bash
git add .gitmodules third_party/SVF
```

- [ ] **Step 4: Verify the submodule is at the correct revision**

```bash
git -C third_party/SVF rev-parse HEAD
# Expected: 18fb5650600530a54f0afc22f4df1a10b03d3c02
```

- [ ] **Step 5: Document the SVF integration**

Create `docs/third_party/SVF.md` with upstream source, license (AGPL-3.0-or-later), pinned revision, LLVM 22+ requirement, CMake 3.23+ requirement, and initialization command.

---

## Task 2: Create CMake Project Skeleton

**Files:**
- Create: `CMakeLists.txt` (top-level)
- Create: `cmake/Dependencies.cmake`
- Create: `cmake/VerifySvfSubmodule.cmake`
- Create: `cmake/VeritasWarnings.cmake`
- Modify: `cmake/VeritasLLVM.cmake` (already exists from PR #25)

**Interfaces:**
- CMake targets: `veritas_core`, `veritas-build`, `veritas-query`, `veritas-diff`, `veritas-explain`
- Private interface target: `veritas_third_party_svf` wrapping `SvfCore` and `SvfLLVM`

- [ ] **Step 1: Create top-level CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.23)

project(VERITAS
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(VERITAS_BUILD_TESTS "Build VERITAS tests" ON)

# Include CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(VeritasWarnings)
include(VerifySvfSubmodule)
include(VeritasLLVM)
include(Dependencies)

# Subdirectories
add_subdirectory(src/core)
add_subdirectory(src/tools)

if(VERITAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

- [ ] **Step 2: Create cmake/VerifySvfSubmodule.cmake**

```cmake
# Verify the SVF submodule is initialized
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/SVF/CMakeLists.txt")
  message(FATAL_ERROR
    "SVF submodule is not initialized.\n"
    "Run: git submodule update --init --recursive third_party/SVF")
endif()

# Verify the submodule is at the expected revision
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/SVF"
  OUTPUT_VARIABLE SVF_CURRENT_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE SVF_REV_RESULT
)

set(SVF_EXPECTED_REVISION "18fb5650600530a54f0afc22f4df1a10b03d3c02")

if(NOT SVF_REV_RESULT EQUAL 0)
  message(WARNING "Could not determine SVF submodule revision")
elseif(NOT SVF_CURRENT_REVISION STREQUAL SVF_EXPECTED_REVISION)
  message(WARNING
    "SVF submodule revision mismatch:\n"
    "  Expected: ${SVF_EXPECTED_REVISION}\n"
    "  Current:  ${SVF_CURRENT_REVISION}\n"
    "VERITAS is pinned to a specific SVF revision for compatibility.")
endif()

message(STATUS "VERITAS: SVF submodule at ${SVF_CURRENT_REVISION}")

# Add SVF with EXCLUDE_FROM_ALL so only required targets are built
add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)

# Create private wrapper target over SVF
add_library(veritas_third_party_svf INTERFACE)
target_link_libraries(veritas_third_party_svf INTERFACE SvfCore SvfLLVM)
```

- [ ] **Step 3: Create cmake/Dependencies.cmake**

```cmake
# Find required dependencies

# Protobuf
find_package(Protobuf REQUIRED)
message(STATUS "VERITAS: Found Protobuf ${Protobuf_VERSION}")

# RocksDB
find_package(RocksDB REQUIRED)
message(STATUS "VERITAS: Found RocksDB")

# SQLite3
find_package(SQLite3 REQUIRED)
message(STATUS "VERITAS: Found SQLite3 ${SQLite3_VERSION}")

# GoogleTest (only if building tests)
if(VERITAS_BUILD_TESTS)
  find_package(GTest REQUIRED)
  message(STATUS "VERITAS: Found GTest ${GTest_VERSION}")
endif()

# Z3 (required)
find_package(Z3 REQUIRED)
message(STATUS "VERITAS: Found Z3")

# Souffle (optional at M0)
find_package(Souffle QUIET)
if(Souffle_FOUND)
  message(STATUS "VERITAS: Found Souffle (optional)")
else()
  message(STATUS "VERITAS: Souffle not found (optional at M0)")
endif()
```

- [ ] **Step 4: Create cmake/VeritasWarnings.cmake**

```cmake
# Compiler warnings for VERITAS targets

function(veritas_add_warnings target_name)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
      -Wno-unused-parameter
    )
  elseif(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      /WX
    )
  endif()
endfunction()
```

- [ ] **Step 5: Verify LLVM/SVF compatibility**

Add to `cmake/VerifySvfSubmodule.cmake` after `add_subdirectory(third_party/SVF)`:

```cmake
# Verify VERITAS and SVF use compatible LLVM
get_target_property(SVF_COMPILE_OPTIONS SvfCore INTERFACE_COMPILE_OPTIONS)
get_target_property(SVF_COMPILE_FEATURES SvfCore INTERFACE_COMPILE_FEATURES)

# Check RTTI consistency (both VERITAS and SVF require RTTI)
if(NOT LLVM_ENABLE_RTTI)
  message(FATAL_ERROR
    "VERITAS requires LLVM with RTTI enabled.\n"
    "Rebuild LLVM with -DLLVM_ENABLE_RTTI=ON")
endif()

# Check exception handling (both require exceptions)
if(NOT LLVM_ENABLE_EH)
  message(FATAL_ERROR
    "VERITAS requires LLVM with exception handling enabled.\n"
    "Rebuild LLVM with -DLLVM_ENABLE_EH=ON")
endif()

message(STATUS "VERITAS: LLVM RTTI and EH verified compatible with SVF")
```

---

## Task 3: Implement Status and StatusOr APIs

**Files:**
- Create: `include/veritas/core/Status.h`
- Create: `src/core/Status.cpp`
- Create: `src/core/CMakeLists.txt`
- Create: `tests/unit/core/StatusTest.cpp`
- Create: `tests/unit/CMakeLists.txt`

**Interfaces:**

```cpp
namespace veritas {

enum class StatusCode {
  kOk,
  kInvalidArgument,
  kNotFound,
  kFailedPrecondition,
  kInternal
};

class Status {
 public:
  static Status Ok();
  static Status InvalidArgument(std::string message);
  static Status NotFound(std::string message);
  static Status FailedPrecondition(std::string message);
  static Status Internal(std::string message);

  bool ok() const;
  StatusCode code() const;
  std::string_view message() const;

 private:
  Status(StatusCode code, std::string message);
  StatusCode code_;
  std::string message_;
};

template <typename T>
class StatusOr {
 public:
  StatusOr(T value);
  StatusOr(Status status);

  bool ok() const;
  const Status& status() const;
  const T& value() const;
  T& value();
  const T& operator*() const;
  T& operator*();

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace veritas
```

- [ ] **Step 1: Write failing StatusTest**

```cpp
#include "veritas/core/Status.h"
#include <gtest/gtest.h>

TEST(StatusTest, OkStatusIsOk) {
  veritas::Status status = veritas::Status::Ok();
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), veritas::StatusCode::kOk);
}

TEST(StatusTest, ErrorStatusCarriesMessage) {
  veritas::Status status = veritas::Status::InvalidArgument("bad input");
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), veritas::StatusCode::kInvalidArgument);
  EXPECT_EQ(status.message(), "bad input");
}

TEST(StatusOrTest, HoldsValue) {
  veritas::StatusOr<int> result(42);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 42);
}

TEST(StatusOrTest, HoldsError) {
  veritas::StatusOr<int> result(veritas::Status::NotFound("missing"));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), veritas::StatusCode::kNotFound);
}
```

- [ ] **Step 2: Implement Status and StatusOr**

Create `include/veritas/core/Status.h` and `src/core/Status.cpp` with the full implementation.

- [ ] **Step 3: Create src/core/CMakeLists.txt**

```cmake
add_library(veritas_core
  Status.cpp
  Version.cpp
)

target_include_directories(veritas_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

veritas_add_warnings(veritas_core)
```

- [ ] **Step 4: Create tests/unit/CMakeLists.txt**

```cmake
add_subdirectory(core)
```

- [ ] **Step 5: Create tests/unit/core/CMakeLists.txt**

```cmake
add_executable(StatusTest StatusTest.cpp)
target_link_libraries(StatusTest PRIVATE veritas_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(StatusTest)

add_executable(VersionTest VersionTest.cpp)
target_link_libraries(VersionTest PRIVATE veritas_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(VersionTest)
```

- [ ] **Step 6: Run StatusTest and verify it passes**

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON \
  -DLLVM_PROJECT_BUILD_DIR="/Users/skg7on/Workspace/Projects/llvm-project/build"
cmake --build build --target StatusTest
ctest --test-dir build -R StatusTest --output-on-failure
```

---

## Task 4: Implement Version API

**Files:**
- Create: `include/veritas/core/Version.h`
- Create: `src/core/Version.cpp`
- Create: `tests/unit/core/VersionTest.cpp`

**Interfaces:**

```cpp
namespace veritas {

struct Version {
  int major;
  int minor;
  int patch;
  std::string git_revision;
};

Version GetVersion();
std::string FormatVersion(const Version& version);

}  // namespace veritas
```

- [ ] **Step 1: Write failing VersionTest**

```cpp
#include "veritas/core/Version.h"
#include <gtest/gtest.h>

TEST(VersionTest, FormatsSemanticVersion) {
  veritas::Version version{0, 1, 0, "dev"};
  EXPECT_EQ(veritas::FormatVersion(version), "VERITAS 0.1.0 (dev)");
}

TEST(VersionTest, GetVersionReturnsValidVersion) {
  veritas::Version version = veritas::GetVersion();
  EXPECT_EQ(version.major, 0);
  EXPECT_EQ(version.minor, 1);
  EXPECT_EQ(version.patch, 0);
  EXPECT_FALSE(version.git_revision.empty());
}
```

- [ ] **Step 2: Implement Version API**

Create `include/veritas/core/Version.h` and `src/core/Version.cpp`.

`GetVersion()` should return `{0, 1, 0, "<git-sha>"}` where git-sha comes from `git rev-parse HEAD` at configure time (CMake configure_file or similar).

- [ ] **Step 3: Run VersionTest and verify it passes**

```bash
cmake --build build --target VersionTest
ctest --test-dir build -R VersionTest --output-on-failure
```

---

## Task 5: Implement CLI Binaries

**Files:**
- Create: `src/tools/veritas-build.cpp`
- Create: `src/tools/veritas-query.cpp`
- Create: `src/tools/veritas-diff.cpp`
- Create: `src/tools/veritas-explain.cpp`
- Create: `src/tools/CMakeLists.txt`

**Interfaces:**
- Each CLI binary accepts `--version` and prints the same format: `VERITAS x.y.z (git-sha)`

- [ ] **Step 1: Create src/tools/CMakeLists.txt**

```cmake
add_executable(veritas-build veritas-build.cpp)
target_link_libraries(veritas-build PRIVATE veritas_core)
veritas_add_warnings(veritas-build)

add_executable(veritas-query veritas-query.cpp)
target_link_libraries(veritas-query PRIVATE veritas_core)
veritas_add_warnings(veritas-query)

add_executable(veritas-diff veritas-diff.cpp)
target_link_libraries(veritas-diff PRIVATE veritas_core)
veritas_add_warnings(veritas-diff)

add_executable(veritas-explain veritas-explain.cpp)
target_link_libraries(veritas-explain PRIVATE veritas_core)
veritas_add_warnings(veritas-explain)
```

- [ ] **Step 2: Implement veritas-build.cpp with --version**

```cpp
#include "veritas/core/Version.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    veritas::Version version = veritas::GetVersion();
    std::cout << veritas::FormatVersion(version) << "\n";
    return 0;
  }

  std::cerr << "veritas-build: no analysis implemented (M0 skeleton)\n";
  return 1;
}
```

- [ ] **Step 3: Implement veritas-query.cpp with --version**

Same structure as veritas-build.cpp.

- [ ] **Step 4: Implement veritas-diff.cpp with --version**

Same structure as veritas-build.cpp.

- [ ] **Step 5: Implement veritas-explain.cpp with --version**

Same structure as veritas-build.cpp.

- [ ] **Step 6: Build all CLI binaries**

```bash
cmake --build build --target veritas-build veritas-query veritas-diff veritas-explain
```

- [ ] **Step 7: Verify --version contract**

```bash
./build/src/tools/veritas-build --version
./build/src/tools/veritas-query --version
./build/src/tools/veritas-diff --version
./build/src/tools/veritas-explain --version
```

Expected output for each: `VERITAS 0.1.0 (<git-sha>)`

---

## Task 6: Create Smoke Test Fixture

**Files:**
- Create: `tests/fixtures/projects/smoke/compile_commands.json`
- Create: `tests/fixtures/projects/smoke/smoke.cpp`
- Create: `tests/integration/CMakeLists.txt`
- Create: `tests/CMakeLists.txt`

**Purpose:**
Minimal project fixture for future integration tests. M0 does not execute analysis; this fixture establishes the structure.

- [ ] **Step 1: Create smoke.cpp**

```cpp
// Minimal C++ translation unit for M0 smoke fixture
int add(int a, int b) {
  return a + b;
}

int main() {
  return add(2, 3) == 5 ? 0 : 1;
}
```

- [ ] **Step 2: Create compile_commands.json**

```json
[
  {
    "directory": "/path/to/veritas/tests/fixtures/projects/smoke",
    "command": "clang++ -c smoke.cpp -o smoke.o",
    "file": "smoke.cpp"
  }
]
```

- [ ] **Step 3: Create tests/CMakeLists.txt**

```cmake
add_subdirectory(unit)
add_subdirectory(integration)
```

- [ ] **Step 4: Create tests/integration/CMakeLists.txt**

```cmake
# Integration tests will be added in M1+
# M0 only establishes the fixture structure
```

---

## Task 7: Document SVF Integration

**Files:**
- Create: `docs/third_party/SVF.md`

- [ ] **Step 1: Create SVF.md**

Content:

```markdown
# SVF (Static Value-Flow) Dependencies

VERITAS requires SVF for pointer analysis and value-flow graph construction.

## Upstream

- **Project**: SVF-tools/SVF
- **Repository**: https://github.com/SVF-tools/SVF.git
- **License**: AGPL-3.0-or-later
- **Pinned Revision**: `18fb5650600530a54f0afc22f4df1a10b03d3c02`
- **Path**: `third_party/SVF`

## Requirements

- **CMake**: 3.23+
- **LLVM/Clang**: 22+ (shared with VERITAS)
- **Build Mode**: Integrated via `add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)`

## Initialization

After cloning VERITAS:

\`\`\`bash
git submodule update --init --recursive third_party/SVF
\`\`\`

## Configuration

SVF inherits VERITAS's LLVM configuration via the cached `LLVM_DIR` variable set by `cmake/VeritasLLVM.cmake`.

When configuring VERITAS with `LLVM_PROJECT_BUILD_DIR`:

\`\`\`bash
cmake -S . -B build \\
  -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"
\`\`\`

Both VERITAS and SVF will use the same LLVM installation, ensuring ABI compatibility.

## Integration

VERITAS provides a private wrapper target `veritas_third_party_svf` that links `SvfCore` and `SvfLLVM`. This target is used internally but never exposed in public headers.

SVF analysis is first invoked in M5.

## License Notice

SVF is licensed under AGPL-3.0-or-later. The full license text is at `third_party/SVF/LICENSE.TXT`.

VERITAS is licensed under Apache-2.0. The SVF integration is a separate work governed by its own license.
```

---

## Milestone Verification

- [ ] Run the full build:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON \\
  -DLLVM_PROJECT_BUILD_DIR="/Users/skg7on/Workspace/Projects/llvm-project/build"
cmake --build build
```

- [ ] Run all tests:

```bash
ctest --test-dir build --output-on-failure
```

- [ ] Verify CLI --version contract:

```bash
./build/src/tools/veritas-build --version
./build/src/tools/veritas-query --version
./build/src/tools/veritas-diff --version
./build/src/tools/veritas-explain --version
```

All four must print the same version format and exit with code 0.

- [ ] Verify SVF submodule:

```bash
git -C third_party/SVF rev-parse HEAD
# Expected: 18fb5650600530a54f0afc22f4df1a10b03d3c02
```

- [ ] Verify uninitialized submodule fails configuration:

```bash
test_tmp="$(mktemp -d)"
git clone --no-hardlinks . "$test_tmp/source"
if cmake -S "$test_tmp/source" -B "$test_tmp/build" \\
    -DVERITAS_BUILD_TESTS=ON \\
    -DLLVM_PROJECT_BUILD_DIR="/Users/skg7on/Workspace/Projects/llvm-project/build" \\
    >"$test_tmp/configure.stdout" \\
    2>"$test_tmp/configure.stderr"; then
  echo "configuration unexpectedly succeeded without SVF" >&2
  exit 1
fi
grep "git submodule update --init --recursive third_party/SVF" "$test_tmp/configure.stderr"
rm -rf "$test_tmp"
```

- [ ] Verify dependency documentation:

Check `docs/third_party/SVF.md` records:
- Upstream URL
- Pinned revision
- AGPL-3.0-or-later license
- LLVM 22+ requirement
- CMake 3.23+ requirement
- Initialization command

---

## Exit Criteria

```text
All four CLI binaries build.
All four CLI binaries print the same version format with --version.
ctest passes (StatusTest, VersionTest).
SVF submodule is pinned at 18fb5650600530a54f0afc22f4df1a10b03d3c02.
Standard build provides SvfCore, SvfLLVM, and veritas_third_party_svf.
LLVM/SVF ABI compatibility is verified at configure time.
Uninitialized submodule fails configuration with recovery command.
Smoke fixture exists under tests/fixtures/projects/smoke/.
Documentation records SVF upstream, license, pinned revision, and toolchain.
```

---

## Commit Message

```text
build: add required SVF toolchain and project skeleton

Establish M0 buildable C++20 foundation with pinned SVF submodule,
LLVM/Clang 22+ dependency, and four CLI binaries.

Changes:
- Add SVF Git submodule at third_party/SVF pinned to 18fb5650...
- Create top-level CMake project with veritas_core library target
- Implement veritas::Status, StatusOr<T>, Version, FormatVersion APIs
- Create four CLI binaries: veritas-build, veritas-query, veritas-diff,
  veritas-explain, each implementing --version
- Add CMake modules: VerifySvfSubmodule, Dependencies, VeritasWarnings
- Verify LLVM/SVF ABI compatibility (RTTI, exceptions, version)
- Add smoke test fixture at tests/fixtures/projects/smoke/
- Document SVF integration in docs/third_party/SVF.md

Requirements: CMake 3.23+, LLVM/Clang 22+, Z3, Protobuf, RocksDB, SQLite,
GoogleTest. SVF is mandatory; Souffle optional at M0.

Addresses issue #3.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```
