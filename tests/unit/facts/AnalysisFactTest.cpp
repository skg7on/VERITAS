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

#include "veritas/facts/AnalysisFact.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

core::StableId FunctionStableId(std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(core::IdKind::kFunctionVariant, bytes);
}

SemanticRow ReachableCallSemanticRow(core::StableId source,
                                     core::StableId target,
                                     EpistemicState epistemic) {
  SemanticRow row;
  row.relation = RelationId::kReachableCall;
  row.cells = {source, target, epistemic};
  return row;
}

}  // namespace

TEST(AnalysisFactTest, WitnessDoesNotChangeFactIdentity) {
  SemanticRow row = ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay);
  auto first = MakeFact(row);
  auto second = MakeFact(row);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first->fact_id, second->fact_id);
}

TEST(AnalysisFactTest, DifferentCellsProduceDifferentFactId) {
  auto a = MakeFact(ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay));
  auto b = MakeFact(ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(3),
                                             EpistemicState::kMay));
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a->fact_id, b->fact_id);
}

TEST(AnalysisFactTest, RejectsInvalidSemanticRow) {
  SemanticRow row = ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay);
  row.cells[0] = core::MakeStableId(
      core::IdKind::kMemoryRef, std::array<std::byte, 1>{std::byte{0x01}});
  EXPECT_EQ(MakeFact(row).status().code(), StatusCode::kInvalidArgument);
}
