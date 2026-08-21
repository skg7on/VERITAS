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

#include "veritas/facts/SummaryFactBuilder.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::facts {
namespace {

namespace v1 = summary::v1;

core::StableId MakeId(core::IdKind kind, std::string_view seed) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(seed.data(), seed.size())));
}

v1::FunctionSummary CompleteSummary() {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  const std::string function =
      core::ToString(MakeId(core::IdKind::kFunctionVariant, "caller"));
  const std::string callee =
      core::ToString(MakeId(core::IdKind::kFunctionVariant, "callee"));
  summary.mutable_identity()->set_function_variant_id(function);

  auto *call = summary.add_calls();
  call->set_callee_symbol("callee");
  call->set_resolved_callee_function_variant_id(callee);
  call->set_call_site_anchor_id("site:call");
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("prov:call");

  auto *read = summary.add_memory_effects();
  read->set_kind(v1::EFFECT_KIND_READ);
  read->set_location("memory:read");
  read->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  read->set_provenance_ref("prov:read");
  auto *write = summary.add_memory_effects();
  write->set_kind(v1::EFFECT_KIND_WRITE);
  write->set_location("memory:write");
  write->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  write->set_provenance_ref("prov:write");

  auto *flow = summary.add_value_flows();
  flow->set_source("value:source");
  flow->set_sink("value:sink");
  flow->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  flow->set_provenance_ref("prov:flow");

  auto *alias = summary.add_alias_facts();
  alias->set_location_a("memory:left");
  alias->set_location_b("memory:right");
  alias->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  alias->set_provenance_ref("prov:alias");
  return summary;
}

v1::FunctionSummary EmptySummaryFor(std::string function_id) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  summary.mutable_identity()->set_function_variant_id(std::move(function_id));
  return summary;
}

std::vector<v1::FunctionSummary> CompleteLoadedSummaries() {
  auto caller = CompleteSummary();
  auto callee =
      EmptySummaryFor(caller.calls(0).resolved_callee_function_variant_id());
  return {std::move(caller), std::move(callee)};
}

TEST(SummaryFactBuilderTest, BuildsEveryBaseRelationWithStableProvenance) {
  const auto summaries = CompleteLoadedSummaries();
  auto facts = BuildBaseFacts(summaries);
  ASSERT_TRUE(facts.ok()) << facts.status().message();
  ASSERT_EQ(facts->size(), 5u);

  std::vector<FactRelation> relations;
  for (const auto &fact : *facts) {
    EXPECT_TRUE(ValidateFactTuple(fact).ok());
    EXPECT_TRUE(fact.rule_id.empty());
    EXPECT_TRUE(fact.input_tuple_ids.empty());
    relations.push_back(fact.relation);
  }
  std::ranges::sort(relations);
  EXPECT_EQ(relations,
            (std::vector{FactRelation::kDirectCall, FactRelation::kDirectRead,
                         FactRelation::kDirectWrite, FactRelation::kLocalFlow,
                         FactRelation::kMayAlias}));
}

TEST(SummaryFactBuilderTest, IsIndependentOfSummaryInsertionOrder) {
  auto summaries = CompleteLoadedSummaries();
  auto first = summaries[0];
  auto second = summaries[1];
  std::vector forward{first, second};
  std::vector reverse{second, first};

  auto left = BuildBaseFacts(forward);
  auto right = BuildBaseFacts(reverse);
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());
  ASSERT_EQ(left->size(), right->size());
  for (std::size_t i = 0; i < left->size(); ++i) {
    EXPECT_EQ((*left)[i].tuple_id, (*right)[i].tuple_id);
  }
}

TEST(SummaryFactBuilderTest, OmitsDirectCallToUnavailableFunctionVariant) {
  const std::vector summaries{CompleteSummary()};

  auto facts = BuildBaseFacts(summaries);

  ASSERT_TRUE(facts.ok()) << facts.status().message();
  EXPECT_EQ(std::ranges::count(*facts, FactRelation::kDirectCall,
                               &FactTuple::relation),
            0);
  EXPECT_EQ(facts->size(), 4u);
}

TEST(SummaryFactBuilderTest, RejectsMalformedResolvedCalleeIdentity) {
  auto caller = CompleteSummary();
  caller.mutable_calls(0)->set_resolved_callee_function_variant_id(
      "not-a-stable-id");
  const std::vector summaries{std::move(caller)};

  auto facts = BuildBaseFacts(summaries);

  ASSERT_FALSE(facts.ok());
  EXPECT_EQ(facts.status().code(), StatusCode::kInvalidArgument);
}

TEST(SummaryFactBuilderTest, RejectsNonHexResolvedCalleeDigest) {
  auto caller = CompleteSummary();
  caller.mutable_calls(0)->set_resolved_callee_function_variant_id(
      "funcvar:sha256:" + std::string(64, 'g'));
  const std::vector summaries{std::move(caller)};

  auto facts = BuildBaseFacts(summaries);

  ASSERT_FALSE(facts.ok());
  EXPECT_EQ(facts.status().code(), StatusCode::kInvalidArgument);
}

TEST(SummaryFactBuilderTest, RejectsWrongKindResolvedCalleeIdentity) {
  auto caller = CompleteSummary();
  caller.mutable_calls(0)->set_resolved_callee_function_variant_id(
      core::ToString(MakeId(core::IdKind::kRevision, "callee")));
  const std::vector summaries{std::move(caller)};

  auto facts = BuildBaseFacts(summaries);

  ASSERT_FALSE(facts.ok());
  EXPECT_EQ(facts.status().code(), StatusCode::kInvalidArgument);
}

} // namespace
} // namespace veritas::facts
