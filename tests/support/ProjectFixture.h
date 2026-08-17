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

// ProjectFixture.h — helpers for materializing on-disk project fixtures.
//
// Fixtures live under `tests/fixtures/projects/`. Because `compile_commands.json`
// references its `directory` by absolute path, tests copy a fixture into a
// unique temporary directory and substitute the `@PROJECT_ROOT@` placeholder
// with the canonical temporary path. The returned path is always canonical so
// that comparisons against `veritas::build::ResolveProjectInput` (which
// canonicalizes internally) are stable.

#ifndef VERITAS_TESTS_SUPPORT_PROJECTFIXTURE_H_
#define VERITAS_TESTS_SUPPORT_PROJECTFIXTURE_H_

#include <filesystem>
#include <string_view>

namespace veritas::testing {

// Absolute path to `tests/` in the VERITAS source tree. Configured at
// build time via VERITAS_TESTS_SOURCE_DIR.
std::filesystem::path TestSourceRoot();

// Materialize the named fixture under `tests/fixtures/projects/<name>` into
// a fresh temporary directory, replace `@PROJECT_ROOT@` in
// `compile_commands.json` (when present) with the canonical destination
// path, and return that canonical destination path.
std::filesystem::path FixtureProject(std::string_view name);

}  // namespace veritas::testing

#endif  // VERITAS_TESTS_SUPPORT_PROJECTFIXTURE_H_
