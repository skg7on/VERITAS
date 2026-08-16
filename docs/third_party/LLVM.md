# LLVM and Clang Dependencies

VERITAS requires LLVM/Clang 22.x for C/C++ frontend analysis, IR generation, and local static analysis.

## Upstream

- **Project**: LLVM Compiler Infrastructure
- **Repository**: https://github.com/llvm/llvm-project
- **License**: Apache-2.0 WITH LLVM-exception
- **Required Version**: LLVM/Clang 22.x
- **Components Used**: LLVM Core, Clang LibTooling, Clang CodeGen

## Configuration

VERITAS supports two approaches for locating LLVM and Clang:

### Option 1: Local LLVM Build (Recommended for Development)

If you have a local LLVM monorepo build, use the `LLVM_PROJECT_BUILD_DIR` CMake variable to point to your build directory:

```bash
cmake -S . -B build \
  -DVERITAS_BUILD_TESTS=ON \
  -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"
```

**Example** (macOS development setup):

```bash
cmake -S . -B build \
  -DVERITAS_BUILD_TESTS=ON \
  -DLLVM_PROJECT_BUILD_DIR="/Users/skg7on/Workspace/Projects/llvm-project/build"
```

VERITAS will automatically derive:
- `LLVM_DIR` = `${LLVM_PROJECT_BUILD_DIR}/lib/cmake/llvm`
- `Clang_DIR` = `${LLVM_PROJECT_BUILD_DIR}/lib/cmake/clang`

**Benefits:**
- Avoids duplicate LLVM builds during development
- Ensures VERITAS and SVF use the same LLVM installation
- Faster iteration when working on both LLVM and VERITAS

### Option 2: System-Installed or Separate LLVM

If LLVM is installed system-wide or in a non-standard location, set `LLVM_DIR` and `Clang_DIR` explicitly:

```bash
cmake -S . -B build \
  -DVERITAS_BUILD_TESTS=ON \
  -DLLVM_DIR="/usr/local/lib/cmake/llvm" \
  -DClang_DIR="/usr/local/lib/cmake/clang"
```

Or let CMake search standard system paths:

```bash
cmake -S . -B build -DVERITAS_BUILD_TESTS=ON
```

## Building LLVM Locally (Optional)

If you need to build LLVM from source for VERITAS development:

```bash
# Clone the LLVM monorepo
git clone https://github.com/llvm/llvm-project.git
cd llvm-project

# Checkout LLVM 22.x
git checkout release/22.x

# Configure LLVM with Clang and required components
cmake -S llvm -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_EH=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"

# Build (adjust -j based on your system)
cmake --build build -j$(nproc)

# Optional: Install to a local prefix
cmake --build build --target install
```

Then use the build directory with VERITAS:

```bash
cd /path/to/veritas
cmake -S . -B build \
  -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"
```

## Compatibility Requirements

VERITAS and SVF must use compatible LLVM configurations:

| Setting | Required Value | Rationale |
|---------|---------------|-----------|
| LLVM Version | 22.x | VERITAS targets LLVM 22 APIs; SVF submodule is pinned to compatible revision |
| RTTI | ON | VERITAS and SVF require LLVM RTTI for type introspection |
| Exceptions | ON | C++ exception handling must be consistent across VERITAS/LLVM/SVF |
| ABI | Match host compiler | Prevents linkage failures and UB from ABI mismatches |

The VERITAS CMake configuration verifies these constraints at configure time.

## Troubleshooting

### CMake cannot find LLVMConfig.cmake

**Symptom:**
```
Could NOT find LLVM (missing: LLVM_DIR)
```

**Solution:**
Set `LLVM_PROJECT_BUILD_DIR` or `LLVM_DIR` explicitly:

```bash
cmake -S . -B build -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"
```

### Version mismatch between VERITAS and SVF LLVM

**Symptom:**
```
VERITAS requires LLVM 22.x but found LLVM 18.x
```

**Solution:**
Ensure your LLVM build is version 22.x. Check with:

```bash
/path/to/llvm-project/build/bin/llvm-config --version
```

### RTTI or exception handling mismatch

**Symptom:**
```
undefined reference to `typeinfo for llvm::Value`
```

**Solution:**
Rebuild LLVM with `-DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON`.

## SVF Integration

The pinned SVF submodule at `third_party/SVF` also depends on LLVM 22.x. When you configure VERITAS with `LLVM_PROJECT_BUILD_DIR`, both VERITAS and SVF will use the same LLVM installation, ensuring ABI compatibility.

### How It Works

The `cmake/VeritasLLVM.cmake` module sets `LLVM_DIR` as a CMake cache variable. When SVF is added via `add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)`, it inherits this cached `LLVM_DIR` and finds the same LLVM configuration.

**Example top-level CMakeLists.txt (M0):**

```cmake
# Configure LLVM first
include(cmake/VeritasLLVM.cmake)  # Sets LLVM_DIR from LLVM_PROJECT_BUILD_DIR

# SVF inherits the cached LLVM_DIR
add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)
```

### Verification

To verify VERITAS and SVF use the same LLVM:

```bash
# After configuration
cmake -S . -B build -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build"

# Check that both found the same LLVM
grep "Found LLVM" build/CMakeCache.txt
grep "LLVM_DIR" build/CMakeCache.txt
```

Both VERITAS and SVF should report the same LLVM version and library directory.

### Requirements

- **LLVM Version**: Both VERITAS and SVF require LLVM 22.x
- **RTTI**: Must be enabled (`-DLLVM_ENABLE_RTTI=ON`)
- **Exceptions**: Must be enabled (`-DLLVM_ENABLE_EH=ON`)
- **ABI Consistency**: All three components (VERITAS, LLVM, SVF) must use compatible compiler flags

The M0 milestone will verify these constraints at configure time.

See `docs/third_party/SVF.md` for SVF-specific configuration details.

## References

- [LLVM CMake Documentation](https://llvm.org/docs/CMake.html)
- [Building LLVM with CMake](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)
- [Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html)
