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

// ProjectManifestLoader.h — load `<project>/compile_commands.json` through
// Clang LibTooling and produce a normalized, deterministic AnalysisManifest.
//
// `LoadProjectManifest` is the single M1 handoff into later milestones. It
// accepts a resolved ProjectInput, delegates JSON parsing to Clang's
// `JSONCompilationDatabase`, and rejects the whole project on any missing
// source, malformed entry, or path that cannot be classified. It never
// returns a partial manifest.

#ifndef VERITAS_BUILD_PROJECTMANIFESTLOADER_H_
#define VERITAS_BUILD_PROJECTMANIFESTLOADER_H_

#include "veritas/build/AnalysisManifest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/core/Status.h"

namespace veritas::build {

StatusOr<AnalysisManifest> LoadProjectManifest(const ProjectInput& input);

}  // namespace veritas::build

#endif  // VERITAS_BUILD_PROJECTMANIFESTLOADER_H_
