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

// CppRuleEvaluator.h — the C++ conformance evaluator.
//
// Implements exactly the joins and epistemic weakening of the Datalog bundles
// in logic/reachability and logic/memory_effects, over the same engine-neutral
// logical component input. It is not a fallback: production recursive WPA is
// compiled Souffle, and this exists so the two can be compared on byte-
// identical input, or run under an explicitly selected emergency mode.
//
// It emits semantic rows and generic witness candidates only. It does not
// derive stable fact IDs and does not decide what is publishable -- grounding
// results in rooted inputs and selecting a canonical proof is the
// canonicalizer's job.

#ifndef VERITAS_WPA_CPP_RULE_EVALUATOR_H_
#define VERITAS_WPA_CPP_RULE_EVALUATOR_H_

#include "veritas/core/Status.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {

class CppRuleEvaluator {
 public:
  // Fails with InvalidArgument when a dense cell has no mapping, a row does
  // not match its relation schema, or the input carries a relation this
  // component does not evaluate.
  StatusOr<facts::RawWpaEvaluation> Evaluate(
      const WpaLogicalComponentInput& input) const;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_CPP_RULE_EVALUATOR_H_
