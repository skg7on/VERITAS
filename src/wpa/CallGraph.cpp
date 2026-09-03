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

#include "veritas/wpa/CallGraph.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

bool IsPositive(v1::EpistemicState epistemic) {
  return epistemic == v1::EPISTEMIC_STATE_MUST ||
         epistemic == v1::EPISTEMIC_STATE_MAY;
}

// Epistemic states that may carry a resolved call edge. INFERRED and ASSUMED
// name a real target under weaker warrant (a model, or an inference), so they
// belong in the graph; MUST_NOT and UNKNOWN never assert a target.
bool IsEdgeAdmissible(v1::EpistemicState epistemic) {
  return IsPositive(epistemic) ||
         epistemic == v1::EPISTEMIC_STATE_INFERRED ||
         epistemic == v1::EPISTEMIC_STATE_ASSUMED;
}

bool HasFunction(std::span<const core::StableId> functions,
                 const core::StableId &function_id) {
  return std::binary_search(functions.begin(), functions.end(), function_id);
}

// A call read from either schema version. `admissible` records whether the
// source schema lets this epistemic state carry a resolved edge: V1 admits
// only MUST and MAY, while V2 also admits INFERRED and ASSUMED. Normalizing
// here keeps the graph-building loop identical for both versions and stops a
// tagged V1 projection from claiming V2 precision.
struct NormalizedCall {
  std::string call_site_anchor_id;
  std::string callee_symbol;
  std::string resolved_callee;
  std::string provenance_ref;
  v1::EpistemicState epistemic;
  bool admissible;
};

std::vector<NormalizedCall>
NormalizeCalls(const summary::SummaryArtifact &artifact) {
  std::vector<NormalizedCall> calls;
  if (const auto *v1_summary = std::get_if<v1::FunctionSummary>(&artifact)) {
    calls.reserve(static_cast<std::size_t>(v1_summary->calls_size()));
    for (const auto &call : v1_summary->calls()) {
      calls.push_back(
          NormalizedCall{.call_site_anchor_id = call.call_site_anchor_id(),
                         .callee_symbol = call.callee_symbol(),
                         .resolved_callee =
                             call.resolved_callee_function_variant_id(),
                         .provenance_ref = call.provenance_ref(),
                         .epistemic = call.epistemic(),
                         .admissible = IsPositive(call.epistemic())});
    }
    return calls;
  }

  const auto &v2_summary = std::get<summary::v2::FunctionSummary>(artifact);
  calls.reserve(static_cast<std::size_t>(v2_summary.calls_size()));
  for (const auto &call : v2_summary.calls()) {
    calls.push_back(
        NormalizedCall{.call_site_anchor_id = call.call_site_id(),
                       .callee_symbol = call.callee_symbol(),
                       .resolved_callee =
                           call.resolved_callee_function_variant_id(),
                       .provenance_ref = call.provenance_ref(),
                       .epistemic = call.epistemic(),
                       .admissible = IsEdgeAdmissible(call.epistemic())});
  }
  return calls;
}

StatusOr<core::StableId> ParseFunctionVariantId(std::string_view text,
                                                std::string_view context) {
  auto id = core::ParseStableId(text);
  if (!id.ok()) {
    return Status::InvalidArgument(std::string(context) + ": " +
                                   std::string(id.status().message()));
  }
  if (!core::HexToDigest(id->digest_hex).has_value()) {
    return Status::InvalidArgument(std::string(context) +
                                   ": invalid SHA-256 digest");
  }
  if (id->kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument(std::string(context) +
                                   ": expected a function-variant ID");
  }
  return *id;
}

} // namespace

Status CallGraph::AddFunction(core::StableId function_variant_id) {
  auto parsed = core::ParseStableId(core::ToString(function_variant_id));
  if (!parsed.ok() || *parsed != function_variant_id ||
      function_variant_id.kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument(
        "call graph vertices require function-variant IDs");
  }
  auto position = std::lower_bound(functions_.begin(), functions_.end(),
                                   function_variant_id);
  if (position == functions_.end() || *position != function_variant_id) {
    functions_.insert(position, std::move(function_variant_id));
  }
  return Status::Ok();
}

