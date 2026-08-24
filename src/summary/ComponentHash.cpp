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

#include "veritas/summary/ComponentHash.h"

#include <cstddef>
#include <span>
#include <string>

namespace veritas::summary {

namespace {

// ---------------------------------------------------------------------------
// Per-component canonicalization helpers over the v1:: message containers.
//
// The unrefined components (control flow, range facts, taint, ownership,
// locks, state, unknowns, assumptions, dependencies, provenance) reuse the
// immutable v1:: message types in BOTH the v1 and v2 summaries, so their
// canonical bytes are identical across schemas. These helpers are shared by
// the v1 and v2 serializers so a future change to canonicalization happens in
// exactly one place. The refined components (calls, memory effects, value
// flows, alias facts) are deliberately not shared: their v2 layouts differ.
// ---------------------------------------------------------------------------

void AppendControlFlow(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::ControlFlowSummary>
        &entries) {
  for (const auto &cf : entries) {
    const auto &block = cf.block();
    *out += block.basic_block_summary_id();
    *out += '\0';
    *out += block.function_variant_id();
    *out += '\0';
    for (const auto &anchor : block.semantic_source_anchor_ids()) {
      *out += anchor;
      *out += '\0';
    }
    for (const auto &pred : block.predecessor_anchor_ids()) {
      *out += pred;
      *out += '\0';
    }
    for (const auto &succ : block.successor_anchor_ids()) {
      *out += succ;
      *out += '\0';
    }
    for (const auto &dom : cf.dominators()) {
      *out += dom.dominator();
      *out += '\0';
      *out += dom.dominated();
      *out += '\0';
      *out += std::to_string(static_cast<int>(dom.epistemic()));
      *out += '\0';
    }
  }
}

void AppendRangeFacts(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::RangeFact> &facts) {
  for (const auto &fact : facts) {
    *out += fact.variable();
    *out += '\0';
    *out += std::to_string(fact.min_value());
    *out += '\0';
    *out += std::to_string(fact.max_value());
    *out += '\0';
    *out += std::to_string(static_cast<int>(fact.epistemic()));
    *out += '\0';
  }
}

void AppendTaintTransfers(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::TaintTransfer> &taints) {
  for (const auto &taint : taints) {
    *out += taint.source();
    *out += '\0';
    *out += taint.sink();
    *out += '\0';
    *out += taint.taint_kind();
    *out += '\0';
    *out += std::to_string(static_cast<int>(taint.epistemic()));
    *out += '\0';
  }
}

void AppendOwnershipEffects(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::OwnershipEffect> &effects) {
  for (const auto &ownership : effects) {
    *out += std::to_string(static_cast<int>(ownership.kind()));
    *out += '\0';
    *out += ownership.object();
    *out += '\0';
    *out += std::to_string(static_cast<int>(ownership.epistemic()));
    *out += '\0';
  }
}

void AppendLockEffects(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::LockEffect> &locks) {
  for (const auto &lock : locks) {
    *out += std::to_string(static_cast<int>(lock.kind()));
    *out += '\0';
    *out += lock.lock_object();
    *out += '\0';
    *out += std::to_string(static_cast<int>(lock.epistemic()));
    *out += '\0';
  }
}

void AppendStateTransitions(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::StateTransition> &states) {
  for (const auto &state : states) {
    *out += state.from_state();
    *out += '\0';
    *out += state.to_state();
    *out += '\0';
    *out += state.event();
    *out += '\0';
    *out += std::to_string(static_cast<int>(state.epistemic()));
    *out += '\0';
  }
}

void AppendUnknowns(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::Unknown> &unknowns) {
  for (const auto &unknown : unknowns) {
    *out += unknown.kind();
    *out += '\0';
    *out += unknown.reason();
    *out += '\0';
    *out += unknown.scope();
    *out += '\0';
  }
}

void AppendAssumptions(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::Assumption> &assumptions) {
  for (const auto &assumption : assumptions) {
    *out += assumption.description();
    *out += '\0';
    *out += assumption.condition();
    *out += '\0';
  }
}

void AppendDependencies(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::Dependency> &dependencies) {
  for (const auto &dep : dependencies) {
    *out += dep.symbol();
    *out += '\0';
    *out += dep.kind();
    *out += '\0';
  }
}

// Evidence suffix for control-flow entries: provenance lives on the dominator
// facts, not on the block.
void AppendControlFlowEvidence(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::ControlFlowSummary>
        &entries) {
  for (const auto &cf : entries) {
    for (const auto &dom : cf.dominators()) {
      *out += dom.provenance_ref();
    }
  }
}

// Evidence suffix for the components whose evidence is a single provenance_ref
// per entry (range facts, taint, ownership, locks, state, unknowns,
// assumptions, dependencies).
template <typename Entries>
void AppendProvenanceRefs(std::string *out, const Entries &entries) {
  for (const auto &entry : entries) {
    *out += entry.provenance_ref();
  }
}

// Evidence for the provenance component itself: full provenance records.
void AppendProvenanceRecords(
    std::string *out,
    const ::google::protobuf::RepeatedPtrField<v1::ProvenanceRef> &refs) {
  for (const auto &prov : refs) {
    *out += prov.id();
    *out += prov.file_path();
    *out += std::to_string(prov.line());
    *out += std::to_string(prov.column());
    *out += prov.analysis_step();
    *out += prov.display_text();
  }
}

// Serialize a component to canonical bytes for hashing.
// Semantic hash: excludes provenance_ref and display fields.
// Evidence hash: includes provenance_ref and display fields.
std::string SerializeComponentSemantic(v1::ComponentKind kind,
                                       const v1::FunctionSummary &summary) {
  std::string canonical;

  switch (kind) {
  case v1::COMPONENT_KIND_CALLS: {
    for (const auto &call : summary.calls()) {
      canonical += call.callee_symbol();
      canonical += '\0'; // Delimiter to prevent ambiguous concatenation
      canonical += call.resolved_callee_function_variant_id();
      canonical += '\0';
      canonical += call.call_site_anchor_id();
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(call.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_RANGE_FACTS: {
    AppendRangeFacts(&canonical, summary.range_facts());
    break;
  }
  case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
    for (const auto &effect : summary.memory_effects()) {
      canonical += std::to_string(static_cast<int>(effect.kind()));
      canonical += '\0';
      canonical += effect.location();
      canonical += '\0';
      canonical += std::to_string(effect.offset());
      canonical += '\0';
      canonical += std::to_string(effect.size());
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(effect.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_VALUE_FLOW: {
    for (const auto &flow : summary.value_flows()) {
      canonical += flow.source();
      canonical += '\0';
      canonical += flow.sink();
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(flow.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_CONTROL_FLOW: {
    AppendControlFlow(&canonical, summary.control_flow());
    break;
  }
  case v1::COMPONENT_KIND_ALIAS_FACTS: {
    for (const auto &alias : summary.alias_facts()) {
      canonical += alias.location_a();
      canonical += '\0';
      canonical += alias.location_b();
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(alias.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_TAINT: {
    AppendTaintTransfers(&canonical, summary.taint_transfers());
    break;
  }
  case v1::COMPONENT_KIND_OWNERSHIP: {
    AppendOwnershipEffects(&canonical, summary.ownership_effects());
    break;
  }
  case v1::COMPONENT_KIND_LOCKS: {
    AppendLockEffects(&canonical, summary.lock_effects());
    break;
  }
  case v1::COMPONENT_KIND_STATE: {
    AppendStateTransitions(&canonical, summary.state_transitions());
    break;
  }
  case v1::COMPONENT_KIND_UNKNOWNS: {
    AppendUnknowns(&canonical, summary.unknowns());
    break;
  }
  case v1::COMPONENT_KIND_ASSUMPTIONS: {
    AppendAssumptions(&canonical, summary.assumptions());
    break;
  }
  case v1::COMPONENT_KIND_DEPENDENCIES: {
    AppendDependencies(&canonical, summary.dependencies());
    break;
  }
  case v1::COMPONENT_KIND_PROVENANCE: {
    // Provenance semantic hash is empty; only evidence hash matters
    break;
  }
  default:
    break;
  }

  return canonical;
}

std::string SerializeComponentEvidence(v1::ComponentKind kind,
                                       const v1::FunctionSummary &summary) {
  std::string canonical = SerializeComponentSemantic(kind, summary);

  // Add provenance and display fields for evidence hash
  switch (kind) {
  case v1::COMPONENT_KIND_CALLS: {
    for (const auto &call : summary.calls()) {
      canonical += call.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_RANGE_FACTS: {
    AppendProvenanceRefs(&canonical, summary.range_facts());
    break;
  }
  case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
    for (const auto &effect : summary.memory_effects()) {
      canonical += effect.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_VALUE_FLOW: {
    for (const auto &flow : summary.value_flows()) {
      canonical += flow.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_CONTROL_FLOW: {
    AppendControlFlowEvidence(&canonical, summary.control_flow());
    break;
  }
  case v1::COMPONENT_KIND_ALIAS_FACTS: {
    for (const auto &alias : summary.alias_facts()) {
      canonical += alias.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_TAINT: {
    AppendProvenanceRefs(&canonical, summary.taint_transfers());
    break;
  }
  case v1::COMPONENT_KIND_OWNERSHIP: {
    AppendProvenanceRefs(&canonical, summary.ownership_effects());
    break;
  }
  case v1::COMPONENT_KIND_LOCKS: {
    AppendProvenanceRefs(&canonical, summary.lock_effects());
    break;
  }
  case v1::COMPONENT_KIND_STATE: {
    AppendProvenanceRefs(&canonical, summary.state_transitions());
    break;
  }
  case v1::COMPONENT_KIND_UNKNOWNS: {
    AppendProvenanceRefs(&canonical, summary.unknowns());
    break;
  }
  case v1::COMPONENT_KIND_ASSUMPTIONS: {
    AppendProvenanceRefs(&canonical, summary.assumptions());
    break;
  }
  case v1::COMPONENT_KIND_DEPENDENCIES: {
    AppendProvenanceRefs(&canonical, summary.dependencies());
    break;
  }
  case v1::COMPONENT_KIND_PROVENANCE: {
    AppendProvenanceRecords(&canonical, summary.provenance_refs());
    break;
  }
  default:
    break;
  }

  return canonical;
}

int32_t GetItemCount(v1::ComponentKind kind,
                     const v1::FunctionSummary &summary) {
  switch (kind) {
  case v1::COMPONENT_KIND_CALLS:
    return summary.calls_size();
  case v1::COMPONENT_KIND_MEMORY_EFFECTS:
    return summary.memory_effects_size();
  case v1::COMPONENT_KIND_VALUE_FLOW:
    return summary.value_flows_size();
  case v1::COMPONENT_KIND_CONTROL_FLOW:
    return summary.control_flow_size();
  case v1::COMPONENT_KIND_RANGE_FACTS:
    return summary.range_facts_size();
  case v1::COMPONENT_KIND_ALIAS_FACTS:
    return summary.alias_facts_size();
  case v1::COMPONENT_KIND_TAINT:
    return summary.taint_transfers_size();
  case v1::COMPONENT_KIND_OWNERSHIP:
    return summary.ownership_effects_size();
  case v1::COMPONENT_KIND_LOCKS:
    return summary.lock_effects_size();
  case v1::COMPONENT_KIND_STATE:
    return summary.state_transitions_size();
  case v1::COMPONENT_KIND_UNKNOWNS:
    return summary.unknowns_size();
  case v1::COMPONENT_KIND_ASSUMPTIONS:
    return summary.assumptions_size();
  case v1::COMPONENT_KIND_DEPENDENCIES:
    return summary.dependencies_size();
  case v1::COMPONENT_KIND_PROVENANCE:
    return summary.provenance_refs_size();
  default:
    return 0;
  }
}

// V2 structured memory-location serialization used by memory-effect and
// alias-fact semantic hashes. Structured fields are emitted in declaration
// order with '\0' separators; provenance lives on the enclosing message.
void AppendMemoryLocation(std::string *canonical,
                          const v2::MemoryLocation &loc) {
  *canonical += loc.memory_location_id();
  *canonical += '\0';
  const auto &object = loc.object();
  *canonical += object.abstract_object_id();
  *canonical += '\0';
  *canonical += std::to_string(static_cast<int>(object.kind()));
  *canonical += '\0';
  *canonical += object.owner_function_variant_id();
  *canonical += '\0';
  *canonical += object.semantic_anchor_id();
  *canonical += '\0';
  *canonical += object.diagnostic_name();
  *canonical += '\0';
  for (const auto &segment : loc.access_path()) {
    *canonical += std::to_string(static_cast<int>(segment.kind()));
    *canonical += '\0';
    *canonical += std::to_string(segment.first());
    *canonical += '\0';
    *canonical += std::to_string(segment.last());
    *canonical += '\0';
  }
  const auto &byte_range = loc.byte_range();
  *canonical += byte_range.offset_known() ? "1" : "0";
  *canonical += '\0';
  *canonical += std::to_string(byte_range.offset());
  *canonical += '\0';
  *canonical += byte_range.size_known() ? "1" : "0";
  *canonical += '\0';
  *canonical += std::to_string(byte_range.size());
  *canonical += '\0';
}

std::string SerializeComponentSemanticV2(v1::ComponentKind kind,
                                         const v2::FunctionSummary &summary) {
  std::string canonical;

  switch (kind) {
  case v1::COMPONENT_KIND_CALLS: {
    for (const auto &call : summary.calls()) {
      canonical += call.call_site_id();
      canonical += '\0';
      canonical += call.callee_symbol();
      canonical += '\0';
      canonical += call.resolved_callee_function_variant_id();
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(call.dispatch()));
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(call.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
    for (const auto &effect : summary.memory_effects()) {
      canonical += std::to_string(static_cast<int>(effect.kind()));
      canonical += '\0';
      AppendMemoryLocation(&canonical, effect.location());
      canonical += std::to_string(static_cast<int>(effect.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_VALUE_FLOW: {
    for (const auto &flow : summary.value_flows()) {
      canonical += flow.source_value_id();
      canonical += '\0';
      canonical += flow.destination_value_id();
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(flow.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_ALIAS_FACTS: {
    for (const auto &alias : summary.alias_facts()) {
      AppendMemoryLocation(&canonical, alias.left());
      AppendMemoryLocation(&canonical, alias.right());
      canonical += std::to_string(static_cast<int>(alias.kind()));
      canonical += '\0';
      canonical += std::to_string(static_cast<int>(alias.epistemic()));
      canonical += '\0';
    }
    break;
  }
  case v1::COMPONENT_KIND_CONTROL_FLOW: {
    AppendControlFlow(&canonical, summary.control_flow());
    break;
  }
  case v1::COMPONENT_KIND_RANGE_FACTS: {
    AppendRangeFacts(&canonical, summary.range_facts());
    break;
  }
  case v1::COMPONENT_KIND_TAINT: {
    AppendTaintTransfers(&canonical, summary.taint_transfers());
    break;
  }
  case v1::COMPONENT_KIND_OWNERSHIP: {
    AppendOwnershipEffects(&canonical, summary.ownership_effects());
    break;
  }
  case v1::COMPONENT_KIND_LOCKS: {
    AppendLockEffects(&canonical, summary.lock_effects());
    break;
  }
  case v1::COMPONENT_KIND_STATE: {
    AppendStateTransitions(&canonical, summary.state_transitions());
    break;
  }
  case v1::COMPONENT_KIND_UNKNOWNS: {
    AppendUnknowns(&canonical, summary.unknowns());
    break;
  }
  case v1::COMPONENT_KIND_ASSUMPTIONS: {
    AppendAssumptions(&canonical, summary.assumptions());
    break;
  }
  case v1::COMPONENT_KIND_DEPENDENCIES: {
    AppendDependencies(&canonical, summary.dependencies());
    break;
  }
  case v1::COMPONENT_KIND_PROVENANCE: {
    break;
  }
  default:
    break;
  }

  return canonical;
}

std::string SerializeComponentEvidenceV2(v1::ComponentKind kind,
                                         const v2::FunctionSummary &summary) {
  std::string canonical = SerializeComponentSemanticV2(kind, summary);

  switch (kind) {
  case v1::COMPONENT_KIND_CALLS: {
    for (const auto &call : summary.calls()) {
      canonical += call.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
    for (const auto &effect : summary.memory_effects()) {
      canonical += effect.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_VALUE_FLOW: {
    for (const auto &flow : summary.value_flows()) {
      canonical += flow.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_ALIAS_FACTS: {
    for (const auto &alias : summary.alias_facts()) {
      canonical += alias.provenance_ref();
    }
    break;
  }
  case v1::COMPONENT_KIND_CONTROL_FLOW: {
    AppendControlFlowEvidence(&canonical, summary.control_flow());
    break;
  }
  case v1::COMPONENT_KIND_RANGE_FACTS: {
    AppendProvenanceRefs(&canonical, summary.range_facts());
    break;
  }
  case v1::COMPONENT_KIND_TAINT: {
    AppendProvenanceRefs(&canonical, summary.taint_transfers());
    break;
  }
  case v1::COMPONENT_KIND_OWNERSHIP: {
    AppendProvenanceRefs(&canonical, summary.ownership_effects());
    break;
  }
  case v1::COMPONENT_KIND_LOCKS: {
    AppendProvenanceRefs(&canonical, summary.lock_effects());
    break;
  }
  case v1::COMPONENT_KIND_STATE: {
    AppendProvenanceRefs(&canonical, summary.state_transitions());
    break;
  }
  case v1::COMPONENT_KIND_UNKNOWNS: {
    AppendProvenanceRefs(&canonical, summary.unknowns());
    break;
  }
  case v1::COMPONENT_KIND_ASSUMPTIONS: {
    AppendProvenanceRefs(&canonical, summary.assumptions());
    break;
  }
  case v1::COMPONENT_KIND_DEPENDENCIES: {
    AppendProvenanceRefs(&canonical, summary.dependencies());
    break;
  }
  case v1::COMPONENT_KIND_PROVENANCE: {
    AppendProvenanceRecords(&canonical, summary.provenance_refs());
    break;
  }
  default:
    break;
  }

  return canonical;
}

int32_t GetItemCountV2(v1::ComponentKind kind,
                       const v2::FunctionSummary &summary) {
  switch (kind) {
  case v1::COMPONENT_KIND_CALLS:
    return summary.calls_size();
  case v1::COMPONENT_KIND_MEMORY_EFFECTS:
    return summary.memory_effects_size();
  case v1::COMPONENT_KIND_VALUE_FLOW:
    return summary.value_flows_size();
  case v1::COMPONENT_KIND_CONTROL_FLOW:
    return summary.control_flow_size();
  case v1::COMPONENT_KIND_RANGE_FACTS:
    return summary.range_facts_size();
  case v1::COMPONENT_KIND_ALIAS_FACTS:
    return summary.alias_facts_size();
  case v1::COMPONENT_KIND_TAINT:
    return summary.taint_transfers_size();
  case v1::COMPONENT_KIND_OWNERSHIP:
    return summary.ownership_effects_size();
  case v1::COMPONENT_KIND_LOCKS:
    return summary.lock_effects_size();
  case v1::COMPONENT_KIND_STATE:
    return summary.state_transitions_size();
  case v1::COMPONENT_KIND_UNKNOWNS:
    return summary.unknowns_size();
  case v1::COMPONENT_KIND_ASSUMPTIONS:
    return summary.assumptions_size();
  case v1::COMPONENT_KIND_DEPENDENCIES:
    return summary.dependencies_size();
  case v1::COMPONENT_KIND_PROVENANCE:
    return summary.provenance_refs_size();
  default:
    return 0;
  }
}

} // namespace

ComponentDigestInfo ComputeComponentDigest(v1::ComponentKind kind,
                                           const v1::FunctionSummary &summary) {
  std::string semantic_bytes = SerializeComponentSemantic(kind, summary);
  std::string evidence_bytes = SerializeComponentEvidence(kind, summary);

  auto semantic_span = std::as_bytes(std::span(semantic_bytes));
  auto evidence_span = std::as_bytes(std::span(evidence_bytes));

  ComponentDigestInfo info;
  info.kind = kind;
  info.semantic_hash = core::ComputeSHA256(semantic_span);
  info.evidence_hash = core::ComputeSHA256(evidence_span);
  info.item_count = GetItemCount(kind, summary);
  info.payload_offset = 0; // Will be computed by serialization layer
  info.payload_length = static_cast<int64_t>(semantic_bytes.size());

  return info;
}

std::vector<ComponentDigestInfo>
ComputeComponentDigests(const v1::FunctionSummary &summary) {
  std::vector<ComponentDigestInfo> digests;

  // Compute digests for all component kinds
  for (int i = v1::COMPONENT_KIND_CALLS; i <= v1::COMPONENT_KIND_PROVENANCE;
       ++i) {
    auto kind = static_cast<v1::ComponentKind>(i);
    digests.push_back(ComputeComponentDigest(kind, summary));
  }

  return digests;
}

ComponentDigestInfo ComputeComponentDigest(v1::ComponentKind kind,
                                           const v2::FunctionSummary &summary) {
  std::string semantic_bytes = SerializeComponentSemanticV2(kind, summary);
  std::string evidence_bytes = SerializeComponentEvidenceV2(kind, summary);

  auto semantic_span = std::as_bytes(std::span(semantic_bytes));
  auto evidence_span = std::as_bytes(std::span(evidence_bytes));

  ComponentDigestInfo info;
  info.kind = kind;
  info.semantic_hash = core::ComputeSHA256(semantic_span);
  info.evidence_hash = core::ComputeSHA256(evidence_span);
  info.item_count = GetItemCountV2(kind, summary);
  info.payload_offset = 0; // Will be computed by serialization layer
  info.payload_length = static_cast<int64_t>(semantic_bytes.size());

  return info;
}

std::vector<ComponentDigestInfo>
ComputeComponentDigests(const v2::FunctionSummary &summary) {
  std::vector<ComponentDigestInfo> digests;

  // Compute digests for all component kinds
  for (int i = v1::COMPONENT_KIND_CALLS; i <= v1::COMPONENT_KIND_PROVENANCE;
       ++i) {
    auto kind = static_cast<v1::ComponentKind>(i);
    digests.push_back(ComputeComponentDigest(kind, summary));
  }

  return digests;
}

} // namespace veritas::summary
