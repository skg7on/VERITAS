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

#include "veritas/facts/SouffleRunner.h"
#include "veritas/facts/SouffleExporter.h"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/summary/FunctionSummary.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/FixpointEngine.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::facts {
namespace {

namespace v1 = summary::v1;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

v1::FunctionSummary Function(std::string_view name) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddCall(v1::FunctionSummary *caller, std::string_view callee) {
  auto *call = caller->add_calls();
  call->set_callee_symbol(std::string(callee));
  call->set_resolved_callee_function_variant_id(
      core::ToString(FunctionId(callee)));
  call->set_call_site_anchor_id("site:" + std::string(callee));
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

void AddWrite(v1::FunctionSummary *function, std::string memory) {
  auto *effect = function->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_location(std::move(memory));
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
}

StatusOr<std::vector<FactTuple>>
MakeBaseFacts(std::span<const v1::FunctionSummary> summaries) {
  std::vector<FactTuple> facts;
  for (const auto &function_summary : summaries) {
    auto summary_id = summary::ComputeFunctionSummaryId(function_summary);
    if (!summary_id.ok())
      return summary_id.status();
    const std::string caller =
        function_summary.identity().function_variant_id();
    for (const auto &call : function_summary.calls()) {
      auto fact = MakeBaseFact(
          FactRelation::kDirectCall,
          {caller, call.resolved_callee_function_variant_id()},
          call.epistemic(),
          {*summary_id, call.call_site_anchor_id(), call.provenance_ref()});
      if (!fact.ok())
        return fact.status();
      facts.push_back(std::move(*fact));
    }
    for (const auto &effect : function_summary.memory_effects()) {
      auto fact = MakeBaseFact(
          FactRelation::kDirectWrite, {caller, effect.location()},
          effect.epistemic(),
          {*summary_id, effect.location(), effect.provenance_ref()});
      if (!fact.ok())
        return fact.status();
      facts.push_back(std::move(*fact));
    }
  }
  return facts;
}

struct SemanticFact {
  FactRelation relation;
  std::vector<std::string> columns;
  v1::EpistemicState epistemic;

  auto operator<=>(const SemanticFact &) const = default;
};

std::set<SemanticFact>
SemanticFacts(std::span<const wpa::SccResult> call_results,
              std::span<const wpa::SccResult> memory_results) {
  std::set<SemanticFact> facts;
  for (const auto &results : {call_results, memory_results}) {
    for (const auto &result : results) {
      for (const auto &fact : result.facts) {
        facts.insert({fact.relation, fact.columns, fact.epistemic});
      }
    }
  }
  return facts;
}

std::set<SemanticFact> SemanticFacts(std::span<const FactTuple> tuples) {
  std::set<SemanticFact> facts;
  for (const auto &tuple : tuples) {
    facts.insert({tuple.relation, tuple.columns, tuple.epistemic});
  }
  return facts;
}

TEST(SouffleRunnerTest, RejectsMissingExecutableBeforeCreatingOutput) {
  const auto test_dir =
      std::filesystem::temp_directory_path() /
      ("veritas_souffle_runner_validation_" +
       std::to_string(static_cast<unsigned long long>(getpid())));
  const auto input_dir = test_dir / "input";
  const auto output_dir = test_dir / "output";
  std::filesystem::remove_all(test_dir);
  ASSERT_TRUE(std::filesystem::create_directories(input_dir));

  auto status =
      SouffleRunner::Run(test_dir / "missing-souffle",
                         test_dir / "missing-rule.dl", input_dir, output_dir);

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("Souffle executable"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(output_dir));
  std::filesystem::remove_all(test_dir);
}

TEST(SouffleRunnerTest, MatchesCppFixpointSemantics) {
#if !VERITAS_HAS_SOUFFLE
  GTEST_SKIP() << "Souffle executable not available";
#endif
  auto a = Function("A");
  auto b = Function("B");
  auto c = Function("C");
  AddCall(&a, "B");
  AddCall(&b, "C");
  AddWrite(&c, "X");
  const std::vector summaries{a, b, c};
  auto call_graph = wpa::CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(call_graph.ok()) << call_graph.status().message();
  auto scc_graph = wpa::SccGraph::Build(*call_graph);
  ASSERT_TRUE(scc_graph.ok()) << scc_graph.status().message();
  wpa::FixpointEngine engine(*call_graph, *scc_graph, summaries);
  auto call_results =
      engine.ComputeAll(v1::COMPONENT_KIND_CALLS, {.max_iterations = 32});
  auto memory_results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                          {.max_iterations = 32});
  ASSERT_TRUE(call_results.ok()) << call_results.status().message();
  ASSERT_TRUE(memory_results.ok()) << memory_results.status().message();
  auto base_facts = MakeBaseFacts(summaries);
  ASSERT_TRUE(base_facts.ok()) << base_facts.status().message();

  const auto test_dir =
      std::filesystem::temp_directory_path() /
      ("veritas_souffle_runner_test_" +
       std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
       std::to_string(reinterpret_cast<std::uintptr_t>(&summaries)));
  const auto input_dir = test_dir / "input";
  const auto output_dir = test_dir / "output";
  std::filesystem::remove_all(test_dir);
  ASSERT_TRUE(std::filesystem::create_directories(input_dir));
  ASSERT_TRUE(SouffleExporter::WriteBaseRelations(input_dir, *base_facts).ok());
  ASSERT_TRUE(SouffleRunner::Run(VERITAS_SOUFFLE_EXECUTABLE,
                                 std::filesystem::path(VERITAS_FACT_RULE_DIR) /
                                     "reachability.dl",
                                 input_dir, output_dir)
                  .ok());
  ASSERT_TRUE(SouffleRunner::Run(VERITAS_SOUFFLE_EXECUTABLE,
                                 std::filesystem::path(VERITAS_FACT_RULE_DIR) /
                                     "memory_effects.dl",
                                 input_dir, output_dir)
                  .ok());
  auto imported =
      SouffleExporter::ReadDerivedRelations(output_dir, *base_facts);
  ASSERT_TRUE(imported.ok()) << imported.status().message();

  EXPECT_EQ(SemanticFacts(*imported),
            SemanticFacts(*call_results, *memory_results));
  std::filesystem::remove_all(test_dir);
}

} // namespace
} // namespace veritas::facts