Status CallGraph::AddCall(CallEdge edge) {
  if (edge.caller.kind != core::IdKind::kFunctionVariant ||
      edge.callee.kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument(
        "call graph edges require function-variant IDs");
  }
  if (!HasFunction(functions_, edge.caller) ||
      !HasFunction(functions_, edge.callee)) {
    return Status::NotFound("call graph edge endpoint is not a vertex");
  }
  if (!IsEdgeAdmissible(edge.epistemic)) {
    return Status::InvalidArgument(
        "call graph edge requires MUST, MAY, INFERRED, or ASSUMED");
  }

  // A call site may name some resolved targets and still carry an unknown
  // flag: a function pointer's points-to set is often partial. Coexistence is
  // therefore allowed, not an error.

  auto &outgoing = outgoing_[edge.caller];
  auto same_target =
      std::ranges::find_if(outgoing, [&](const CallEdge &existing) {
        return existing.call_site_anchor_id == edge.call_site_anchor_id &&
               existing.callee == edge.callee;
      });
  if (same_target != outgoing.end()) {
    return *same_target == edge
               ? Status::Ok()
               : Status::InvalidArgument(
                     "conflicting call facts share one call-site target");
  }
  const auto same_site =
      std::ranges::find_if(outgoing, [&](const CallEdge &existing) {
        return existing.call_site_anchor_id == edge.call_site_anchor_id;
      });
  // One call site may name several targets only when no target is asserted as
  // the definite one. For the MUST/MAY-only V1 domain this is exactly the
  // previous "both must be MAY" rule.
  if (same_site != outgoing.end() &&
      (same_site->epistemic == v1::EPISTEMIC_STATE_MUST ||
       edge.epistemic == v1::EPISTEMIC_STATE_MUST)) {
    return Status::InvalidArgument(
        "multiple call-site targets require non-MUST call facts");
  }
  outgoing.push_back(std::move(edge));
  std::ranges::sort(outgoing);
  return Status::Ok();
}

Status CallGraph::AddUnknownCall(UnknownCallEffect effect) {
  if (effect.caller.kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument(
        "unknown call requires a function-variant caller");
  }
  if (!HasFunction(functions_, effect.caller)) {
    return Status::NotFound("unknown call caller is not a vertex");
  }

  // A call site may carry both resolved (partial) targets and an unknown flag;
  // see AddCall.

  auto &unknowns = unknown_calls_[effect.caller];
  auto same_site =
      std::ranges::find_if(unknowns, [&](const UnknownCallEffect &existing) {
        return existing.call_site_anchor_id == effect.call_site_anchor_id;
      });
  if (same_site != unknowns.end()) {
    return *same_site == effect
               ? Status::Ok()
               : Status::InvalidArgument(
                     "conflicting unknown facts share one call site");
  }
  unknowns.push_back(std::move(effect));
  std::ranges::sort(unknowns);
  return Status::Ok();
}

std::span<const CallEdge> CallGraph::Outgoing(core::StableId caller) const {
  auto it = outgoing_.find(caller);
  if (it == outgoing_.end())
    return {};
  return it->second;
}

std::span<const UnknownCallEffect>
CallGraph::UnknownCalls(core::StableId caller) const {
  auto it = unknown_calls_.find(caller);
  if (it == unknown_calls_.end())
    return {};
  return it->second;
}

StatusOr<CallGraph>
CallGraph::FromSummaries(std::span<const v1::FunctionSummary> summaries) {
  std::vector<summary::SummaryArtifact> artifacts;
  artifacts.reserve(summaries.size());
  for (const auto &summary : summaries) {
    artifacts.emplace_back(summary);
  }
  return FromSummaries(std::span<const summary::SummaryArtifact>(artifacts));
}

StatusOr<CallGraph>
CallGraph::FromSummaries(std::span<const summary::SummaryArtifact> summaries) {
  struct SummaryRef {
    core::StableId function_id;
    const summary::SummaryArtifact *artifact;
  };

  CallGraph graph;
  std::vector<SummaryRef> ordered;
  ordered.reserve(summaries.size());
  std::set<core::StableId> seen;
  for (const auto &artifact : summaries) {
    auto function_id =
        ParseFunctionVariantId(summary::Identity(artifact).function_variant_id(),
                               "invalid summary identity");
    if (!function_id.ok())
      return function_id.status();
    if (!seen.insert(*function_id).second) {
      return Status::InvalidArgument(
          "multiple current summaries share one function variant");
    }
    auto status = graph.AddFunction(*function_id);
    if (!status.ok())
      return status;
    ordered.push_back(SummaryRef{*function_id, &artifact});
  }
  std::ranges::sort(ordered, {}, &SummaryRef::function_id);

  for (const auto &entry : ordered) {
    std::vector<NormalizedCall> calls = NormalizeCalls(*entry.artifact);
    for (const auto &call : calls) {
      bool resolved = false;
      core::StableId callee;
      if (call.admissible && !call.resolved_callee.empty()) {
        auto parsed = ParseFunctionVariantId(call.resolved_callee,
                                             "invalid resolved callee identity");
        if (!parsed.ok())
          return parsed.status();
        if (HasFunction(graph.Functions(), *parsed)) {
          callee = *parsed;
          resolved = true;
        }
      }

      Status status;
      if (resolved) {
        status = graph.AddCall(
            CallEdge{.caller = entry.function_id,
                     .callee = std::move(callee),
                     .call_site_anchor_id = call.call_site_anchor_id,
                     .epistemic = call.epistemic,
                     .provenance_ref = call.provenance_ref});
      } else {
        status = graph.AddUnknownCall(
            UnknownCallEffect{.caller = entry.function_id,
                              .call_site_anchor_id = call.call_site_anchor_id,
                              .callee_symbol = call.callee_symbol,
                              .provenance_ref = call.provenance_ref});
      }
      if (!status.ok())
        return status;
    }
  }
  return graph;
}

} // namespace veritas::wpa
