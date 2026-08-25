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

// Witness.h — the generic, relation-independent witness vocabulary.
//
// Engines emit immediate witness edges: "this result follows from this input,
// by this rule, at this argument position". Nothing here knows what a
// ReachableCall or a MayWrite is, which is what lets one canonicalizer replace
// per-relation C++ reconstruction of Datalog joins.

#ifndef VERITAS_FACTS_WITNESS_H_
#define VERITAS_FACTS_WITNESS_H_

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "veritas/facts/AnalysisFact.h"

namespace veritas::facts {

// A semantic row used as a graph node. Comparison is structural so keys can be
// ordered and de-duplicated without hashing first.
struct SemanticKey {
  SemanticRow row;

  auto operator<=>(const SemanticKey&) const = default;
  bool operator==(const SemanticKey&) const = default;
};

// One immediate derivation step. `input_ordinal` is the rule's argument
// position, so a rule that joins two inputs emits two edges that cannot be
// confused with two alternative single-input proofs.
struct WitnessEdge {
  SemanticKey result;
  std::string rule_id;
  SemanticKey input;
  std::uint32_t input_ordinal = 0;

  auto operator<=>(const WitnessEdge&) const = default;
  bool operator==(const WitnessEdge&) const = default;
};

// An input fact together with where it came from. Roots terminate every
// witness chain, so a published result is always traceable to a declared
// input rather than to the evaluator's assertion.
struct RootedInputFact {
  AnalysisFact fact;
  std::string provenance_ref;
};

// What an engine returns before validation: asserted results, the witness
// edges backing them, and any diagnostics. None of it is trusted yet.
struct RawWpaEvaluation {
  std::vector<SemanticRow> results;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

// Canonical, injective encoding of a semantic row.
//
// Every field is length-prefixed and type-tagged, so no choice of cell
// contents can forge a field boundary: {"a","bc"} and {"ab","c"} encode
// differently, and a cell containing the encoding's own delimiters is inert.
// The relation name and every cell participate, so two relations sharing a
// column shape never collide.
std::string EncodeSemanticKey(const SemanticRow& row);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_WITNESS_H_
