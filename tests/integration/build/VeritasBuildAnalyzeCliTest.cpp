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

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ProjectFixture.h"

#ifndef VERITAS_BUILD_BINARY
#error "VERITAS_BUILD_BINARY must be defined by the build system"
#endif

namespace veritas::build {
namespace {

namespace fs = std::filesystem;

struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
};

std::string ShellQuote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

CliResult RunVeritasBuild(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_BUILD_BINARY);
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(ShellQuote(argument));
  }
  command.append(" 2>&1");

  CliResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    result.exit_code = -1;
    return result;
  }
  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result.stdout_text.append(buffer.data());
  }
  const int status = ::pclose(pipe);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else {
    result.exit_code = -1;
  }
  return result;
}

TEST(VeritasBuildAnalyzeCliTest, AcceptsProjectLevelSourceInput) {
  const auto project = testing::FixtureProject("smoke");
  const auto result =
      RunVeritasBuild({"analyze", "--project", project.string()});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Translation Units: 1"),
            std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Repository: repo:sha256:"),
            std::string::npos)
      << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(project / ".veritas" / "manifest.json"));
}

TEST(VeritasBuildAnalyzeCliTest, HonorsExplicitOutputDirectory) {
  const auto project = testing::FixtureProject("smoke");
  const auto output = fs::temp_directory_path() /
                      ("veritas-cli-explicit-" +
                       std::to_string(std::rand()));
  const auto result = RunVeritasBuild(
      {"analyze", "--project", project.string(), "--output", output.string()});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(output / "manifest.json"));
  EXPECT_FALSE(fs::exists(project / ".veritas"));
}

TEST(VeritasBuildAnalyzeCliTest, RejectsMissingProjectFlag) {
  const auto result = RunVeritasBuild({"analyze"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("--project is required"),
            std::string::npos)
      << result.stdout_text;
}

TEST(VeritasBuildAnalyzeCliTest, RejectsArtifactInputFlags) {
  static constexpr std::array<std::string_view, 5> kFlags = {
      "--compile-db", "--manifest", "--bitcode", "--llvm-module", "--svf-input",
  };
  for (const auto flag : kFlags) {
    const auto result =
        RunVeritasBuild({"analyze", std::string(flag), "input"});
    EXPECT_NE(result.exit_code, 0) << flag;
    EXPECT_NE(result.stdout_text.find("does not accept"), std::string::npos)
        << flag << " : " << result.stdout_text;
  }
}

TEST(VeritasBuildAnalyzeCliTest, RejectsUnknownFlag) {
  const auto project = testing::FixtureProject("smoke");
  const auto result = RunVeritasBuild(
      {"analyze", "--project", project.string(), "--unknown"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("unknown argument"),
            std::string::npos)
      << result.stdout_text;
}

TEST(VeritasBuildAnalyzeCliTest, WritesDeterministicDiagnosticManifest) {
  // Materialize the same fixture into two distinct temp directories. If any
  // absolute-path leak sneaks back into the manifest, the two runs will
  // produce different bytes.
  const auto project_a = testing::FixtureProject("multiple_tus");
  const auto project_b = testing::FixtureProject("multiple_tus");
  ASSERT_NE(project_a, project_b);

  const auto output_a = fs::temp_directory_path() /
                        ("veritas-cli-det-a-" + std::to_string(std::rand()));
  const auto output_b = fs::temp_directory_path() /
                        ("veritas-cli-det-b-" + std::to_string(std::rand()));

  ASSERT_EQ(0, RunVeritasBuild({"analyze", "--project", project_a.string(),
                                "--output", output_a.string()})
                  .exit_code);
  ASSERT_EQ(0, RunVeritasBuild({"analyze", "--project", project_b.string(),
                                "--output", output_b.string()})
                  .exit_code);

  std::ifstream a(output_a / "manifest.json");
  std::ifstream b(output_b / "manifest.json");
  std::stringstream a_buf, b_buf;
  a_buf << a.rdbuf();
  b_buf << b.rdbuf();
  EXPECT_EQ(a_buf.str(), b_buf.str());
  EXPECT_FALSE(a_buf.str().empty());
}

TEST(VeritasBuildAnalyzeCliTest, RejectsFlagValueThatIsAnotherFlag) {
  // If --project consumed the next token unconditionally, `--compile-db`
  // would be swallowed as the project path and the rejection contract for
  // artifact-input flags would be bypassed.
  const auto result = RunVeritasBuild(
      {"analyze", "--project", "--compile-db", "/tmp/db.json"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_TRUE(result.stdout_text.find("does not accept --compile-db") !=
                  std::string::npos ||
              result.stdout_text.find("requires a value") != std::string::npos)
      << result.stdout_text;
}

TEST(VeritasBuildAnalyzeCliTest, WritesRunMetricsByDefault) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-cli-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(output / "run-metrics.json"));
  EXPECT_NE(result.stdout_text.find("Analysis phase report"), std::string::npos)
      << result.stdout_text;

  // Both ingest spans must appear. They are the measurable form of the
  // duplication spec section 10 records, and the first finding this feature
  // exists to surface — so their absence is a failure, not a detail.
  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  const std::string json = buffer.str();
  EXPECT_NE(json.find("cli.ingest"), std::string::npos) << json.substr(0, 500);
  EXPECT_NE(json.find("m1.ingest"), std::string::npos) << json.substr(0, 500);
  // The real root span, not the synthetic fallback: the promotion path in
  // TakeStats is what keeps the report's total equal to the command's wall time.
  EXPECT_NE(json.find("\"name\": \"run\""), std::string::npos)
      << json.substr(0, 500);
}

TEST(VeritasBuildAnalyzeCliTest, MetricsFalseWritesNoArtifactAndNoReport) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-off-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string(),
                                       "--metrics", "false"});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_FALSE(fs::exists(output / "run-metrics.json"));
  EXPECT_EQ(result.stdout_text.find("Analysis phase report"), std::string::npos)
      << result.stdout_text;
}

