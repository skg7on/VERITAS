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

// ResultCanonicalizer.h — turns a raw evaluation into publishable facts.
//
// An engine's asserted results are hypotheses until each one is shown to have
// a finite proof rooted in declared inputs. The canonicalizer validates the
// witness graph, rejects anything it cannot ground, and selects exactly one
// canonical proof per result so two engines evaluating the same logical input
// publish byte-identical evidence.
//
// This operates on roots and raw evaluation only -- deliberately not on the
// whole logical component input -- so it stays in the facts layer, below wpa.

#ifndef VERITAS_FACTS_RESULT_CANONICALIZER_H_
#define VERITAS_FACTS_RESULT_CANONICALIZER_H_

#include <span>
#include <string>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {

struct CanonicalizationRequest {
  std::span<const RootedInputFact> local_roots;
  std::span<const RootedInputFact> successor_roots;
  const RawWpaEvaluation* evaluation = nullptr;
};

struct CanonicalizedResult {
  // Canonically ordered published facts. Their IDs come from MakeFact and are
  // witness-independent, so a proof change never re-identifies a fact.
  std::vector<AnalysisFact> facts;
  // The selected proof forest: one derivation per published fact.
  std::vector<WitnessEdge> witnesses;
  // Covers the canonical facts and the selected witnesses.
  std::string fixpoint_hash;
  // Covers only the published semantics. A witness-only change moves
  // fixpoint_hash but leaves this stable, so it does not schedule
  // predecessors.
  std::string external_hash;
  std::vector<std::string> diagnostics;
};

struct CanonicalResultHashes {
  std::string fixpoint_hash;
  std::string external_hash;
};

// Computes the canonical content hashes used by CanonicalizedResult. The
// inputs are sorted internally so callers cannot accidentally hash a
// producer-dependent iteration order.
CanonicalResultHashes ComputeCanonicalResultHashes(
    std::span<const AnalysisFact> facts,
    std::span<const WitnessEdge> witnesses);

class ResultCanonicalizer {
 public:
  // Fails with InvalidArgument on a malformed row, an unregistered rule, a
  // witness for an unpublished result, or one rule claiming two inputs at one
  // ordinal; and with FailedPrecondition when a result has no finite proof
  // rooted in the declared inputs, which covers both orphaned results and
  // results supported only by a cycle.
  static StatusOr<CanonicalizedResult> Canonicalize(
      const CanonicalizationRequest& request);
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_RESULT_CANONICALIZER_H_
