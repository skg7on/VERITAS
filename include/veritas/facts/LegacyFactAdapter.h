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

// LegacyFactAdapter.h — lossless projection of M8 facts and V1 summaries.
//
// Positive M8 tuples and every V1 summary semantic surface are projected into
// typed V2 semantic rows. Every platform epistemic state either round-trips to
// its V2 relation or becomes an explicit UnsupportedFeature row; no state is
// silently removed. V1 memory strings become stable kMemoryRef values with
// legacy-opaque object identity.

#ifndef VERITAS_FACTS_LEGACY_FACT_ADAPTER_H_
#define VERITAS_FACTS_LEGACY_FACT_ADAPTER_H_

#include <span>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/FactSchema.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::facts {

struct LegacyProjection {
  std::vector<SemanticRow> rows;
};

// Projects validated positive M8 fact tuples into V2 semantic rows. Rejects
// any tuple that fails M8 validation.
StatusOr<LegacyProjection> ProjectLegacyFacts(
    const AnalysisRunManifest& run, std::span<const FactTuple> facts);

// Projects the complete V1 summary semantic surface (calls, memory effects,
// flows, aliases, unknowns, and assumptions) into V2 semantic rows.
StatusOr<LegacyProjection> ProjectLegacySummaries(
    const AnalysisRunManifest& run,
    std::span<const summary::v1::FunctionSummary> summaries);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_LEGACY_FACT_ADAPTER_H_
