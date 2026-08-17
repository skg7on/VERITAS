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

#include "ProjectFixture.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

#ifndef VERITAS_TESTS_SOURCE_DIR
#error "VERITAS_TESTS_SOURCE_DIR must be defined by the build system"
#endif

namespace veritas::testing {

namespace {

namespace fs = std::filesystem;

std::atomic<std::uint64_t>& FixtureCounter() {
  static std::atomic<std::uint64_t> counter{0};
  return counter;
}

fs::path MakeUniqueTestDirectory(std::string_view slug) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto pid = static_cast<std::uintmax_t>(::getpid());
  const auto seq = FixtureCounter().fetch_add(1, std::memory_order_relaxed);
  std::ostringstream name;
  name << "veritas-fixture-" << slug << "-" << pid << "-" << stamp << "-"
       << seq;

  auto path = fs::temp_directory_path() / name.str();
  std::error_code error;
  fs::create_directories(path, error);
  if (error) {
    throw std::runtime_error("cannot create fixture directory: " +
                             error.message());
  }
  return fs::canonical(path);
}

void ReplaceAllInFile(const fs::path& file, std::string_view needle,
                      std::string_view replacement) {
  std::ifstream in(file);
  if (!in) {
    throw std::runtime_error("cannot open fixture file: " + file.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  in.close();

  std::string contents = buffer.str();
  std::string::size_type position = 0;
  while ((position = contents.find(needle, position)) != std::string::npos) {
    contents.replace(position, needle.size(), replacement);
    position += replacement.size();
  }

  std::ofstream out(file, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("cannot rewrite fixture file: " + file.string());
  }
  out << contents;
}

}  // namespace

fs::path TestSourceRoot() {
  return fs::path(VERITAS_TESTS_SOURCE_DIR);
}

fs::path FixtureProject(std::string_view name) {
  const auto source = TestSourceRoot() / "fixtures" / "projects" / name;
  if (!fs::is_directory(source)) {
    throw std::runtime_error("unknown project fixture: " + source.string());
  }

  const auto destination = MakeUniqueTestDirectory(name);
  std::error_code copy_error;
  fs::copy(source, destination,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing,
           copy_error);
  if (copy_error) {
    throw std::runtime_error("cannot copy fixture " + source.string() +
                             " to " + destination.string() + ": " +
                             copy_error.message());
  }

  const auto database = destination / "compile_commands.json";
  if (fs::exists(database)) {
    ReplaceAllInFile(database, "@PROJECT_ROOT@", destination.string());
  }
  return destination;
}

}  // namespace veritas::testing
