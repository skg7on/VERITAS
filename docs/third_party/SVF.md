# SVF (Static Value-Flow Analysis)

VERITAS vendors [SVF](https://github.com/SVF-tools/SVF) directly into
`third_party/SVF/` and builds it with `add_subdirectory(... EXCLUDE_FROM_ALL)`
so `SvfCore` and `SvfLLVM` become in-tree CMake targets. SVF is required by
the standard VERITAS build; there is no `VERITAS_ENABLE_SVF` toggle and no
`FindSVF.cmake` support for system-provided SVF.

## Upstream and version

| Field    | Value                                                       |
| -------- | ----------------------------------------------------------- |
| Source   | https://github.com/SVF-tools/SVF                            |
| Tag      | `SVF-3.3`                                                   |
| Commit   | `5c45081f75d16afffc5fc9121e1f2f7a614e0bef`                  |
| Released | 2026-05-20                                                  |
| License  | GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later) |

The full upstream license text lives in `third_party/SVF/LICENSE.TXT` and is
preserved unchanged. Any downstream distribution of VERITAS that includes
`SvfCore` / `SvfLLVM` must comply with AGPL-3.0-or-later.

Upstream targets LLVM 21.1.0. VERITAS builds against a newer LLVM (24.x from
`LLVM_PROJECT_BUILD_DIR`), and applies the local patches described below to
close the API drift.

## Toolchain

- LLVM/Clang 24.x from a local `llvm-project` build tree, pointed at via the
  `LLVM_PROJECT_BUILD_DIR` CMake variable. `LLVM_DIR` and `Clang_DIR` are
  derived from that build tree.
- Z3 (SVF hard requirement). On macOS install via `brew install z3` and
  point at it with `Z3_DIR=$(brew --prefix z3)`.
- CMake 3.23 or newer.
- A C++17-capable compiler (SVF uses C++17; VERITAS itself uses C++20).

## Fetching (developers)

The vendored source is committed directly; a fresh clone of VERITAS already
contains `third_party/SVF`. Nothing needs to be initialised. There is no SVF
submodule.

To refresh the vendored source in future, replace the tree with a new
upstream tag, re-apply the patches in this file, and update the version
table above.

## Local patches (LLVM 24 API drift)

SVF-3.3 was authored against LLVM 21. Building against LLVM 24 requires the
following localised changes. Each hunk is guarded by `LLVM_VERSION_MAJOR`
so older LLVM versions keep working.

### 1. `UnifyFunctionExitNodes` pass removed upstream

LLVM upstream removed `llvm::UnifyFunctionExitNodesPass` in commit
`30abd9ec2b8d` ("[UnifyFunctionExitNodes] Remove the pass", PR
[llvm/llvm-project#205519](https://github.com/llvm/llvm-project/pull/205519)).
SVF's `LLVMModuleSet::prePassSchedule` still relies on the transformation to
normalise each function to a single return / unreachable block before
points-to analysis.

Fix: port the deleted upstream logic into
`third_party/SVF/svf-llvm/include/SVF-LLVM/UnifyFunctionExitNodes.h` as a
self-contained `SVF::unifyFunctionExitNodes(llvm::Function&)` helper built
on stable LLVM IR APIs (`BranchInst::Create`, `ReturnInst::Create`, etc).
The port carries the original Apache-2.0 WITH LLVM-exception header.

Files touched:
- `third_party/SVF/svf-llvm/include/SVF-LLVM/UnifyFunctionExitNodes.h` (new)
- `third_party/SVF/svf-llvm/include/SVF-LLVM/BasicTypes.h`
  - Guarded `#include <llvm/Transforms/Utils/UnifyFunctionExitNodes.h>` to `LLVM_VERSION_MAJOR <= 16`.
  - Removed the `LLVM_VERSION_MAJOR > 16` typedef branch; SVF no longer needs the pass type name for that range.
- `third_party/SVF/svf-llvm/include/SVF-LLVM/BreakConstantExpr.h`
  - Replaced the `PassBuilder` / `FunctionPassManager` plumbing under `LLVM_VERSION_MAJOR > 16` with a direct call to `SVF::unifyFunctionExitNodes`.
- `third_party/SVF/svf-llvm/lib/LLVMModule.cpp`
  - Same replacement in `LLVMModuleSet::prePassSchedule`.

### 2. `llvm::DITypeRefArray` renamed to `llvm::DITypeArray`

Files touched:
- `third_party/SVF/svf-llvm/include/SVF-LLVM/BasicTypes.h` — version-gated typedef under `LLVM_VERSION_MAJOR >= 24`.

### 3. Debug intrinsics migrated to debug records

LLVM 24 replaced `llvm::findDbgDeclares(...)` (returning
`SmallVector<DbgDeclareInst *>`) with `llvm::findDVRDeclares(...)`
(returning `TinyPtrVector<DbgVariableRecord *>`). `DbgVariableRecord`
exposes `isDbgDeclare()` and `getVariable() -> DILocalVariable *`.

Files touched:
- `third_party/SVF/svf-llvm/lib/LLVMUtil.cpp::LLVMUtil::getSourceLoc` — added a `LLVM_VERSION_MAJOR >= 24` branch that iterates `DbgVariableRecord *` and uses the new predicates.

## Building

The canonical configure uses the `default` preset, which pins the Ninja
generator and the binary directory to `<repo>/build`:

```bash
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR="/path/to/llvm-project/build" \
  -DZ3_DIR="$(brew --prefix z3)"
cmake --build --preset default
```

`SvfCore`, `SvfLLVM`, and the private `veritas_third_party_svf` wrapper (M0
deliverable) become available as CMake targets. Nothing needs to be
initialised at clone time.

## Verified build

- Debug build, arm64-apple-darwin against `llvm-project` at LLVM 24.0.0git
  (`LLVM_ENABLE_RTTI=OFF`, `LLVM_ENABLE_EH=OFF`).
- All SVF targets compile and link: `SvfCore`, `SvfLLVM`, and the front-end
  binaries `wpa`, `ae`, `dvf`, `cfl`, `mta`, `saber`, `svf-ex`, `llvm2svf`.
- SVF auto-downgrades its own `SVF_ENABLE_RTTI` to match `LLVM_ENABLE_RTTI`;
  SVF's hand-rolled `SVFUtil::isa/cast/dyn_cast` hierarchy does not depend
  on LLVM's `dynamic_cast`.

End-to-end analysis correctness against real bitcode has **not** been
verified yet. Add a smoke fixture (an LLVM bitcode file exercising both
`unifyFunctionExitNodes` and the debug-record path) before relying on
results.

## Files under this policy

The vendored source is at `third_party/SVF/`. Any modification to that
subtree must be recorded above and stay minimal.
