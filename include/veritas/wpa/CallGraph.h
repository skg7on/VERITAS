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

#ifndef VERITAS_WPA_CALL_GRAPH_H_
#define VERITAS_WPA_CALL_GRAPH_H_

#include <compare>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::wpa {

struct CallEdge {
  core::StableId caller;
  core::StableId callee;
  std::string call_site_anchor_id;
  summary::v1::EpistemicState epistemic;
  std::string provenance_ref;

  auto operator<=>(const CallEdge &) const = default;
};

struct UnknownCallEffect {
  core::StableId caller;
  std::string call_site_anchor_id;
  std::string callee_symbol;
  std::string provenance_ref;

  auto operator<=>(const UnknownCallEffect &) const = default;
};

class CallGraph {
public:
  static StatusOr<CallGraph>
  FromSummaries(std::span<const summary::v1::FunctionSummary> summaries);

  // Version-neutral construction. A V2 call with a stable resolved target and
  // epistemic MUST, MAY, INFERRED, or ASSUMED becomes an edge, so SVF's
  // indirect and virtual MAY candidates reach the SCC scheduler. An empty or
  // unresolvable target stays a scoped unknown-call effect and never fans out
  // to every function. Tagged V1 artifacts keep their MUST/MAY semantics and
  // cannot fabricate V2 precision.
  static StatusOr<CallGraph>
  FromSummaries(std::span<const summary::SummaryArtifact> summaries);

  Status AddFunction(core::StableId function_variant_id);
  Status AddCall(CallEdge edge);
  Status AddUnknownCall(UnknownCallEffect effect);

  std::span<const core::StableId> Functions() const { return functions_; }
  std::span<const CallEdge> Outgoing(core::StableId caller) const;
  std::span<const UnknownCallEffect> UnknownCalls(core::StableId caller) const;

private:
  std::vector<core::StableId> functions_;
  std::map<core::StableId, std::vector<CallEdge>> outgoing_;
  std::map<core::StableId, std::vector<UnknownCallEffect>> unknown_calls_;
};

} // namespace veritas::wpa

#endif // VERITAS_WPA_CALL_GRAPH_H_
