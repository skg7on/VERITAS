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

// OverflowClaimSeed.h — deterministic buffer-overflow ClaimSeed resolution.
//
// The `veritas-query evidence overflow` CLI must turn `--sink <value>` plus a
// materialized analysis into the ClaimSeed the evidence builder consumes. That
// resolution is a pure function of the CPG projection and the run's current
// facts, so it lives here — shared by the public CLI and the integration tests
// — rather than being duplicated in either.
//
// Resolution never guesses. Every candidate set is ordered by content-derived
// StableId, and an empty or ambiguous candidate set is a hard error rather than
// an arbitrary pick. Consequently the resolved seed is identical for identical
// analyses, independent of store order, checkout root, and run timing.

#ifndef VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_
#define VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::evidence {

// The seed-resolution vocabulary. `--sink memcpy` matches any CPG node whose
// label contains "memcpy" (Clang lowers the call to the llvm.memcpy.* intrinsic
// in the linked module, and the projection labels the unmodeled external with
// that symbol).
inline constexpr std::string_view kMemcpySink = "memcpy";

// Resolves the buffer-overflow ClaimSeed for `sink` against one analysis.
//
// Selection, in canonical StableId order at every choice point:
//
//   sink_ref    — the deepest CPG value that leaves the analyzed code into the
//                 unmodeled sink's argument list. A value leaves the analyzed
//                 code when it is the source of a GlobalFlow fact whose target
//                 is not a CPG node (the external has no CPG parameter nodes).
//                 Among those "exit" values, the terminal of the kFlowsTo
//                 closure — the one that flows into no further exit value — is
//                 the operand the sink actually consumes. M10A does not encode
//                 formal-parameter positions, so the sink's size, destination,
//                 and source formals cannot be told apart by name; the choice
//                 is therefore canonical, not semantic.
//   source_ref  — the root of that terminal's ancestor chain within the same
//                 exit closure: the origin of the value the sink consumes.
//   subject_ref — the memory object the sink's calling function writes.
//
// kind is BUFFER_OVERFLOW and severity is HIGH: the sink consumes a value whose
// extent is not established at the call, which is the demo's unsafe shape.
// finding_id is content-derived from the resolved refs (never a fixture
// literal), so identical slices carry identical finding identities.
//
// Fails (rather than guessing) with NotFound/InvalidArgument when: the sink has
// no unique call node, no value reaches the sink's argument list, the sink has
// no single owning function, or that function writes no memory object.
StatusOr<ClaimSeed> ResolveOverflowClaimSeed(
    const cpg::ThinCpg& cpg,
    const std::vector<facts::AnalysisFact>& current_facts,
    std::string_view sink = kMemcpySink);

namespace detail {

// The canonical minimum of a non-empty set of StableIds.
inline core::StableId CanonicalMin(const std::set<core::StableId>& ids) {
  return *ids.begin();
}

}  // namespace detail

