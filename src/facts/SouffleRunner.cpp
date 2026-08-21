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

#include "veritas/facts/SouffleRunner.h"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Program.h>

namespace veritas::facts {
namespace {

Status RequireRegularFile(const std::filesystem::path &path,
                          std::string_view description) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    return Status::Internal("failed to inspect " + std::string(description) +
                            ": " + error.message());
  }
  if (!exists || !std::filesystem::is_regular_file(path, error)) {
    if (error) {
      return Status::Internal("failed to inspect " + std::string(description) +
                              ": " + error.message());
    }
    return Status::InvalidArgument(std::string(description) +
                                   " must be an existing regular file");
  }
  return Status::Ok();
}

Status RequireDirectory(const std::filesystem::path &path,
                        std::string_view description) {
  std::error_code error;
  const bool is_directory = std::filesystem::is_directory(path, error);
  if (error) {
    return Status::Internal("failed to inspect " + std::string(description) +
                            ": " + error.message());
  }
  return is_directory
             ? Status::Ok()
             : Status::InvalidArgument(std::string(description) +
                                       " must be an existing directory");
}

} // namespace

Status SouffleRunner::Run(const std::filesystem::path &executable,
                          const std::filesystem::path &rule_file,
                          const std::filesystem::path &input_directory,
                          const std::filesystem::path &output_directory) {
  auto status = RequireRegularFile(executable, "Souffle executable");
  if (!status.ok())
    return status;
  status = RequireRegularFile(rule_file, "Souffle rule file");
  if (!status.ok())
    return status;
  status = RequireDirectory(input_directory, "Souffle input directory");
  if (!status.ok())
    return status;

  std::error_code error;
  if (std::filesystem::exists(output_directory, error)) {
    if (error) {
      return Status::Internal("failed to inspect Souffle output directory: " +
                              error.message());
    }
    status = RequireDirectory(output_directory, "Souffle output directory");
    if (!status.ok())
      return status;
  } else {
    if (error) {
      return Status::Internal("failed to inspect Souffle output directory: " +
                              error.message());
    }
    std::filesystem::create_directories(output_directory, error);
    if (error) {
      return Status::Internal("failed to create Souffle output directory: " +
                              error.message());
    }
  }

  const std::string executable_text = executable.string();
  const std::string input_text = input_directory.string();
  const std::string output_text = output_directory.string();
  const std::string rule_text = rule_file.string();
  const std::vector<llvm::StringRef> arguments = {
      executable_text, "-F", input_text, "-D", output_text, rule_text};
  std::string execution_error;
  bool execution_failed = false;
  const int exit_code =
      llvm::sys::ExecuteAndWait(executable_text, arguments, std::nullopt, {}, 0,
                                0, &execution_error, &execution_failed);
  if (execution_failed || exit_code != 0) {
    std::string message =
        "Souffle execution failed with exit code " + std::to_string(exit_code);
    if (!execution_error.empty())
      message += ": " + execution_error;
    return Status::Internal(std::move(message));
  }
  return Status::Ok();
}

} // namespace veritas::facts
