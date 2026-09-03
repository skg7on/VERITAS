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

#include "veritas/wpa/RelationIo.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/SemanticKeyCodec.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/CppRuleEvaluator.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;
namespace sem = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

class RelationIoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("veritas_relation_io_" +
                  std::to_string(
                      ::testing::UnitTest::GetInstance()->random_seed()) +
                  "_" +
                  ::testing::UnitTest::GetInstance()
                      ->current_test_info()
                      ->name());
    std::filesystem::remove_all(directory_);
    std::filesystem::create_directories(directory_);
  }
  void TearDown() override { std::filesystem::remove_all(directory_); }

  std::filesystem::path directory_;
};

v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v2::FunctionSummary* summary, std::string_view from,
             std::string_view to) {
  auto* call = summary->add_calls();
  call->set_call_site_id(
      core::ToString(CallSiteId(std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(core::ToString(FunctionId(to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

StatusOr<WpaLogicalComponentInput> BuildInput() {
  auto f = V2Summary("f");
  AddCall(&f, "f", "g");
  auto g = V2Summary("g");
  AddCall(&g, "g", "f");
  AddCall(&g, "g", "h");
  const std::vector<summary::SummaryArtifact> program = {f, g, V2Summary("h")};

  auto graph = CallGraph::FromSummaries(program);
  if (!graph.ok())
    return graph.status();
  auto scc = SccGraph::Build(*graph);
  if (!scc.ok())
    return scc.status();
  auto scc_id = scc->SccForFunction(FunctionId("f"));
  if (!scc_id.ok())
    return scc_id.status();

  facts::AnalysisRunSemanticDescriptor semantics;
  semantics.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  semantics.summary_schema_version = "summary.v2";
  semantics.relation_schema_version = "relations.v2";
  semantics.rule_bundle_version = "rules.v2";
  semantics.model_bundle_version = "models.v1";
  semantics.svf_configuration_hash = std::string(64, 'a');
  semantics.wpa_configuration_hash = std::string(64, 'b');

  WpaMaterializationRequest request;
  request.semantics = semantics;
  request.scc_id = *scc_id;
  request.component = WpaComponentKind::kReachability;
  request.summaries = program;
  // Materializer copies what it needs, but the program must outlive the call.
  auto input = WpaInputMaterializer::Build(request);
  return input;
}

TEST_F(RelationIoTest, WritesOneFilePerEdbRelation) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  ASSERT_TRUE(RelationIo::WriteInput(directory_, *input).ok());

  EXPECT_TRUE(std::filesystem::exists(directory_ / "DirectCall.facts"));
  EXPECT_TRUE(std::filesystem::exists(directory_ / "FunctionMap.facts"));
  // A relation with no rows still gets an empty facts file: the compiled
  // bundle's .input directive loads every input relation unconditionally, so
  // an absent file would fail the run.
  EXPECT_TRUE(std::filesystem::exists(directory_ / "DirectWrite.facts"));
}

// The evaluator's own output, written out as an engine would and read back,
// must survive the round trip unchanged. This is the mechanism the eventual
// Souffle comparison rests on.
TEST_F(RelationIoTest, RoundTripsResultsAndWitnesses) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  auto raw = CppRuleEvaluator().Evaluate(*input);
  ASSERT_TRUE(raw.ok());
  ASSERT_FALSE(raw->results.empty());
  ASSERT_FALSE(raw->witnesses.empty());

  // Emit the evaluation the way a generated program would.
  {
    std::ofstream results(directory_ / "ReachableCall.csv");
    for (const auto& row : raw->results) {
      auto source = input->mappings.functions.ToDense(
          std::get<core::StableId>(row.cells[0]));
      auto target = input->mappings.functions.ToDense(
          std::get<core::StableId>(row.cells[1]));
      ASSERT_TRUE(source.ok());
      ASSERT_TRUE(target.ok());
      results << source->value << '\t' << target->value << '\t'
              << static_cast<int>(
                     std::get<sem::EpistemicState>(row.cells[2]))
              << '\n';
    }
    std::ofstream witnesses(directory_ / "Witness.csv");
    for (const auto& edge : raw->witnesses) {
      witnesses << facts::EncodeSemanticKey(edge.result.row) << '\t'
                << edge.rule_id << '\t'
                << facts::EncodeSemanticKey(edge.input.row) << '\t'
                << edge.input_ordinal << '\n';
    }
  }

  auto read = RelationIo::ReadOutput(directory_, *input);
  ASSERT_TRUE(read.ok()) << read.status().message();
  EXPECT_EQ(read->results.size(), raw->results.size());
  EXPECT_EQ(read->witnesses, raw->witnesses);
}

TEST_F(RelationIoTest, RejectsMissingResultFile) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  std::ofstream(directory_ / "Witness.csv").close();

  auto read = RelationIo::ReadOutput(directory_, *input);

  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.status().code(), StatusCode::kInternal);
}

TEST_F(RelationIoTest, RejectsMissingWitnessFile) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  std::ofstream(directory_ / "ReachableCall.csv").close();

  auto read = RelationIo::ReadOutput(directory_, *input);

  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.status().code(), StatusCode::kInternal);
}

// A dense id the component never mapped cannot become a fact.
TEST_F(RelationIoTest, RejectsResultCellOutsideItsMapping) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  {
    std::ofstream results(directory_ / "ReachableCall.csv");
    results << "9999\t0\t0\n";
  }
  EXPECT_FALSE(RelationIo::ReadOutput(directory_, *input).ok());
}

// A witness key naming a relation outside the schema is unverifiable.
TEST_F(RelationIoTest, RejectsWitnessKeyForUnknownRelation) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  const std::string bogus = facts::EncodeKeyHeader("NotARelation", 1) +
                            facts::EncodeIdField("func:sha256:aa");
  {
    std::ofstream witnesses(directory_ / "Witness.csv");
    witnesses << bogus << "\twpa.reachability.direct.v2\t" << bogus << "\t0\n";
  }
  EXPECT_FALSE(RelationIo::ReadOutput(directory_, *input).ok());
}

// A key whose field tag disagrees with the column's domain -- here a symbol
// where the schema declares an identifier -- must be rejected, not coerced.
TEST_F(RelationIoTest, RejectsWitnessKeyWithWrongColumnDomain) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  const std::string wrong = facts::EncodeKeyHeader("ReachableCall", 3) +
                            facts::EncodeSymbolField("func:sha256:aa") +
                            facts::EncodeIdField("func:sha256:bb") +
                            facts::EncodeEnumField(0);
  {
    std::ofstream witnesses(directory_ / "Witness.csv");
    witnesses << wrong << "\twpa.reachability.direct.v2\t" << wrong << "\t0\n";
  }
  EXPECT_FALSE(RelationIo::ReadOutput(directory_, *input).ok());
}

// A symbol cell carrying a tab would shift every column to its right.
TEST_F(RelationIoTest, RejectsSymbolCellContainingDelimiter) {
  auto input = BuildInput();
  ASSERT_TRUE(input.ok());
  WpaLogicalComponentInput mutated = *input;
  mutated.edb.push_back(facts::ExecutionRow{
      facts::RelationId::kUnknownCall,
      {facts::CallSiteId{0}, facts::FunctionId{0}, std::string("a\tb"),
       sem::EpistemicState::kUnknown}});
  EXPECT_FALSE(RelationIo::WriteInput(directory_, mutated).ok());
}

}  // namespace
}  // namespace veritas::wpa
