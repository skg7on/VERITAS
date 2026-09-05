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

// Epistemic.h — conservative epistemic propagation across a derivation.
//
// A derived fact's epistemic state is the weakest state among the rule's
// inputs, capped by how strongly the rule itself can assert. This keeps LLM
// and heuristic outputs from ever turning into MUST: a MAY, INFERRED, ASSUMED,
// or UNKNOWN input can never produce a MUST (or MUST_NOT) output, and an
// over-approximating rule can only ever yield MAY or weaker.
//
// The join is a meet over a total order of "evidence strength", weakest first:
//
//     UNKNOWN < INFERRED < ASSUMED < MAY < MUST_NOT / MUST
//
// MUST and MUST_NOT are the two definite truth values; when both are present
// with no weaker state in between, the inputs contradict and the join reports
// UNKNOWN. Confidence is intentionally absent here: it is stored alongside a
// fact's binding, never folded into its epistemic state.

#ifndef VERITAS_FACTS_EPISTEMIC_H_
#define VERITAS_FACTS_EPISTEMIC_H_

#include <cstdint>
#include <span>

#include "veritas/analysis/semantic/SemanticTypes.h"

namespace veritas::facts {

// How much a rule can assert about its output.
enum class RuleSoundness : std::uint8_t {
  // An exact/sound rule preserves the joined epistemic state of its inputs.
  kSound,
  // An over-approximating rule only establishes possibility: its output is at
  // most MAY, regardless of how strong its inputs are.
  kMayProducing,
};

// Joins the epistemic states of a rule's inputs into the output state. An
// empty input set yields UNKNOWN (nothing was established).
analysis::semantic::EpistemicState JoinEpistemic(
    std::span<const analysis::semantic::EpistemicState> inputs,
    RuleSoundness soundness);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_EPISTEMIC_H_
