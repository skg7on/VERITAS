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

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "ProjectFixture.h"

namespace veritas::analysis::svf {
namespace {

bool FileContains(const std::filesystem::path& path, const std::string& pattern) {
  std::ifstream in(path);
  if (!in) return false;
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  return content.find(pattern) != std::string::npos;
}

// Search a path (file or directory) under the repository root for a pattern.
bool SourceTreeContains(const std::string& relative_path,
                        const std::string& pattern) {
  const auto root =
      testing::TestSourceRoot().parent_path() / relative_path;
  if (!std::filesystem::exists(root)) return false;
  if (std::filesystem::is_regular_file(root)) {
    return FileContains(root, pattern);
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && FileContains(entry.path(), pattern)) {
      return true;
    }
  }
  return false;
}

TEST(RequiredSvfBoundaryTest, PublicApiContainsNoNativeAnalysisTypes) {
  EXPECT_FALSE(SourceTreeContains("include/veritas", "#include <SVF"));
  EXPECT_FALSE(SourceTreeContains("include/veritas", "SVF::"));
  EXPECT_FALSE(SourceTreeContains("include/veritas/analysis/ProjectAnalyzer.h",
                                  "#include <llvm"));
  EXPECT_FALSE(SourceTreeContains("include/veritas/analysis/ProjectAnalyzer.h",
                                  "llvm::Module"));
}

TEST(RequiredSvfBoundaryTest, NoOptionalSvfToggleExists) {
  const auto cmake_file =
      testing::TestSourceRoot().parent_path() / "CMakeLists.txt";
  std::ifstream in(cmake_file);
  ASSERT_TRUE(in.is_open()) << "cannot open " << cmake_file;

  std::string line;
  while (std::getline(in, line)) {
    EXPECT_EQ(line.find("VERITAS_ENABLE_SVF"), std::string::npos)
        << "Found VERITAS_ENABLE_SVF in: " << line;
  }
}

TEST(RequiredSvfBoundaryTest, NoFindSvfModule) {
  const auto find_svf =
      testing::TestSourceRoot().parent_path() / "cmake" / "FindSVF.cmake";
  EXPECT_FALSE(std::filesystem::exists(find_svf))
      << "FindSVF.cmake should not exist; SVF is vendored and required";
}

TEST(RequiredSvfBoundaryTest, SvfHeadersRemainPrivate) {
  // SVF/LLVM native types must not leak into installed public headers.
  EXPECT_FALSE(SourceTreeContains("include/veritas", "#include <SVF"));
  EXPECT_FALSE(SourceTreeContains("include/veritas", "#include <llvm"));
}

}  // namespace
}  // namespace veritas::analysis::svf
