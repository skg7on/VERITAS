# GitHub Actions CI Build Design

## Status

Approved for implementation.

## Purpose

Add the first GitHub Actions continuous-integration workflow for VERITAS. The
workflow must reproduce the documented build on GitHub-hosted Ubuntu 24.04,
compile the vendored SVF dependency, and run the registered tests. LLVM must be
built from pinned source with the configuration required by VERITAS, then
reused from the GitHub Actions cache so ordinary CI runs do not download or
rebuild it.

## Scope

The change adds one workflow under `.github/workflows/`. It covers dependency
installation, source-built Z3 and LLVM toolchains, the VERITAS build, explicit
SVF targets, and CTest. It does not cache VERITAS build products, publish
release artifacts, add additional operating systems, or introduce deployment
jobs.

## Workflow Entry Points

The workflow runs for:

- pull requests targeting `main`;
- pushes to `main`; and
- manual `workflow_dispatch` runs.

One build job runs on the explicit `ubuntu-24.04` runner label. The workflow
uses read-only repository permissions, cancels superseded runs for the same
workflow and Git ref, and has a 180-minute timeout for the initial LLVM build.

## Pinned Inputs

All third-party Actions and source repositories use immutable commit SHAs.

| Input | Version or commit | Purpose |
| --- | --- | --- |
| `actions/checkout` | `3d3c42e5aac5ba805825da76410c181273ba90b1` (`v7.0.1`) | Checkout VERITAS and pinned dependency sources |
| `actions/cache` | `27d5ce7f107fe9357f9df03efb73ab90386fccae` (`v5.0.5`) | Restore and save installed dependency trees |
| LLVM | `860fcb7accb22e57a020a353a39f2fdbd0dc1b44` | Exact LLVM 24 development source used by CI |
| Z3 | `ddb49568d3520e99799e364fb22f35fc67d887b1` (`z3-4.16.0`) | Provide the required CMake package absent from Ubuntu's `libz3-dev` package |

The action tags appear only as comments beside the pinned SHAs for
maintainability; tags are not used as executable references.

## Dependency Provisioning

Ubuntu packages provide the native compiler toolchain, CMake, Ninja, Python,
and development packages needed by LLVM and VERITAS. The workflow installs
`build-essential`, `cmake`, `ninja-build`, `python3`, `git`, `pkg-config`,
`libprotobuf-dev`, `protobuf-compiler`, `librocksdb-dev`, `libsqlite3-dev`,
`libgtest-dev`, `zlib1g-dev`, `libzstd-dev`, `libxml2-dev`, `libedit-dev`,
`libffi-dev`, and `libtinfo-dev`.

Z3 4.16.0 is configured and installed from its pinned source into
`${{ runner.temp }}/z3-install`. That install directory is cached independently
from LLVM with key
`veritas-ubuntu-24.04-x86_64-z3-ddb49568d3520e99799e364fb22f35fc67d887b1-r1`.
A Z3 cache miss checks out and builds Z3; an exact hit skips those steps. The
Z3 cache also has no partial restore keys.

## LLVM Build and Cache

LLVM is configured from the pinned LLVM 24 source with:

- a Release build;
- the Clang project enabled;
- only the X86 target enabled;
- RTTI and exception handling enabled;
- `LLVM_BUILD_LLVM_DYLIB` and `LLVM_LINK_LLVM_DYLIB` enabled to reduce the
  installed footprint; and
- tests, examples, benchmarks, and documentation disabled.

The source checkout occurs only when the LLVM cache is absent. A miss builds
and installs LLVM into `${{ runner.temp }}/llvm-install`, using at most four
parallel build jobs. The workflow
then removes the LLVM source and build trees before building VERITAS to recover
runner disk space.

Only LLVM's install tree is cached. Source and intermediate build trees are not
cached because they are large, unnecessary on a hit, and likely to exceed
GitHub-hosted runner disk or repository cache limits.

The initial cache key is
`veritas-ubuntu-24.04-x86_64-llvm-860fcb7accb22e57a020a353a39f2fdbd0dc1b44-r1`.
It contains:

- the explicit `ubuntu-24.04` platform;
- the X86 architecture;
- the complete LLVM source commit; and
- a manually incremented LLVM build-configuration revision.

The cache has no partial restore keys. Therefore a changed source commit or
build configuration cannot restore an ABI-incompatible LLVM installation. On
an exact cache hit, LLVM checkout, configuration, compilation, and installation
are all skipped.

After restore or installation, the workflow verifies `llvm-config`, `clang`,
and the installed LLVM and Clang CMake package files. A corrupt exact cache
fails with an actionable validation error instead of silently using an
incomplete toolchain.

## VERITAS Build and Test Flow

The job configures VERITAS with Ninja and Release mode. It passes the LLVM
installation root through `LLVM_PROJECT_BUILD_DIR`, the Z3 CMake package through
`Z3_DIR`, and selects the installed Clang compilers.

The workflow executes the canonical build sequence:

```text
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM_INSTALL_DIR/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_INSTALL_DIR/bin/clang++" \
  -DLLVM_PROJECT_BUILD_DIR="$LLVM_INSTALL_DIR" \
  -DZ3_DIR="$Z3_INSTALL_DIR/lib/cmake/z3"
cmake --build build --parallel
cmake --build build --target SvfCore SvfLLVM --parallel
ctest --test-dir build --output-on-failure
```

The normal build validates VERITAS libraries, tools, and unit-test binaries.
The explicit SVF targets ensure the required vendored dependency actually
compiles even though it is excluded from the default `all` target. CTest runs
all registered unit tests and repository checks, including the tracked
pre-commit hook test.

## Failure Behavior

- Package installation, dependency configuration, compilation, or tests fail
  the job immediately.
- A dependency cache miss is normal and activates the corresponding pinned
  source build.
- A dependency cache hit must pass tool and CMake-package validation.
- Compiler and test failures retain their native command output in the Actions
  log; CTest additionally prints failed-test output.
- Superseded branch runs are cancelled to avoid wasting runner time on a large
  LLVM cache miss.

## Verification and Acceptance Criteria

Implementation is complete when:

1. the workflow YAML parses and uses only pinned third-party Action SHAs;
2. its Ubuntu package list covers every required project and LLVM dependency;
3. a cache miss builds and validates pinned Z3 and LLVM installations;
4. an exact LLVM cache hit skips LLVM checkout and every LLVM build step;
5. VERITAS configures and builds against the installed LLVM and Z3 packages;
6. `SvfCore` and `SvfLLVM` build successfully;
7. all registered CTest tests pass; and
8. the first pushed workflow run is inspected in GitHub Actions, followed by a
   rerun or later run confirming the LLVM cache hit path.

GitHub may evict a cache that is unused for its retention period or when the
repository exceeds its cache quota. Such eviction causes one expected rebuild;
otherwise unchanged CI runs reuse the installed LLVM toolchain.
