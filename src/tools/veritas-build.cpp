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

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    for (const auto rejected : kRejectedFlags) {
      if (arg == rejected) {
        return veritas::Status::InvalidArgument(
            std::string("veritas-build analyze does not accept ") +
            std::string(rejected) +
            "; the project directory is the only source-input abstraction.");
      }
    }
    if (arg == "--project") {
      if (i + 1 >= args.size()) {
        return veritas::Status::InvalidArgument(
            "--project requires a directory argument");
      }
      if (project_seen) {
        return veritas::Status::InvalidArgument(
            "--project must be provided exactly once");
      }
      parsed.project = args[++i];
      project_seen = true;
    } else if (arg == "--output") {
      if (i + 1 >= args.size()) {
        return veritas::Status::InvalidArgument(
            "--output requires a directory argument");
      }
      if (output_seen) {
        return veritas::Status::InvalidArgument(
            "--output must be provided at most once");
      }
      parsed.output = args[++i];
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

int WriteDiagnosticManifest(const fs::path& output_root,
                            const veritas::build::AnalysisManifest& manifest) {
  std::error_code error;
  fs::create_directories(output_root, error);
  if (error) {
    std::cerr << "veritas-build: cannot create output directory "
              << output_root.string() << ": " << error.message() << '\n';
    return 1;
  }
  const auto path = output_root / "manifest.json";
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    std::cerr << "veritas-build: cannot write manifest: " << path.string()
              << '\n';
    return 1;
  }
  out << veritas::build::ToDiagnosticJson(manifest);
  return 0;
}

int RunAnalyze(const std::vector<std::string>& args) {
  auto parsed = ParseAnalyzeArguments(args);
  if (!parsed.ok()) return ReportStatus(parsed.status());

  const veritas::analysis::ProjectAnalysisRequest request{
      .project_root = parsed->project,
      .output_root = parsed->output,
  };
  auto input = veritas::build::ResolveProjectInput(request);
  if (!input.ok()) return ReportStatus(input.status());

  auto manifest = veritas::build::LoadProjectManifest(*input);
  if (!manifest.ok()) return ReportStatus(manifest.status());

  if (const int rc = WriteDiagnosticManifest(input->output_root, *manifest);
      rc != 0) {
    return rc;
  }

  std::cout << "Project: " << input->project_root.string() << '\n';
  std::cout << "Repository: " << manifest->context.repository_id << '\n';
  std::cout << "Revision: " << manifest->context.revision_id << '\n';
  std::cout << "Build Variant: " << manifest->context.build_variant_id << '\n';
  std::cout << "Translation Units: " << manifest->translation_units.size()
            << '\n';
  std::cout << "Diagnostic Manifest: "
            << (input->output_root / "manifest.json").string() << '\n';
  return 0;
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
