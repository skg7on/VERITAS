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

#include "veritas/facts/FactSchema.h"

#include <algorithm>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

namespace veritas::facts {
namespace {

namespace v1 = summary::v1;

core::StableId Id(core::IdKind kind, std::string_view literal) {
  return core::MakeStableId(
      kind, std::as_bytes(std::span(literal.data(), literal.size())));
}

TEST(FactSchemaTest, MustAndMayWeakenToMay) {
  auto joined = WeakenPositiveEpistemic(v1::EPISTEMIC_STATE_MUST,
                                        v1::EPISTEMIC_STATE_MAY);
  ASSERT_TRUE(joined.ok());
  EXPECT_EQ(*joined, v1::EPISTEMIC_STATE_MAY);
}

TEST(FactSchemaTest, DerivedTupleCarriesCanonicalImmediateInputs) {
  const auto summary_id = Id(core::IdKind::kFunctionSummary, "summary");
  const BaseFactOrigin origin{summary_id, "callsite:1", "prov:1"};
  auto call = MakeBaseFact(FactRelation::kDirectCall, {"A", "B"},
                           v1::EPISTEMIC_STATE_MUST, origin);
  auto write = MakeBaseFact(FactRelation::kDirectWrite, {"B", "X"},
                            v1::EPISTEMIC_STATE_MUST, origin);
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(write.ok());

  auto derived = MakeDerivedFact(
      FactRelation::kMayWrite, {"A", "X"}, v1::EPISTEMIC_STATE_MUST,
      "m8.may_write.transitive.v1", {write->tuple_id, call->tuple_id});
  ASSERT_TRUE(derived.ok()) << derived.status().message();
  EXPECT_EQ(derived->tuple_id.kind, core::IdKind::kFact);
  EXPECT_EQ(derived->rule_id, "m8.may_write.transitive.v1");
  EXPECT_TRUE(std::is_sorted(derived->input_tuple_ids.begin(),
                             derived->input_tuple_ids.end()));
}

TEST(FactSchemaTest, RelationNamesAndAritiesAreStable) {
  auto direct_call_name = FactRelationName(FactRelation::kDirectCall);
  auto local_flow_arity = FactRelationArity(FactRelation::kLocalFlow);
  auto global_flow_name = FactRelationName(FactRelation::kGlobalFlow);
  ASSERT_TRUE(direct_call_name.ok());
  ASSERT_TRUE(local_flow_arity.ok());
  ASSERT_TRUE(global_flow_name.ok());
  EXPECT_EQ(*direct_call_name, "DirectCall");
  EXPECT_EQ(*local_flow_arity, 3u);
  EXPECT_EQ(*global_flow_name, "GlobalFlow");
  EXPECT_EQ(*ParseFactRelation("MayWrite"), FactRelation::kMayWrite);
  EXPECT_FALSE(ParseFactRelation("may_write").ok());
}

TEST(FactSchemaTest, RejectsWrongArityAndNonPositiveEpistemic) {
  const BaseFactOrigin origin{Id(core::IdKind::kFunctionSummary, "summary"),
                              "site", "prov"};
  EXPECT_FALSE(MakeBaseFact(FactRelation::kDirectCall, {"A"},
                            v1::EPISTEMIC_STATE_MUST, origin)
                   .ok());
  EXPECT_FALSE(WeakenPositiveEpistemic(v1::EPISTEMIC_STATE_UNKNOWN,
                                       v1::EPISTEMIC_STATE_MUST)
                   .ok());
}

TEST(FactSchemaTest, DerivedTupleIdIgnoresInputInsertionOrder) {
  const auto input_a = Id(core::IdKind::kFact, "a");
  const auto input_b = Id(core::IdKind::kFact, "b");
  auto forward = MakeDerivedFact(
      FactRelation::kMayWrite, {"A", "X"}, v1::EPISTEMIC_STATE_MAY,
      "m8.may_write.transitive.v1", {input_a, input_b});
  auto reverse = MakeDerivedFact(
      FactRelation::kMayWrite, {"A", "X"}, v1::EPISTEMIC_STATE_MAY,
      "m8.may_write.transitive.v1", {input_b, input_a});
  ASSERT_TRUE(forward.ok());
  ASSERT_TRUE(reverse.ok());
  EXPECT_EQ(forward->tuple_id, reverse->tuple_id);
  EXPECT_EQ(forward->input_tuple_ids, reverse->input_tuple_ids);
}

TEST(FactSchemaTest, RejectsControlCharactersAndWrongProvenanceShape) {
  const BaseFactOrigin origin{Id(core::IdKind::kFunctionSummary, "summary"),
                              "site", "prov"};
  EXPECT_FALSE(MakeBaseFact(FactRelation::kDirectWrite, {"A", "X\nY"},
                            v1::EPISTEMIC_STATE_MUST, origin)
                   .ok());
  EXPECT_FALSE(MakeDerivedFact(FactRelation::kMayWrite, {"A", "X"},
                               v1::EPISTEMIC_STATE_MAY, "", {})
                   .ok());
}

TEST(FactSchemaTest, RejectsNonHexTupleOriginAndInputIds) {
  FactTuple invalid_tuple{
      .tuple_id = core::StableId{core::IdKind::kFact, std::string(64, 'g')},
      .relation = FactRelation::kDirectCall,
      .columns = {"A", "B"},
      .epistemic = v1::EPISTEMIC_STATE_MUST,
      .rule_id = {},
      .input_tuple_ids = {}};
  EXPECT_FALSE(ValidateFactTuple(invalid_tuple).ok());

  const BaseFactOrigin invalid_origin{
      core::StableId{core::IdKind::kFunctionSummary, std::string(64, 'g')},
      "site", "prov"};
  EXPECT_FALSE(MakeBaseFact(FactRelation::kDirectCall, {"A", "B"},
                            v1::EPISTEMIC_STATE_MUST, invalid_origin)
                   .ok());

  const auto invalid_input =
      core::StableId{core::IdKind::kFact, std::string(64, 'g')};
  EXPECT_FALSE(MakeDerivedFact(FactRelation::kMayWrite, {"A", "X"},
                               v1::EPISTEMIC_STATE_MUST,
                               "m8.may_write.transitive.v1", {invalid_input})
                   .ok());
}

TEST(FactSchemaTest, RejectsUppercaseDigestAliases) {
  FactTuple invalid_tuple{
      .tuple_id = core::StableId{core::IdKind::kFact, std::string(64, 'A')},
      .relation = FactRelation::kDirectCall,
      .columns = {"A", "B"},
      .epistemic = v1::EPISTEMIC_STATE_MUST,
      .rule_id = {},
      .input_tuple_ids = {}};
  EXPECT_FALSE(ValidateFactTuple(invalid_tuple).ok());

  const BaseFactOrigin invalid_origin{
      core::StableId{core::IdKind::kFunctionSummary, std::string(64, 'A')},
      "site", "prov"};
  EXPECT_FALSE(MakeBaseFact(FactRelation::kDirectCall, {"A", "B"},
                            v1::EPISTEMIC_STATE_MUST, invalid_origin)
                   .ok());

  const auto invalid_input =
      core::StableId{core::IdKind::kFact, std::string(64, 'A')};
  EXPECT_FALSE(MakeDerivedFact(FactRelation::kMayWrite, {"A", "X"},
                               v1::EPISTEMIC_STATE_MUST,
                               "m8.may_write.transitive.v1", {invalid_input})
                   .ok());
}

} // namespace
} // namespace veritas::facts
