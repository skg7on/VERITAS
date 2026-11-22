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

// ProofSelector.h — internal: canonical finite rooted proof selection.
//
// The canonicalizer already selects exactly one derivation per published fact
// inside each component. Across a query's admitted SCCs, this selector takes
// the transitive closure of those selected derivations for the queried
// results, stops at declared roots, and never drops a premise to meet a
// budget: if a result's complete proof does not fit, the result is omitted and
// the selection reports truncation.

#ifndef VERITAS_QUERY_PROOF_SELECTOR_H_
#define VERITAS_QUERY_PROOF_SELECTOR_H_

#include <cstdint>
#include <span>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/Witness.h"

namespace veritas::query::internal {

struct SelectedProof {
  // The candidates whose complete proofs fit the budget, in input order.
  std::vector<facts::AnalysisFact> results;
  // Every non-result fact a selected edge names, including roots.
  std::vector<facts::AnalysisFact> premises;
  std::vector<facts::WitnessEdge> edges;
  // The proof budget could not cover the next candidate's complete proof. The
  // candidate is omitted; no selected result has a truncated proof.
  bool truncated = false;
  // A candidate had no derivation rooted in a declared input. The candidate is
  // omitted and the caller must record an open frontier; it can never be
  // sealed as a complete result.
  bool missing_premise = false;
};

// `candidates` must be canonically ordered. A candidate with no rooted
// derivation is omitted and reported through `missing_premise`, never with a
// fabricated or partial proof.
StatusOr<SelectedProof>
SelectProofs(std::span<const facts::AnalysisFact> candidates,
             std::span<const facts::WitnessEdge> witnesses,
             std::span<const facts::RootedInputFact> roots,
             std::uint64_t max_edges);

} // namespace veritas::query::internal

#endif // VERITAS_QUERY_PROOF_SELECTOR_H_
