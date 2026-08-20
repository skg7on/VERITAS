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
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/summary/FunctionSummary.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/DependencyIndex.h"
#include "veritas/summarydb/SummaryRepository.h"

#ifndef VERITAS_DIFF_BINARY
#error "VERITAS_DIFF_BINARY must be defined by the build system"
#endif

namespace veritas::summarydb {
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

CliResult RunVeritasDiff(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_DIFF_BINARY);
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

// A synthetic summary whose only semantic content is a single range fact with
// the given max value. Varying the value yields a new summary ID while the
// identity stays pinned to the same function variant.
summary::v1::FunctionSummary MakeRangeSummary(int64_t max_value) {
  summary::v1::FunctionSummary summary;

  auto* header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(0);

  auto* identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id("rev:sha256:def");
  identity->set_build_variant_id("bv:sha256:ghi");
  identity->set_function_variant_id("funcvar:sha256:jkl");
  identity->set_function_body_id("funcbody:sha256:mno");

  auto* range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(0);
  range->set_max_value(max_value);
  range->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);

  return summary;
}

// A valid 64-hex-char consumer summary ID unique per character.
core::StableId Consumer(char c) {
  return core::StableId{core::IdKind::kFunctionSummary, std::string(64, c)};
}

DependencyEdge RangeEdge(char consumer_char, core::StableId producer_id) {
  DependencyEdge edge;
  edge.consumer_id = Consumer(consumer_char);
  edge.consumer_component = summary::v1::COMPONENT_KIND_VALUE_FLOW;
  edge.producer_id = producer_id;
  edge.producer_component = summary::v1::COMPONENT_KIND_RANGE_FACTS;
  edge.dependency_kind = DependencyKind::kRange;
  edge.sensitivity = Sensitivity::kSemantic;
  return edge;
}

class VeritasDiffTest : public ::testing::Test {
 protected:
  void SetUp() override {
    db_dir_ = fs::temp_directory_path() /
              ("veritas_diff_" + std::to_string(::getpid()) + "_" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(db_dir_);
  }

  void TearDown() override { fs::remove_all(db_dir_); }

  fs::path db_dir_;
};

TEST_F(VeritasDiffTest, ReportsChangedComponentsAndConsumers) {
  const auto old_summary = MakeRangeSummary(10);
  const auto new_summary = MakeRangeSummary(20);

  auto old_id = summary::ComputeFunctionSummaryId(old_summary);
  auto new_id = summary::ComputeFunctionSummaryId(new_summary);
  ASSERT_TRUE(old_id.ok()) << old_id.status().message();
  ASSERT_TRUE(new_id.ok()) << new_id.status().message();

  {
    auto repo = SummaryRepository::Open(db_dir_.string());
    ASSERT_TRUE(repo.ok()) << repo.status().message();
    auto put = (*repo)->PutImmutableSummaries({old_summary, new_summary});
    ASSERT_TRUE(put.ok()) << put.status().message();

    DependencyIndex index((*repo)->metadata_store());
    ASSERT_TRUE(index
                    .ReplaceCurrentDependencies(Consumer('c'),
                                                {RangeEdge('c', *old_id)})
                    .ok());
  }

  const auto result =
      RunVeritasDiff({"--db", db_dir_.string(), "--old",
                      core::ToString(*old_id), "--new", core::ToString(*new_id)});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Semantic changed: yes"), std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("RANGE_FACTS"), std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find(core::ToString(Consumer('c'))),
            std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Truncated: no"), std::string::npos)
      << result.stdout_text;
}

TEST_F(VeritasDiffTest, ReportsTruncationWhenBudgetExceeded) {
  const auto old_summary = MakeRangeSummary(10);
  const auto new_summary = MakeRangeSummary(20);

  auto old_id = summary::ComputeFunctionSummaryId(old_summary);
  auto new_id = summary::ComputeFunctionSummaryId(new_summary);
  ASSERT_TRUE(old_id.ok());
  ASSERT_TRUE(new_id.ok());

  {
    auto repo = SummaryRepository::Open(db_dir_.string());
    ASSERT_TRUE(repo.ok());
    auto put = (*repo)->PutImmutableSummaries({old_summary, new_summary});
    ASSERT_TRUE(put.ok());

    DependencyIndex index((*repo)->metadata_store());
    ASSERT_TRUE(index
                    .ReplaceCurrentDependencies(Consumer('c'),
                                                {RangeEdge('c', *old_id)})
                    .ok());
    ASSERT_TRUE(index
                    .ReplaceCurrentDependencies(Consumer('d'),
                                                {RangeEdge('d', *old_id)})
                    .ok());
  }

  const auto result = RunVeritasDiff(
      {"--db", db_dir_.string(), "--old", core::ToString(*old_id), "--new",
       core::ToString(*new_id), "--max-consumers", "1"});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Impacted consumers: 1"),
            std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("Truncated: yes"), std::string::npos)
      << result.stdout_text;
}

}  // namespace
}  // namespace veritas::summarydb
