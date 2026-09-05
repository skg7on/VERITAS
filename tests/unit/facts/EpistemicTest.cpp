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

#include "veritas/facts/Epistemic.h"

#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

EpistemicState Join(std::initializer_list<EpistemicState> inputs,
                    RuleSoundness soundness = RuleSoundness::kSound) {
  return JoinEpistemic(std::vector<EpistemicState>(inputs), soundness);
}

}  // namespace

TEST(EpistemicTest, EmptyInputsAreUnknown) {
  EXPECT_EQ(Join({}), EpistemicState::kUnknown);
}

TEST(EpistemicTest, InferredDoesNotBecomeMust) {
  EXPECT_EQ(Join({EpistemicState::kInferred, EpistemicState::kMust}),
            EpistemicState::kInferred);
}

TEST(EpistemicTest, AssumedDoesNotBecomeMust) {
  EXPECT_EQ(Join({EpistemicState::kAssumed, EpistemicState::kMust}),
            EpistemicState::kAssumed);
}

TEST(EpistemicTest, MayDoesNotBecomeMust) {
  EXPECT_EQ(Join({EpistemicState::kMay, EpistemicState::kMust}),
            EpistemicState::kMay);
}

TEST(EpistemicTest, UnknownDominates) {
  EXPECT_EQ(Join({EpistemicState::kUnknown, EpistemicState::kMust}),
            EpistemicState::kUnknown);
}

TEST(EpistemicTest, AllMustWithSoundRuleIsMust) {
  EXPECT_EQ(Join({EpistemicState::kMust, EpistemicState::kMust}),
            EpistemicState::kMust);
}

TEST(EpistemicTest, MayProducingRuleCapsMust) {
  EXPECT_EQ(Join({EpistemicState::kMust}, RuleSoundness::kMayProducing),
            EpistemicState::kMay);
}

TEST(EpistemicTest, MayProducingRuleCapsMustNot) {
  EXPECT_EQ(Join({EpistemicState::kMustNot}, RuleSoundness::kMayProducing),
            EpistemicState::kMay);
}

TEST(EpistemicTest, MustNotPropagatesUnderSoundRule) {
  EXPECT_EQ(Join({EpistemicState::kMustNot, EpistemicState::kMustNot}),
            EpistemicState::kMustNot);
}

TEST(EpistemicTest, MustAndMustNotContradict) {
  EXPECT_EQ(Join({EpistemicState::kMust, EpistemicState::kMustNot}),
            EpistemicState::kUnknown);
}

TEST(EpistemicTest, InferredIsWeakerThanAssumed) {
  // Meet of an inferred and an assumed input is the weaker inferred state.
  EXPECT_EQ(Join({EpistemicState::kInferred, EpistemicState::kAssumed}),
            EpistemicState::kInferred);
}

TEST(EpistemicTest, AssumedIsWeakerThanMay) {
  EXPECT_EQ(Join({EpistemicState::kAssumed, EpistemicState::kMay}),
            EpistemicState::kAssumed);
}
