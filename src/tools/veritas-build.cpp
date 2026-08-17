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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "veritas/analysis/ProjectAnalysisRequest.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/build/ProjectInput.h"
#include "veritas/build/ProjectManifestLoader.h"
#include "veritas/core/Status.h"
#include "veritas/core/Version.h"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-build --version\n"
    "  veritas-build analyze --project <directory> [--output <directory>]\n"
    "\n"
    "`analyze` is the only source-input command. No `--compile-db`,\n"
    "`--manifest`, `--bitcode`, `--llvm-module`, or `--svf-input` alternative\n"
    "is accepted; the project directory is the sole public source-input.\n";

struct AnalyzeArguments {
  fs::path project;
  fs::path output;
};

veritas::StatusOr<AnalyzeArguments> ParseAnalyzeArguments(
    const std::vector<std::string>& args) {
  static constexpr std::string_view kRejectedFlags[] = {
      "--compile-db", "--manifest", "--bitcode", "--llvm-module", "--svf-input"};

  AnalyzeArguments parsed;
  bool project_seen = false;
  bool output_seen = false;

  auto rejection_error = [](std::string_view flag) {
    return veritas::Status::InvalidArgument(
        std::string("veritas-build analyze does not accept ") +
        std::string(flag) +
        "; the project directory is the only source-input abstraction.");
  };
  auto is_rejected = [](std::string_view value) -> std::string_view {
    for (const auto rejected : kRejectedFlags) {
      if (value == rejected) return rejected;
    }
    return {};
  };

  // Consume the next argument as an option value. We validate that it exists
  // and that it is not itself a rejected artifact flag — otherwise
  // `veritas-build analyze --project --compile-db path` would silently take
  // `--compile-db` as the project path and bypass the rejection contract.
  auto take_value = [&](std::size_t& i, std::string_view option)
      -> veritas::StatusOr<std::string> {
    if (i + 1 >= args.size()) {
      return veritas::Status::InvalidArgument(
          std::string(option) + " requires a value argument");
    }
    const auto& value = args[i + 1];
    if (const auto rejected = is_rejected(value); !rejected.empty()) {
      return rejection_error(rejected);
    }
    if (!value.empty() && value.front() == '-') {
      return veritas::Status::InvalidArgument(
          std::string(option) + " requires a value, got flag: " + value);
    }
    ++i;
    return value;
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (const auto rejected = is_rejected(arg); !rejected.empty()) {
      return rejection_error(rejected);
    }
    if (arg == "--project") {
      if (project_seen) {
        return veritas::Status::InvalidArgument(
            "--project must be provided exactly once");
      }
      auto value = take_value(i, "--project");
      if (!value.ok()) return value.status();
      parsed.project = *value;
      project_seen = true;
    } else if (arg == "--output") {
      if (output_seen) {
        return veritas::Status::InvalidArgument(
            "--output must be provided at most once");
      }
      auto value = take_value(i, "--output");
      if (!value.ok()) return value.status();
      parsed.output = *value;
      output_seen = true;
    } else {
      return veritas::Status::InvalidArgument("unknown argument: " + arg);
    }
  }

  if (!project_seen) {
    return veritas::Status::InvalidArgument(
        "--project is required for `veritas-build analyze`");
  }
  return parsed;
}

int ReportStatus(const veritas::Status& status) {
  std::cerr << "veritas-build: " << status.message() << '\n';
  return 1;
}

veritas::Status WriteDiagnosticManifest(
    const fs::path& output_root,
    const veritas::build::AnalysisManifest& manifest) {
  std::error_code error;
  fs::create_directories(output_root, error);
  if (error) {
    return veritas::Status::Internal("cannot create output directory " +
                                     output_root.string() + ": " +
                                     error.message());
  }
  const auto path = output_root / "manifest.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return veritas::Status::Internal("cannot write manifest: " + path.string());
  }
  out << veritas::build::ToDiagnosticJson(manifest);
  return veritas::Status::Ok();
}

veritas::Status Analyze(const std::vector<std::string>& args) {
  auto parsed = ParseAnalyzeArguments(args);
  if (!parsed.ok()) return parsed.status();

  const veritas::analysis::ProjectAnalysisRequest request{
      .project_root = parsed->project,
      .output_root = parsed->output,
  };
  auto input = veritas::build::ResolveProjectInput(request);
  if (!input.ok()) return input.status();

  auto manifest = veritas::build::LoadProjectManifest(*input);
  if (!manifest.ok()) return manifest.status();

  if (auto status = WriteDiagnosticManifest(input->output_root, *manifest);
      !status.ok()) {
    return status;
  }

  std::cout << "Project: " << input->project_root.string() << '\n'
            << "Repository: " << manifest->context.repository_id << '\n'
            << "Revision: " << manifest->context.revision_id << '\n'
            << "Build Variant: " << manifest->context.build_variant_id << '\n'
            << "Translation Units: " << manifest->translation_units.size()
            << '\n'
            << "Diagnostic Manifest: "
            << (input->output_root / "manifest.json").string() << '\n';
  return veritas::Status::Ok();
}

int RunAnalyze(const std::vector<std::string>& args) {
  const auto status = Analyze(args);
  return status.ok() ? 0 : ReportStatus(status);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << veritas::FormatVersion(veritas::GetVersion()) << '\n';
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "analyze") == 0) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
    return RunAnalyze(args);
  }
  std::cerr << kUsage;
  return 1;
}
