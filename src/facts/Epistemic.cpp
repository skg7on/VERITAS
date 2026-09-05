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

#include <algorithm>

namespace veritas::facts {

namespace {

namespace sem = analysis::semantic;

// Evidence-strength rank, weakest first. The meet takes the minimum rank.
int Rank(sem::EpistemicState state) {
  switch (state) {
    case sem::EpistemicState::kUnknown:
      return 0;
    case sem::EpistemicState::kInferred:
      return 1;
    case sem::EpistemicState::kAssumed:
      return 2;
    case sem::EpistemicState::kMay:
      return 3;
    case sem::EpistemicState::kMustNot:
      return 4;
    case sem::EpistemicState::kMust:
      return 5;
  }
  return 0;
}

}  // namespace

sem::EpistemicState JoinEpistemic(
    std::span<const sem::EpistemicState> inputs, RuleSoundness soundness) {
  if (inputs.empty()) {
    return sem::EpistemicState::kUnknown;
  }

  int weakest_rank = Rank(sem::EpistemicState::kMust);
  bool has_must = false;
  bool has_must_not = false;

  for (const sem::EpistemicState state : inputs) {
    if (state == sem::EpistemicState::kUnknown) {
      // Nothing is knowable once any input is unknown.
      return sem::EpistemicState::kUnknown;
    }
    weakest_rank = std::min(weakest_rank, Rank(state));
    has_must = has_must || state == sem::EpistemicState::kMust;
    has_must_not = has_must_not || state == sem::EpistemicState::kMustNot;
  }

  // Both definite truth and definite falsehood with no weaker state in between
  // is a contradiction; the fact cannot be grounded.
  if (has_must && has_must_not && weakest_rank >= Rank(sem::EpistemicState::kMustNot)) {
    return sem::EpistemicState::kUnknown;
  }

  sem::EpistemicState result = sem::EpistemicState::kMust;
  switch (weakest_rank) {
    case 1:
      result = sem::EpistemicState::kInferred;
      break;
    case 2:
      result = sem::EpistemicState::kAssumed;
      break;
    case 3:
      result = sem::EpistemicState::kMay;
      break;
    case 4:
      result = sem::EpistemicState::kMustNot;
      break;
    default:
      result = sem::EpistemicState::kMust;
      break;
  }

  // An over-approximating rule only establishes possibility, never a definite
  // truth or falsehood.
  if (soundness == RuleSoundness::kMayProducing &&
      (result == sem::EpistemicState::kMust ||
       result == sem::EpistemicState::kMustNot)) {
    result = sem::EpistemicState::kMay;
  }

  return result;
}

}  // namespace veritas::facts
