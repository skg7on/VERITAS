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

#ifndef VERITAS_SUMMARY_SUMMARY_V2_BUILDER_H_
#define VERITAS_SUMMARY_SUMMARY_V2_BUILDER_H_

#include <string>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::summary {

// The durable semantic value types live under veritas::analysis::semantic;
// expose a short alias for the builder-facing structs below.
namespace semantic = veritas::analysis::semantic;

// V2-refined per-function local facts. The components V2 refines (calls,
// memory effects, value flows, and alias facts) carry typed C++ semantic
// structs instead of proto messages; every component V2 does not refine reuses
// the immutable v1:: message type.
struct CallFactV2 {
  core::StableId call_site_id;                      // must be kCallSite
  std::string callee_symbol;
  std::string resolved_callee_function_variant_id;  // canonical string form
  semantic::DispatchKind dispatch;
  semantic::EpistemicState epistemic;
  std::string provenance_ref;
};

struct MemoryEffectFactV2 {
  v1::EffectKind kind;
  semantic::MemoryLocation location;
  semantic::EpistemicState epistemic;
  std::string provenance_ref;
};

struct ValueFlowFactV2 {
  core::StableId source_value_id;       // must be kValueRef
  core::StableId destination_value_id;  // must be kValueRef
  semantic::EpistemicState epistemic;
  std::string provenance_ref;
};

struct AliasFactV2 {
  semantic::MemoryLocation left;
  semantic::MemoryLocation right;
  semantic::AliasKind kind;
  semantic::EpistemicState epistemic;
  std::string provenance_ref;
};

// FunctionLocalFactsV2 mirrors the field set of the V1 FunctionLocalFacts
// struct, upgrading only calls/memory_effects/value_flows/alias_facts to typed
// semantic structs.
struct FunctionLocalFactsV2 {
  std::string function_symbol_id;
  std::string function_variant_id;

  // V2-refined components (typed C++ semantic structs).
  std::vector<CallFactV2> calls;
  std::vector<MemoryEffectFactV2> memory_effects;
  std::vector<ValueFlowFactV2> value_flows;
  std::vector<AliasFactV2> aliases;

  // Components V2 does not refine (immutable v1:: proto messages).
  std::vector<v1::BasicBlockSummaryRef> basic_block_summaries;
  std::vector<v1::DominatorSummaryFact> dominator_summaries;
  std::vector<v1::RangeFact> range_facts;
  std::vector<v1::TaintTransfer> taint_transfers;
  std::vector<v1::OwnershipEffect> ownership_effects;
  std::vector<v1::LockEffect> lock_effects;
  std::vector<v1::StateTransition> state_transitions;
  std::vector<v1::Unknown> unknowns;
  std::vector<v1::Assumption> assumptions;
  std::vector<v1::Dependency> dependencies;
  std::vector<v1::ProvenanceRef> provenance_refs;
};

// Build an immutable v2::FunctionSummary from V2 local facts. Every stable ID,
// memory location, and abstract object is validated (mismatched ID kinds and
// invalid locations are rejected, never fabricated); repeated records are
// sorted by deterministic serialized bytes so equivalent input produces
// byte-identical summaries; and component digests are computed with the same
// semantic/evidence split as V1. creation_epoch_ms is pinned to zero.
StatusOr<v2::FunctionSummary> BuildLocalSummaryV2(
    const FunctionLocalFactsV2& facts,
    const build::ProgramContext& context);

}  // namespace veritas::summary

#endif  // VERITAS_SUMMARY_SUMMARY_V2_BUILDER_H_
