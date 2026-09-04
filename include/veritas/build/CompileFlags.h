// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// CompileFlags.h — derive compiler flags from a normalized M1 translation-unit
// command. The returned flags are suitable for clang::tooling::FixedCompilationDatabase:
// argv[0] (the driver), the `-c`/`-o` driver flags, and the source file are all
// dropped, and the M1 `<repo>` sentinel is re-substituted with the project root.
// FixedCompilationDatabase prepends its own driver and appends the source file,
// so its ArgList must contain only the compiler flags.

#ifndef VERITAS_BUILD_COMPILEFLAGS_H_
#define VERITAS_BUILD_COMPILEFLAGS_H_

#include <filesystem>
#include <string>
#include <vector>

#include "veritas/build/AnalysisManifest.h"

namespace veritas::build {

// Re-substitute the M1 `<repo>` sentinel (written during argument
// normalization) with the actual project root so Clang can resolve paths.
std::string ResolveArguments(const std::string& argument,
                             const std::string& project_root);

// The compiler flags for one translation unit: argv[0], `-c`, `-o <file>`, and
// the source file are stripped; the remaining arguments are `<repo>`-resolved.
std::vector<std::string> CompileFlags(
    const TranslationUnitCommand& command,
    const std::filesystem::path& project_root);

}  // namespace veritas::build

#endif  // VERITAS_BUILD_COMPILEFLAGS_H_