inline StatusOr<ClaimSeed> ResolveOverflowClaimSeed(
    const cpg::ThinCpg& cpg,
    const std::vector<facts::AnalysisFact>& current_facts,
    std::string_view sink) {
  // --- The sink's call node: labelled with the sink family, and the target of
  // --- a call edge. Zero or several candidates is an error, never a pick.
  std::set<core::StableId> called;
  for (const auto& edge : cpg.edges()) {
    if (edge.kind == cpg::EdgeKind::kCalls ||
        edge.kind == cpg::EdgeKind::kMayCall) {
      called.insert(edge.target_node_id);
    }
  }
  std::set<core::StableId> anchors;
  for (const auto& node : cpg.nodes()) {
    if (called.count(node.node_id) != 0 &&
        node.label.find(sink) != std::string::npos) {
      anchors.insert(node.node_id);
    }
  }
  if (anchors.empty()) {
    return Status::NotFound("no " + std::string(sink) +
                            " call in the CPG projection");
  }
  if (anchors.size() > 1) {
    return Status::InvalidArgument(
        "ambiguous " + std::string(sink) + " call in the CPG projection");
  }
  const core::StableId anchor = *anchors.begin();

  // --- The values that leave the analyzed code into the sink's argument list.
  std::set<core::StableId> node_ids;
  for (const auto& node : cpg.nodes()) {
    node_ids.insert(node.node_id);
  }
  std::set<core::StableId> exit_values;
  for (const auto& fact : current_facts) {
    if (fact.row.relation != facts::RelationId::kGlobalFlow ||
        fact.row.cells.size() < 2) {
      continue;
    }
    const auto* from = std::get_if<core::StableId>(&fact.row.cells[0]);
    const auto* to = std::get_if<core::StableId>(&fact.row.cells[1]);
    if (from == nullptr || to == nullptr) {
      continue;
    }
    if (node_ids.count(*from) != 0 && node_ids.count(*to) == 0) {
      exit_values.insert(*from);
    }
  }
  if (exit_values.empty()) {
    return Status::NotFound(std::string("no value flows into the ") +
                            std::string(sink) + " argument list");
  }

  // --- Internal value-flow edges among the exit values (kFlowsTo).
  std::set<std::pair<core::StableId, core::StableId>> internal;
  for (const auto& edge : cpg.edges()) {
    if (edge.kind != cpg::EdgeKind::kFlowsTo) {
      continue;
    }
    if (exit_values.count(edge.source_node_id) != 0 &&
        exit_values.count(edge.target_node_id) != 0) {
      internal.insert({edge.source_node_id, edge.target_node_id});
    }
  }
  std::set<core::StableId> has_successor;
  std::set<core::StableId> has_predecessor;
  for (const auto& [from, to] : internal) {
    has_successor.insert(from);
    has_predecessor.insert(to);
  }

  std::set<core::StableId> terminals;
  for (const core::StableId& value : exit_values) {
    if (has_successor.count(value) == 0) {
      terminals.insert(value);
    }
  }
  if (terminals.empty()) {
    // Transitively closed kFlowsTo makes this unreachable for a well-formed
    // projection; it would mean every exit value feeds another.
    return Status::Internal("sink operand closure has no terminal value");
  }
  const core::StableId sink_ref = detail::CanonicalMin(terminals);

  // --- The ancestors of the chosen terminal, and the root of that chain.
  std::set<core::StableId> ancestors{sink_ref};
  std::vector<core::StableId> frontier{sink_ref};
  while (!frontier.empty()) {
    const core::StableId current = frontier.back();
    frontier.pop_back();
    for (const auto& [from, to] : internal) {
      if (to != current || ancestors.count(from) != 0) {
        continue;
      }
      ancestors.insert(from);
      frontier.push_back(from);
    }
  }
  std::set<core::StableId> roots;
  for (const core::StableId& ancestor : ancestors) {
    if (has_predecessor.count(ancestor) == 0) {
      roots.insert(ancestor);
    }
  }
  if (roots.empty()) {
    return Status::Internal("sink operand chain has no root value");
  }
  const core::StableId source_ref = detail::CanonicalMin(roots);

  // --- The sink's owning function, and the memory object it writes.
  std::set<core::StableId> callers;
  for (const auto& edge : cpg.edges()) {
    if ((edge.kind == cpg::EdgeKind::kCalls ||
         edge.kind == cpg::EdgeKind::kMayCall) &&
        edge.target_node_id == anchor) {
      callers.insert(edge.source_node_id);
    }
  }
  if (callers.size() != 1) {
    return Status::NotFound("the " + std::string(sink) +
                            " call has no single owning function");
  }
  const core::StableId caller = *callers.begin();

  std::set<core::StableId> subjects;
  for (const auto& edge : cpg.edges()) {
    if (edge.kind == cpg::EdgeKind::kWrites &&
        edge.source_node_id == caller) {
      subjects.insert(edge.target_node_id);
    }
  }
  if (subjects.empty()) {
    return Status::NotFound("the calling function writes no memory object");
  }
  const core::StableId subject_ref = detail::CanonicalMin(subjects);

  std::string finding_bytes = "veritas.evidence.buffer-overflow.v1|";
  finding_bytes += core::ToString(subject_ref);
  finding_bytes += "|";
  finding_bytes += core::ToString(source_ref);
  finding_bytes += "|";
  finding_bytes += core::ToString(sink_ref);

  ClaimSeed seed;
  seed.finding_id = core::MakeStableId(
      core::IdKind::kFact,
      std::as_bytes(std::span(finding_bytes.data(), finding_bytes.size())));
  seed.kind = ClaimKind::kBufferOverflow;
  seed.severity = Severity::kHigh;
  seed.subject_ref = subject_ref;
  seed.source_ref = source_ref;
  seed.sink_ref = sink_ref;
  return seed;
}

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_OVERFLOW_CLAIM_SEED_H_
