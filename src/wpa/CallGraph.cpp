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
#include <set>
#include <utility>

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;

bool IsPositive(v1::EpistemicState epistemic) {
  return epistemic == v1::EPISTEMIC_STATE_MUST ||
         epistemic == v1::EPISTEMIC_STATE_MAY;
}

bool HasFunction(std::span<const core::StableId> functions,
                 const core::StableId& function_id) {
  return std::binary_search(functions.begin(), functions.end(), function_id);
}

}  // namespace

Status CallGraph::AddFunction(core::StableId function_variant_id) {
  if (function_variant_id.kind != core::IdKind::kFunctionVariant) {
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
  if (!IsPositive(edge.epistemic)) {
    return Status::InvalidArgument("call graph edge requires MUST or MAY");
  }
  auto unknown_it = unknown_calls_.find(edge.caller);
  if (unknown_it != unknown_calls_.end() &&
      std::ranges::any_of(unknown_it->second, [&](const auto& unknown) {
        return unknown.call_site_anchor_id == edge.call_site_anchor_id;
      })) {
    return Status::InvalidArgument(
        "call site cannot be both resolved and unknown");
  }

  auto& outgoing = outgoing_[edge.caller];
  auto same_site = std::ranges::find_if(outgoing, [&](const CallEdge& existing) {
    return existing.call_site_anchor_id == edge.call_site_anchor_id;
  });
  if (same_site != outgoing.end()) {
    return *same_site == edge
               ? Status::Ok()
               : Status::InvalidArgument(
                     "conflicting call facts share one call site");
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
  auto outgoing_it = outgoing_.find(effect.caller);
  if (outgoing_it != outgoing_.end() &&
      std::ranges::any_of(outgoing_it->second, [&](const auto& edge) {
        return edge.call_site_anchor_id == effect.call_site_anchor_id;
      })) {
    return Status::InvalidArgument(
        "call site cannot be both resolved and unknown");
  }

  auto& unknowns = unknown_calls_[effect.caller];
  auto same_site =
      std::ranges::find_if(unknowns, [&](const UnknownCallEffect& existing) {
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
  if (it == outgoing_.end()) return {};
  return it->second;
}

std::span<const UnknownCallEffect> CallGraph::UnknownCalls(
    core::StableId caller) const {
  auto it = unknown_calls_.find(caller);
  if (it == unknown_calls_.end()) return {};
  return it->second;
}

StatusOr<CallGraph> CallGraph::FromSummaries(
    std::span<const v1::FunctionSummary> summaries) {
  struct SummaryRef {
    core::StableId function_id;
    const v1::FunctionSummary* summary;
  };

  CallGraph graph;
  std::vector<SummaryRef> ordered;
  ordered.reserve(summaries.size());
  std::set<core::StableId> seen;
  for (const auto& summary : summaries) {
    auto function_id =
        core::ParseStableId(summary.identity().function_variant_id());
    if (!function_id.ok()) return function_id.status();
    if (function_id->kind != core::IdKind::kFunctionVariant) {
      return Status::InvalidArgument(
          "summary identity is not a function-variant ID");
    }
    if (!seen.insert(*function_id).second) {
      return Status::InvalidArgument(
          "multiple current summaries share one function variant");
    }
    auto status = graph.AddFunction(*function_id);
    if (!status.ok()) return status;
    ordered.push_back(SummaryRef{*function_id, &summary});
  }
  std::ranges::sort(ordered, {}, &SummaryRef::function_id);

  for (const auto& entry : ordered) {
    for (const auto& call : entry.summary->calls()) {
      bool resolved = false;
      core::StableId callee;
      if (IsPositive(call.epistemic()) &&
          !call.resolved_callee_function_variant_id().empty()) {
        auto parsed = core::ParseStableId(
            call.resolved_callee_function_variant_id());
        if (parsed.ok() &&
            parsed->kind == core::IdKind::kFunctionVariant &&
            HasFunction(graph.Functions(), *parsed)) {
          callee = *parsed;
          resolved = true;
        }
      }

      Status status;
      if (resolved) {
        status = graph.AddCall(
            CallEdge{.caller = entry.function_id,
                     .callee = std::move(callee),
                     .call_site_anchor_id = call.call_site_anchor_id(),
                     .epistemic = call.epistemic(),
                     .provenance_ref = call.provenance_ref()});
      } else {
        status = graph.AddUnknownCall(
            UnknownCallEffect{.caller = entry.function_id,
                              .call_site_anchor_id =
                                  call.call_site_anchor_id(),
                              .callee_symbol = call.callee_symbol(),
                              .provenance_ref = call.provenance_ref()});
      }
      if (!status.ok()) return status;
    }
  }
  return graph;
}

}  // namespace veritas::wpa
