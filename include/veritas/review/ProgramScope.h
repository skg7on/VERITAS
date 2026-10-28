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

#ifndef VERITAS_REVIEW_PROGRAM_SCOPE_H_
#define VERITAS_REVIEW_PROGRAM_SCOPE_H_
#include "veritas/review/ReviewTypes.h"
#include <map>
#include <optional>
namespace veritas::review {
struct ProgramScopeInput {
  std::optional<Json> manifest;
  std::string target_key;
  std::string membership_source = "EXPLICIT_DECLARATION";
  std::map<std::string, std::string> translation_unit_digests;
  std::map<std::string, std::string> artifact_digests;
};
// Manifest schema errors are InvalidArgument. Missing
// targets/dependencies/input artifacts produce UNRESOLVED with a frontier,
// never a closed inferred scope.
StatusOr<ProgramScope> ResolveProgramScope(const ProgramScopeInput &);
StatusOr<std::vector<std::string>>
DeclaredProgramPaths(const Json &, std::string_view target_key);
// Reject absolute paths, traversal, NUL, .git and backslash aliases. Ordinary
// redundant '.' components normalize to a repository-relative spelling.
StatusOr<std::string> NormalizeReviewPath(std::string_view);
} // namespace veritas::review
#endif
