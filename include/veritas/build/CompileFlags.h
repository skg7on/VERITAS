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

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "veritas/build/AnalysisManifest.h"

namespace veritas::build {

// Re-substitute the M1 `<repo>` sentinel (written during argument
// normalization) with the actual project root so Clang can resolve paths.
inline std::string ResolveArguments(const std::string& argument,
                                    const std::string& project_root) {
  std::string out = argument;
  std::size_t pos = 0;
  while ((pos = out.find("<repo>", pos)) != std::string::npos) {
    out.replace(pos, 6, project_root);
    pos += project_root.size();
  }
  return out;
}

// The compiler flags for one translation unit: argv[0], `-c`, `-o <file>`, and
// the source file are stripped; the remaining arguments are `<repo>`-resolved.
inline std::vector<std::string> CompileFlags(
    const TranslationUnitCommand& command,
    const std::filesystem::path& project_root) {
  const std::string source_basename =
      command.source_path.relative_path.filename().string();
  std::vector<std::string> flags;
  flags.reserve(command.arguments.size());
  for (std::size_t i = 1; i < command.arguments.size(); ++i) {
    const std::string& arg = command.arguments[i];
    if (arg == "-c") continue;
    if (arg == "-o") {
      ++i;  // skip the output path
      continue;
    }
    if (arg == source_basename) continue;
    if (arg == command.source_path.relative_path.generic_string()) continue;
    flags.push_back(ResolveArguments(arg, project_root.string()));
  }
  return flags;
}

}  // namespace veritas::build

#endif  // VERITAS_BUILD_COMPILEFLAGS_H_
