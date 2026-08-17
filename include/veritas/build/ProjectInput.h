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

// ProjectInput.h — resolved, canonical form of a ProjectAnalysisRequest.
//
// The M1 loader consumes a ProjectInput, not a raw request. Resolution
// canonicalizes the project root, pins the compilation database to
// `<project_root>/compile_commands.json`, and picks a default output
// root when the request leaves it empty. The compilation database path
// is retained as runtime metadata only; it is host-dependent and must
// not enter semantic hashes.

#ifndef VERITAS_BUILD_PROJECTINPUT_H_
#define VERITAS_BUILD_PROJECTINPUT_H_

#include <filesystem>

#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/core/Status.h"

namespace veritas::build {

struct ProjectInput {
  std::filesystem::path project_root;
  std::filesystem::path compile_database_path;
  std::filesystem::path output_root;
};

StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request);

}  // namespace veritas::build

#endif  // VERITAS_BUILD_PROJECTINPUT_H_
