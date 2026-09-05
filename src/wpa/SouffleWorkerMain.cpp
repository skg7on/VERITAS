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

// SouffleWorkerMain.cpp — the compiled recursive-WPA worker entry point.
//
// A thin command-line shim over the in-process runner (SouffleRunner.h): it
// parses arguments and forwards to veritas_souffle_run. The production
// executor calls that function directly; this executable remains for CLI use
// and for the provenance digest that pins the compiled rule bundles.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "veritas/wpa/SouffleRunner.h"

namespace {

void PrintUsage(std::ostream& os) {
  os << "usage: veritas-souffle-worker"
        " --component=<reachability|memory-effects>"
        " -F <input-dir> -D <output-dir> [--jobs N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string component;
  std::string input_dir;
  std::string output_dir;
  std::size_t jobs = 1;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--component=")) {
      component = std::string(arg.substr(std::strlen("--component=")));
    } else if (arg == "-F" && i + 1 < argc) {
      input_dir = argv[++i];
    } else if (arg == "-D" && i + 1 < argc) {
      output_dir = argv[++i];
    } else if (arg == "--jobs" && i + 1 < argc) {
      jobs = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage(std::cout);
      return 0;
    } else {
      std::cerr << "veritas-souffle-worker: unknown argument: " << arg << '\n';
      PrintUsage(std::cerr);
      return 2;
    }
  }

  if (component.empty() || input_dir.empty() || output_dir.empty()) {
    PrintUsage(std::cerr);
    return 2;
  }

  return veritas_souffle_run(component.c_str(), input_dir.c_str(),
                             output_dir.c_str(), static_cast<unsigned>(jobs));
}
