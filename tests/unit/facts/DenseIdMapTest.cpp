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

#include "veritas/facts/DenseIdMap.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::facts;

namespace {

core::StableId Stable(core::IdKind kind, std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(kind, bytes);
}

}  // namespace

TEST(DenseIdMapTest, SortsStableIdsBeforeAssigningDenseIds) {
  auto high = Stable(core::IdKind::kFunctionVariant, 0xf0);
  auto low = Stable(core::IdKind::kFunctionVariant, 0x10);
  auto map = FunctionDenseMap::Build({high, low});
  ASSERT_TRUE(map.ok());
  EXPECT_LT(map->ToDense(low)->value, map->ToDense(high)->value);
  EXPECT_EQ(*map->ToStable(*map->ToDense(high)), high);
}

TEST(DenseIdMapTest, RejectsStableIdsFromAnotherDomain) {
  auto memory = Stable(core::IdKind::kMemoryRef, 0x10);
  EXPECT_FALSE(FunctionDenseMap::Build({memory}).ok());
}