TEST(VeritasBuildAnalyzeCliTest, RejectsANonBooleanMetricsValue) {
  const auto result = RunVeritasBuild(
      {"analyze", "--project", "/tmp", "--metrics", "maybe"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("--metrics must be true or false"),
            std::string::npos);
}

TEST(VeritasBuildAnalyzeCliTest, AcceptsZeroSamplingInterval) {
  // Zero disables the series entirely: no sampler thread and no samples. The
  // run must still succeed and still produce a parseable artifact, because
  // turning the series off is a choice about the series, not a failure of the
  // run. (Span-boundary sampling is a different mechanism — the fallback when
  // pthread_create fails — and is not reachable through this flag.)
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-zero-" + std::to_string(std::rand()));
  const auto result =
      RunVeritasBuild({"analyze", "--project", project.string(), "--output",
                       output.string(), "--metrics-interval-ms", "0"});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  EXPECT_NE(buffer.str().find("veritas.run-metrics.v1"), std::string::npos);
}

TEST(VeritasBuildAnalyzeCliTest, HonoursTopNAndMetricsPath) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-path-" + std::to_string(std::rand()));
  const auto custom = output / "custom-metrics.json";
  const auto result = RunVeritasBuild(
      {"analyze", "--project", project.string(), "--output", output.string(),
       "--metrics-top-n", "3", "--metrics-path", custom.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_TRUE(fs::is_regular_file(custom));
  // The default location is not also written: --metrics-path replaces it.
  EXPECT_FALSE(fs::exists(output / "run-metrics.json"));
}

TEST(VeritasBuildAnalyzeCliTest, EmitsNoAbsolutePathInTheArtifact) {
  const auto project = testing::FixtureProject("multiple_tus");
  const auto output = fs::temp_directory_path() /
                      ("veritas-metrics-paths-" + std::to_string(std::rand()));
  const auto result = RunVeritasBuild({"analyze", "--project", project.string(),
                                       "--output", output.string()});
  ASSERT_EQ(result.exit_code, 0) << result.stdout_text;

  std::ifstream artifact(output / "run-metrics.json");
  ASSERT_TRUE(artifact.good());
  std::stringstream buffer;
  buffer << artifact.rdbuf();
  const std::string json = buffer.str();
  ASSERT_FALSE(json.empty());
  // Spec section 6.5 rule 4: an absolute path would make two machines'
  // artifacts differ for no semantic reason, so the artifact carries none.
  EXPECT_EQ(json.find(output.string()), std::string::npos)
      << json.substr(0, 500);
  EXPECT_EQ(json.find(project.string()), std::string::npos)
      << json.substr(0, 500);
}

}  // namespace
}  // namespace veritas::build
