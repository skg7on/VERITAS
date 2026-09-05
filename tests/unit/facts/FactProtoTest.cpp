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

#include "veritas/facts/FactProto.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

core::StableId Id(core::IdKind kind, std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(kind, bytes);
}

core::StableId Function(std::uint8_t seed) {
  return Id(core::IdKind::kFunctionVariant, seed);
}

core::StableId Memory(std::uint8_t seed) {
  return Id(core::IdKind::kMemoryRef, seed);
}

core::StableId CallSite(std::uint8_t seed) {
  return Id(core::IdKind::kCallSite, seed);
}

SemanticRow ReachableCallRow() {
  SemanticRow row;
  row.relation = RelationId::kReachableCall;
  row.cells = {Function(1), Function(2), EpistemicState::kMay};
  return row;
}

SemanticRow DirectCallRow() {
  SemanticRow row;
  row.relation = RelationId::kDirectCall;
  row.cells = {CallSite(3), Function(4), Function(5), DispatchKind::kDirect,
               EpistemicState::kMust};
  return row;
}

SemanticRow DirectReadRow() {
  SemanticRow row;
  row.relation = RelationId::kDirectRead;
  row.cells = {Function(6), Memory(7), ByteRangeKind::kKnown,
               std::int64_t{8}, std::uint64_t{16}, EpistemicState::kMust};
  return row;
}

SemanticRow AliasRow() {
  SemanticRow row;
  row.relation = RelationId::kAlias;
  row.cells = {Memory(9), Memory(10), AliasKind::kNoAlias, EpistemicState::kMust};
  return row;
}

}  // namespace

TEST(FactProtoTest, RoundTripsReachableCall) {
  auto fact = MakeFact(ReachableCallRow());
  ASSERT_TRUE(fact.ok());
  auto proto = ToProtoFact(*fact);
  ASSERT_TRUE(proto.ok());
  EXPECT_EQ(proto->relation_name(), "ReachableCall");
  auto back = FromProtoFact(*proto);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back->fact_id, fact->fact_id);
  EXPECT_EQ(back->row, fact->row);
}

TEST(FactProtoTest, RoundTripsEnumCells) {
  for (const auto& row : {DirectCallRow(), DirectReadRow(), AliasRow()}) {
    auto fact = MakeFact(row);
    ASSERT_TRUE(fact.ok());
    auto proto = ToProtoFact(*fact);
    ASSERT_TRUE(proto.ok());
    auto back = FromProtoFact(*proto);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back->fact_id, fact->fact_id);
    EXPECT_EQ(back->row, fact->row);
  }
}

TEST(FactProtoTest, RejectsMismatchedFactId) {
  auto fact = MakeFact(ReachableCallRow());
  ASSERT_TRUE(fact.ok());
  auto proto = ToProtoFact(*fact);
  ASSERT_TRUE(proto.ok());

  // Corrupt the fact_id on the wire; the round trip must reject it.
  auto other = MakeFact(AliasRow());
  ASSERT_TRUE(other.ok());
  proto->set_fact_id(core::ToString(other->fact_id));
  EXPECT_FALSE(FromProtoFact(*proto).ok());
}

TEST(FactProtoTest, RejectsUnknownRelationName) {
  auto fact = MakeFact(ReachableCallRow());
  ASSERT_TRUE(fact.ok());
  auto proto = ToProtoFact(*fact);
  ASSERT_TRUE(proto.ok());
  proto->set_relation_name("NotARealRelation");
  EXPECT_FALSE(FromProtoFact(*proto).ok());
}

TEST(FactProtoTest, RejectsMalformedFactId) {
  auto fact = MakeFact(ReachableCallRow());
  ASSERT_TRUE(fact.ok());
  auto proto = ToProtoFact(*fact);
  ASSERT_TRUE(proto.ok());
  proto->set_fact_id("not-an-id");
  EXPECT_FALSE(FromProtoFact(*proto).ok());
}
