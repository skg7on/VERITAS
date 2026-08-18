# SVF Third-Party Dependency Documentation

**Path:** `third_party/SVF`

**Upstream:** https://github.com/SVF-tools/SVF.git

**Revision:** `18fb5650600530a54f0afc22f4df1a10b03d3c02`

**License:** AGPL-3.0-or-later

See `third_party/SVF/LICENSE.TXT` for the full upstream license text.

**Purpose:** SVF (Static Value-Flow Analysis) provides flow-sensitive and context-sensitive pointer analysis for LLVM IR. VERITAS uses SVF as a required component for:
- Andersen-style pointer analysis (AndersenWaveDiff)
- Value-flow graph (SVFG) construction
- Indirect call target resolution
- Memory alias facts
- Interprocedural value-flow facts

**Integration:** SVF is vendored directly in the VERITAS repository (not a git submodule). The VERITAS build system compiles SVF as part of the standard build with no optional toggle. SVF headers and types remain private to `src/analysis/svf/` implementation files and never appear in VERITAS public APIs.

**Toolchain Requirements:**
- LLVM/Clang 22+ (24.x recommended)
- CMake 3.23+
- Compatible RTTI, exception handling, and target architecture settings with LLVM
- Z3 (required by SVF)

**Build Configuration:**
- SVF is built as shared or static libraries based on `BUILD_SHARED_LIBS`
- SVF build artifacts live under `${CMAKE_BINARY_DIR}/svf-build/`
- SVF is excluded from the default `all` target but builds on-demand when VERITAS libraries link against `veritas_third_party_svf`

**Initialization:**
SVF is committed directly into `third_party/SVF/`. A clean VERITAS checkout includes the full SVF source tree. No separate initialization step is required.

**Version Selection:**
Revision `18fb5650600530a54f0afc22f4df1a10b03d3c02` is pinned for:
- LLVM 22 compatibility
- Modern CMake support (3.23+)
- Stable Andersen pointer analysis API

**Licensing Considerations:**
SVF is licensed under AGPL-3.0-or-later. VERITAS (Apache-2.0) links to SVF as a separate library. This configuration preserves the distinction between VERITAS code (Apache-2.0) and the SVF library (AGPL-3.0-or-later). Consult legal counsel for specific deployment scenarios.

**References:**
- SVF project: https://github.com/SVF-tools/SVF
- SVF documentation: https://svf-tools.github.io/SVF/
- VERITAS toolchain contract: `docs/third_party/LLVM.md`
- VERITAS SVF integration: M5 milestone design spec
