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

#include "veritas/core/Version.h"

#include <regex>
#include <string>

#include <gtest/gtest.h>

TEST(VersionTest, FormatVersionHasCanonicalShape) {
  veritas::Version version{0, 1, 0, "abcdef1"};
  EXPECT_EQ(veritas::FormatVersion(version), "VERITAS 0.1.0 (abcdef1)");
}

TEST(VersionTest, GetVersionReflectsProjectVersion) {
  veritas::Version version = veritas::GetVersion();
  EXPECT_EQ(version.major, 0);
  EXPECT_EQ(version.minor, 1);
  EXPECT_EQ(version.patch, 0);
}

TEST(VersionTest, GetVersionCarriesGitRevision) {
  veritas::Version version = veritas::GetVersion();
  EXPECT_FALSE(version.git_revision.empty());
}

TEST(VersionTest, FormatVersionMatchesCliContract) {
  // The four CLIs (veritas-build|query|diff|explain) share this exact
  // format for --version. Downstream users grep against it, so lock the
  // shape here.
  std::string rendered = veritas::FormatVersion(veritas::GetVersion());
  std::regex pattern(R"(^VERITAS \d+\.\d+\.\d+ \(.+\)$)");
  EXPECT_TRUE(std::regex_match(rendered, pattern)) << "actual: " << rendered;
}
