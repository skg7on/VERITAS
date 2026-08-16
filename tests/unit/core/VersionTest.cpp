// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
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
