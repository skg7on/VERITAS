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

#include "veritas/analysis/semantic/SemanticTypes.h"

#include <gtest/gtest.h>

using namespace veritas::analysis::semantic;

TEST(SemanticTypesTest, NoAliasIsNotUnknownAlias) {
  AliasObservation proven_no_alias{AliasKind::kNoAlias,
                                   EpistemicState::kMust};
  AliasObservation unknown{AliasKind::kUnknownAlias,
                           EpistemicState::kUnknown};
  EXPECT_NE(proven_no_alias, unknown);
}

TEST(SemanticTypesTest, UnknownByteRangeIsExplicit) {
  ByteRange range = ByteRange::Unknown();
  EXPECT_FALSE(range.offset.has_value());
  EXPECT_FALSE(range.size.has_value());
  EXPECT_TRUE(Validate(range).ok());
}

TEST(SemanticTypesTest, UnknownRangeDiffersFromKnownZeroRange) {
  EXPECT_NE(ByteRange::Unknown(), ByteRange::Known(0, 0));
  EXPECT_EQ(RelationRangeKind(ByteRange::Unknown()), ByteRangeKind::kUnknown);
  EXPECT_EQ(RelationRangeKind(ByteRange::Known(0, 0)), ByteRangeKind::kKnown);
}
