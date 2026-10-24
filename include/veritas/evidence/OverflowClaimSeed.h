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

// Exact compiler-bound buffer-overflow seed resolution. Family lookup returns
// one seed or an ambiguity error; explicit callsite selection does not enable
// automatic producer-gated fan-out. Legacy projections remain readable, but
// cannot supply this capability.

#ifndef VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_
#define VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_

#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::evidence {

inline constexpr std::string_view kMemcpySink = "memcpy";

// Destination memory is actual argument 0; source bytes and extent remain in
// the selected CallSiteBinding as arguments 1 and 2. sink_ref is the exact
// extent value. source_ref is its unique caller-local flow root when one can
// be established; otherwise it is the extent itself (no guessed upstream
// origin). This identifies a concern, never capacity, feasibility or a defect.
// current_facts remains part of the public API, but cannot choose operands.
inline StatusOr<ClaimSeed> ResolveOverflowClaimSeed(
    const cpg::ThinCpg &graph,
    const std::vector<facts::AnalysisFact> & /*current_facts*/,
    std::string_view sink = kMemcpySink,
    std::optional<core::StableId> callsite = std::nullopt) {
  if (!graph.HasCompilerOwnership()) {
    return Status::FailedPrecondition(
        "legacy CPG coverage: exact compiler callsite bindings unavailable; "
        "reanalyze");
  }
  auto valid = graph.Validate();
  if (!valid.ok())
    return valid;
  if (sink != kMemcpySink) {
    return Status::InvalidArgument("unsupported overflow sink family");
  }
  const cpg::CallSiteBinding *selected = nullptr;
  for (const auto &binding : graph.callsites()) {
    if (callsite && binding.callsite_id != *callsite)
      continue;
    if (binding.normalized_operation != "memcpy.v1") {
      if (callsite)
        return Status::FailedPrecondition(
            "unsupported sink ABI at selected callsite");
      continue;
    }
    if (selected)
      return Status::InvalidArgument(
          "ambiguous memcpy callsite; select one exact callsite");
    selected = &binding;
  }
  if (!selected)
    return Status::NotFound(
        "no supported memcpy callsite in the CPG projection");
  const auto subject = *selected->arguments[0].memory_ref;
  const auto extent = selected->arguments[2].value_ref;

  // Traverse only compiler-owned values of this caller, backwards from this
  // exact size operand. Other callees' formal parameters, global endpoints and
  // unrelated callers cannot contribute a root.
  std::set<core::StableId> local;
  for (const auto &edge : graph.edges()) {
    if (edge.kind == cpg::EdgeKind::kContains &&
        edge.source_node_id == selected->function_variant_id)
      local.insert(edge.target_node_id);
  }
  std::map<core::StableId, std::set<core::StableId>> predecessors;
  for (const auto &edge : graph.edges()) {
    if (edge.kind == cpg::EdgeKind::kFlowsTo &&
        local.contains(edge.source_node_id) &&
        local.contains(edge.target_node_id)) {
      predecessors[edge.target_node_id].insert(edge.source_node_id);
    }
  }
  std::set<core::StableId> visited{extent}, roots;
  std::vector<core::StableId> frontier{extent};
  while (!frontier.empty()) {
    const auto value = frontier.back();
    frontier.pop_back();
    auto it = predecessors.find(value);
    if (it == predecessors.end() || it->second.empty()) {
      roots.insert(value);
      continue;
    }
    for (const auto &parent : it->second) {
      if (visited.insert(parent).second)
        frontier.push_back(parent);
    }
  }
  const auto source = roots.size() == 1 ? *roots.begin() : extent;
  std::string bytes = "veritas.evidence.buffer-overflow.v2";
  const auto append = [&](std::string_view field) {
    bytes += std::to_string(field.size()) + ":";
    bytes += field;
  };
  append(core::ToString(selected->callsite_id));
  append(core::ToString(selected->function_variant_id));
  append(core::ToString(selected->callee_ref));
  append(core::ToString(subject));
  for (const auto &arg : selected->arguments)
    append(core::ToString(arg.value_ref));
  append(core::ToString(source));
  return ClaimSeed{
      core::MakeStableId(core::IdKind::kFact,
                         std::as_bytes(std::span(bytes.data(), bytes.size()))),
      ClaimKind::kBufferOverflow,
      Severity::kHigh,
      subject,
      source,
      extent,
      selected->callsite_id};
}

}  // namespace veritas::evidence
#endif  // VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_
