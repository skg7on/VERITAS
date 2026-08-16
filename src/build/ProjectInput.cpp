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

#include "veritas/build/ProjectInput.h"

#include <filesystem>
#include <system_error>

namespace veritas::build {

namespace fs = std::filesystem;

StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request) {
  if (request.project_root.empty()) {
    return Status::InvalidArgument("project root path is empty");
  }

  std::error_code canonical_error;
  auto root = fs::weakly_canonical(request.project_root, canonical_error);
  if (canonical_error) {
    return Status::InvalidArgument(
        "cannot canonicalize project root: " + canonical_error.message());
  }

  std::error_code stat_error;
  if (!fs::is_directory(root, stat_error) || stat_error) {
    return Status::InvalidArgument(
        "project root is not a directory: " + root.string());
  }

  auto database = root / "compile_commands.json";
  std::error_code database_error;
  if (!fs::is_regular_file(database, database_error) || database_error) {
    return Status::FailedPrecondition(
        "project root is missing compile_commands.json: " + database.string());
  }

  std::error_code size_error;
  const auto database_size = fs::file_size(database, size_error);
  if (size_error || database_size == 0) {
    return Status::FailedPrecondition(
        "compile_commands.json is empty: " + database.string());
  }

  fs::path output;
  if (request.output_root.empty()) {
    output = root / ".veritas";
  } else {
    std::error_code output_error;
    output = fs::weakly_canonical(request.output_root, output_error);
    if (output_error) {
      std::error_code absolute_error;
      output = fs::absolute(request.output_root, absolute_error);
      if (absolute_error) {
        return Status::InvalidArgument(
            "cannot resolve output root " + request.output_root.string() +
            ": " + absolute_error.message());
      }
    }
  }

  return ProjectInput{
      .project_root = std::move(root),
      .compile_database_path = std::move(database),
      .output_root = std::move(output),
  };
}

}  // namespace veritas::build
