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

#include "veritas/core/AtomicFile.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

namespace veritas::core {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

TEST(AtomicFileTest, DoesNotFollowAPrecreatedTemporarySymlink) {
  namespace fs = std::filesystem;
  const fs::path area =
      fs::temp_directory_path() /
      ("veritas-atomic-file-test-" + std::to_string(::getpid()));
  std::error_code ignored;
  fs::remove_all(area, ignored);
  ASSERT_TRUE(fs::create_directories(area));

  const fs::path victim = area / "victim.txt";
  const fs::path destination = area / "result.bin";
  fs::path predictable_temporary = destination;
  predictable_temporary += ".tmp.";
  predictable_temporary += std::to_string(::getpid());
  predictable_temporary += ".1";
  {
    std::ofstream output(victim, std::ios::binary);
    output << "victim contents";
  }
  std::error_code symlink_error;
  fs::create_symlink(victim, predictable_temporary, symlink_error);
  ASSERT_FALSE(symlink_error) << symlink_error.message();

  const Status status = WriteFileAtomically(destination, "replacement");
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(fs::is_symlink(destination));
  EXPECT_TRUE(fs::is_regular_file(destination));
  EXPECT_EQ(ReadFile(destination), "replacement");
  EXPECT_EQ(ReadFile(victim), "victim contents");

  fs::remove_all(area, ignored);
}

TEST(AtomicFileTest, AppliesTheCallersUmaskToANewDestination) {
  namespace fs = std::filesystem;
  const fs::path area =
      fs::temp_directory_path() /
      ("veritas-atomic-file-mode-test-" + std::to_string(::getpid()));
  std::error_code ignored;
  fs::remove_all(area, ignored);
  ASSERT_TRUE(fs::create_directories(area));

  const mode_t previous_umask = ::umask(0027);
  const fs::path destination = area / "result.bin";
  const Status status = WriteFileAtomically(destination, "contents");
  ::umask(previous_umask);

  EXPECT_TRUE(status.ok()) << status.message();
  struct stat metadata {};
  ASSERT_EQ(::stat(destination.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0640);

  fs::remove_all(area, ignored);
}

}  // namespace
}  // namespace veritas::core
