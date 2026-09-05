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

#include <array>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/FactStore.h"

#ifndef VERITAS_EXPLAIN_BINARY
#error "VERITAS_EXPLAIN_BINARY must be defined by the build system"
#endif

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

namespace fs = std::filesystem;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kReachableCall,
                     {FunctionId(from), FunctionId(to), EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to), DispatchKind::kDirect,
                      EpistemicState::kMay}};
}

WitnessEdge Edge(const SemanticRow& result, std::string_view rule,
                 const SemanticRow& input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

AnalysisRunManifest TestRun() {
  AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("cli-rev", 7)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("cli-bv", 6)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = "test-toolchain";
  return std::move(MakeAnalysisRun(descriptor)).value();
}

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

struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
};

CliResult RunVeritasExplain(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_EXPLAIN_BINARY);
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(ShellQuote(argument));
  }
  command.append(" 2>&1");

  CliResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }
  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result.stdout_text.append(buffer.data());
  }
  const int status = ::pclose(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

class VeritasExplainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    db_dir_ = fs::temp_directory_path() /
              ("veritas_explain_" + std::to_string(::getpid()) + "_" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(db_dir_);
  }

  void TearDown() override { fs::remove_all(db_dir_); }

  fs::path db_dir_;
};

// Publishes a single derivation (direct -> reachable) and returns the derived
// fact and its run so the test can point veritas-explain at it.
struct Fixture {
  AnalysisRunManifest run;
  core::StableId fact_id;
};

Fixture PublishFixture(const fs::path& db_dir) {
  const auto root = MakeFact(DirectCall("f", "g")).value();
  const auto derived = MakeFact(Reachable("f", "g")).value();

  AnalysisFactBatch batch;
  batch.run = TestRun();
  batch.batch_id = core::MakeStableId(
      core::IdKind::kFact, std::as_bytes(std::span("explain-batch", 13)));
  batch.rooted_input_fact_ids = {root.fact_id};
  batch.facts = {derived};
  batch.witnesses = {Edge(Reachable("f", "g"), "direct", DirectCall("f", "g"), 0)};

  auto store = FactStore::Open(db_dir);
  const bool ok = store.ok() && store->Publish(batch).ok();
  EXPECT_TRUE(ok);

  return Fixture{batch.run, derived.fact_id};
}

}  // namespace

TEST_F(VeritasExplainTest, PrintsExplanationSections) {
  const auto fixture = PublishFixture(db_dir_);

  const auto result = RunVeritasExplain(
      {"fact", core::ToString(fixture.fact_id), "--run",
       core::ToString(fixture.run.run_id), "--db", db_dir_.string()});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;

  for (std::string_view section :
       {"Fact:", "Epistemic:", "Producer:", "Rule:", "Inputs:",
        "Assumptions:", "Unknowns:", "Source anchors:", "Truncated:"}) {
    EXPECT_NE(result.stdout_text.find(section), std::string::npos)
        << "missing section " << section << " in:\n"
        << result.stdout_text;
  }
}

TEST_F(VeritasExplainTest, JsonModeEmitsGraphJson) {
  const auto fixture = PublishFixture(db_dir_);

  const auto result = RunVeritasExplain(
      {"fact", core::ToString(fixture.fact_id), "--run",
       core::ToString(fixture.run.run_id), "--db", db_dir_.string(), "--json"});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("\"factId\""), std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("\"nodes\""), std::string::npos)
      << result.stdout_text;
}
