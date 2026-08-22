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

#include "veritas/summary/SummaryV2Builder.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/ComponentHash.h"

namespace veritas::summary {
namespace {

using veritas::analysis::semantic::AccessPathSegment;
using veritas::analysis::semantic::AliasKind;
using veritas::analysis::semantic::DispatchKind;
using veritas::analysis::semantic::EpistemicState;

// Sort a vector of proto messages by serialized bytes so equivalent input
// produces byte-identical summaries regardless of insertion order.
template <typename Message>
void SortBySerialized(std::vector<Message>* items) {
  std::sort(items->begin(), items->end(), [](const Message& a, const Message& b) {
    return a.SerializeAsString() < b.SerializeAsString();
  });
}

// Reject a stable ID whose kind does not match the expected kind.
Status ValidateStableIdKind(const core::StableId& id, core::IdKind expected,
                            std::string_view field) {
  if (id.kind != expected) {
    return Status::InvalidArgument(std::string(field) +
                                   " has the wrong ID kind");
  }
  return Status::Ok();
}

v1::EpistemicState ToProto(EpistemicState state) {
  switch (state) {
  case EpistemicState::kMust:
    return v1::EPISTEMIC_STATE_MUST;
  case EpistemicState::kMay:
    return v1::EPISTEMIC_STATE_MAY;
  case EpistemicState::kMustNot:
    return v1::EPISTEMIC_STATE_MUST_NOT;
  case EpistemicState::kInferred:
    return v1::EPISTEMIC_STATE_INFERRED;
  case EpistemicState::kAssumed:
    return v1::EPISTEMIC_STATE_ASSUMED;
  case EpistemicState::kUnknown:
    return v1::EPISTEMIC_STATE_UNKNOWN;
  }
  return v1::EPISTEMIC_STATE_UNSPECIFIED;
}

v2::AliasKind ToProto(AliasKind kind) {
  switch (kind) {
  case AliasKind::kMustAlias:
    return v2::ALIAS_KIND_MUST_ALIAS;
  case AliasKind::kMayAlias:
    return v2::ALIAS_KIND_MAY_ALIAS;
  case AliasKind::kNoAlias:
    return v2::ALIAS_KIND_NO_ALIAS;
  case AliasKind::kUnknownAlias:
    return v2::ALIAS_KIND_UNKNOWN_ALIAS;
  }
  return v2::ALIAS_KIND_UNSPECIFIED;
}

v2::DispatchKind ToProto(DispatchKind kind) {
  switch (kind) {
  case DispatchKind::kDirect:
    return v2::DISPATCH_KIND_DIRECT;
  case DispatchKind::kIndirect:
    return v2::DISPATCH_KIND_INDIRECT;
  case DispatchKind::kVirtual:
    return v2::DISPATCH_KIND_VIRTUAL;
  case DispatchKind::kCallback:
    return v2::DISPATCH_KIND_CALLBACK;
  case DispatchKind::kExternal:
    return v2::DISPATCH_KIND_EXTERNAL;
  case DispatchKind::kUnknown:
    return v2::DISPATCH_KIND_UNKNOWN;
  }
  return v2::DISPATCH_KIND_UNSPECIFIED;
}

v2::AbstractObjectKind ToProto(
    veritas::analysis::semantic::AbstractObjectKind kind) {
  switch (kind) {
  case veritas::analysis::semantic::AbstractObjectKind::kGlobal:
    return v2::ABSTRACT_OBJECT_KIND_GLOBAL;
  case veritas::analysis::semantic::AbstractObjectKind::kStack:
    return v2::ABSTRACT_OBJECT_KIND_STACK;
  case veritas::analysis::semantic::AbstractObjectKind::kHeap:
    return v2::ABSTRACT_OBJECT_KIND_HEAP;
  case veritas::analysis::semantic::AbstractObjectKind::kArgument:
    return v2::ABSTRACT_OBJECT_KIND_ARGUMENT;
  case veritas::analysis::semantic::AbstractObjectKind::kFunction:
    return v2::ABSTRACT_OBJECT_KIND_FUNCTION;
  case veritas::analysis::semantic::AbstractObjectKind::kExternal:
    return v2::ABSTRACT_OBJECT_KIND_EXTERNAL;
  case veritas::analysis::semantic::AbstractObjectKind::kUnknown:
    return v2::ABSTRACT_OBJECT_KIND_UNKNOWN;
  case veritas::analysis::semantic::AbstractObjectKind::kLegacyOpaque:
    return v2::ABSTRACT_OBJECT_KIND_LEGACY_OPAQUE;
  }
  return v2::ABSTRACT_OBJECT_KIND_UNSPECIFIED;
}

v2::AccessPathSegment::Kind ToProto(AccessPathSegment::Kind kind) {
  switch (kind) {
  case AccessPathSegment::Kind::kField:
    return v2::AccessPathSegment::KIND_FIELD;
  case AccessPathSegment::Kind::kArrayIndex:
    return v2::AccessPathSegment::KIND_ARRAY_INDEX;
  case AccessPathSegment::Kind::kArrayRange:
    return v2::AccessPathSegment::KIND_ARRAY_RANGE;
  case AccessPathSegment::Kind::kUnknown:
    return v2::AccessPathSegment::KIND_UNKNOWN;
  }
  return v2::AccessPathSegment::KIND_UNSPECIFIED;
}

StatusOr<v2::ByteRange> ToProto(
    const veritas::analysis::semantic::ByteRange& range) {
  if (Status status = veritas::analysis::semantic::Validate(range);
      !status.ok()) {
    return status;
  }
  v2::ByteRange out;
  if (range.offset.has_value()) {
    out.set_offset_known(true);
    out.set_offset(*range.offset);
  }
  if (range.size.has_value()) {
    out.set_size_known(true);
    out.set_size(*range.size);
  }
  return out;
}

StatusOr<v2::AbstractObject> ToProto(
    const veritas::analysis::semantic::AbstractObject& object) {
  if (Status status = veritas::analysis::semantic::Validate(object);
      !status.ok()) {
    return status;
  }
  v2::AbstractObject out;
  out.set_abstract_object_id(core::ToString(object.id));
  out.set_kind(ToProto(object.kind));
  if (object.owner_function.has_value()) {
    out.set_owner_function_variant_id(core::ToString(*object.owner_function));
  }
  out.set_semantic_anchor_id(object.semantic_anchor);
  out.set_diagnostic_name(object.diagnostic_name);
  return out;
}

StatusOr<v2::MemoryLocation> ToProto(
    const veritas::analysis::semantic::MemoryLocation& location) {
  if (Status status = veritas::analysis::semantic::Validate(location);
      !status.ok()) {
    return status;
  }
  v2::MemoryLocation out;
  out.set_memory_location_id(core::ToString(location.id));
  {
    auto object = ToProto(location.object);
    if (!object.ok()) {
      return object.status();
    }
    *out.mutable_object() = std::move(*object);
  }
  for (const auto& segment : location.access_path) {
    v2::AccessPathSegment proto_segment;
    proto_segment.set_kind(ToProto(segment.kind));
    proto_segment.set_first(segment.first);
    proto_segment.set_last(segment.last);
    *out.add_access_path() = std::move(proto_segment);
  }
  {
    auto byte_range = ToProto(location.byte_range);
    if (!byte_range.ok()) {
      return byte_range.status();
    }
    *out.mutable_byte_range() = std::move(*byte_range);
  }
  return out;
}

StatusOr<v2::Call> ToProto(const CallFactV2& call) {
  if (Status status =
          ValidateStableIdKind(call.call_site_id, core::IdKind::kCallSite,
                               "call_site_id");
      !status.ok()) {
    return status;
  }
  v2::Call out;
  out.set_call_site_id(core::ToString(call.call_site_id));
  out.set_callee_symbol(call.callee_symbol);
  out.set_resolved_callee_function_variant_id(
      call.resolved_callee_function_variant_id);
  out.set_dispatch(ToProto(call.dispatch));
  out.set_epistemic(ToProto(call.epistemic));
  out.set_provenance_ref(call.provenance_ref);
  return out;
}

StatusOr<v2::MemoryEffect> ToProto(const MemoryEffectFactV2& effect) {
  auto location = ToProto(effect.location);
  if (!location.ok()) {
    return location.status();
  }
  v2::MemoryEffect out;
  out.set_kind(effect.kind);
  *out.mutable_location() = std::move(*location);
  out.set_epistemic(ToProto(effect.epistemic));
  out.set_provenance_ref(effect.provenance_ref);
  return out;
}

StatusOr<v2::ValueFlow> ToProto(const ValueFlowFactV2& flow) {
  if (Status status =
          ValidateStableIdKind(flow.source_value_id, core::IdKind::kValueRef,
                               "source_value_id");
      !status.ok()) {
    return status;
  }
  if (Status status = ValidateStableIdKind(flow.destination_value_id,
                                           core::IdKind::kValueRef,
                                           "destination_value_id");
      !status.ok()) {
    return status;
  }
  v2::ValueFlow out;
  out.set_source_value_id(core::ToString(flow.source_value_id));
  out.set_destination_value_id(core::ToString(flow.destination_value_id));
  out.set_epistemic(ToProto(flow.epistemic));
  out.set_provenance_ref(flow.provenance_ref);
  return out;
}

StatusOr<v2::AliasFact> ToProto(const AliasFactV2& alias) {
  auto left = ToProto(alias.left);
  if (!left.ok()) {
    return left.status();
  }
  auto right = ToProto(alias.right);
  if (!right.ok()) {
    return right.status();
  }
  v2::AliasFact out;
  *out.mutable_left() = std::move(*left);
  *out.mutable_right() = std::move(*right);
  out.set_kind(ToProto(alias.kind));
  out.set_epistemic(ToProto(alias.epistemic));
  out.set_provenance_ref(alias.provenance_ref);
  return out;
}

// Convert a component digest to its v1::ComponentDigest proto form.
void AppendComponentDigest(const ComponentDigestInfo& digest,
                           v2::FunctionSummary* summary) {
  auto* proto = summary->add_component_digests();
  proto->set_kind(digest.kind);
  proto->set_semantic_hash(
      std::string(reinterpret_cast<const char*>(digest.semantic_hash.data()),
                  digest.semantic_hash.size()));
  proto->set_evidence_hash(
      std::string(reinterpret_cast<const char*>(digest.evidence_hash.data()),
                  digest.evidence_hash.size()));
  proto->set_item_count(digest.item_count);
  proto->set_payload_offset(digest.payload_offset);
  proto->set_payload_length(digest.payload_length);
}

}  // namespace

StatusOr<v2::FunctionSummary> BuildLocalSummaryV2(
    const FunctionLocalFactsV2& facts,
    const build::ProgramContext& context) {
  v2::FunctionSummary summary;

  auto* header = summary.mutable_header();
  header->set_schema_version("summary.v2");
  header->set_creation_epoch_ms(0);  // pinned to keep FunctionSummaryID stable

  auto* identity = summary.mutable_identity();
  identity->set_repository_id(context.repository_id);
  identity->set_revision_id(context.revision_id);
  identity->set_build_variant_id(context.build_variant_id);
  identity->set_function_variant_id(facts.function_variant_id);

  // Refined components: convert to proto (validating), sort, then append.
  std::vector<v2::Call> calls;
  for (const auto& call : facts.calls) {
    auto proto = ToProto(call);
    if (!proto.ok()) {
      return proto.status();
    }
    calls.push_back(std::move(*proto));
  }
  SortBySerialized(&calls);
  for (const auto& call : calls) {
    *summary.add_calls() = call;
  }

  std::vector<v2::MemoryEffect> memory_effects;
  for (const auto& effect : facts.memory_effects) {
    auto proto = ToProto(effect);
    if (!proto.ok()) {
      return proto.status();
    }
    memory_effects.push_back(std::move(*proto));
  }
  SortBySerialized(&memory_effects);
  for (const auto& effect : memory_effects) {
    *summary.add_memory_effects() = effect;
  }

  std::vector<v2::ValueFlow> value_flows;
  for (const auto& flow : facts.value_flows) {
    auto proto = ToProto(flow);
    if (!proto.ok()) {
      return proto.status();
    }
    value_flows.push_back(std::move(*proto));
  }
  SortBySerialized(&value_flows);
  for (const auto& flow : value_flows) {
    *summary.add_value_flows() = flow;
  }

  std::vector<v2::AliasFact> alias_facts;
  for (const auto& alias : facts.aliases) {
    auto proto = ToProto(alias);
    if (!proto.ok()) {
      return proto.status();
    }
    alias_facts.push_back(std::move(*proto));
  }
  SortBySerialized(&alias_facts);
  for (const auto& alias : alias_facts) {
    *summary.add_alias_facts() = alias;
  }

  // Unrefined components: copy, sort, then append.
  std::vector<v1::RangeFact> range_facts = facts.range_facts;
  std::vector<v1::TaintTransfer> taint_transfers = facts.taint_transfers;
  std::vector<v1::OwnershipEffect> ownership_effects = facts.ownership_effects;
  std::vector<v1::LockEffect> lock_effects = facts.lock_effects;
  std::vector<v1::StateTransition> state_transitions = facts.state_transitions;
  std::vector<v1::Unknown> unknowns = facts.unknowns;
  std::vector<v1::Assumption> assumptions = facts.assumptions;
  std::vector<v1::Dependency> dependencies = facts.dependencies;
  std::vector<v1::ProvenanceRef> provenance_refs = facts.provenance_refs;
  SortBySerialized(&range_facts);
  SortBySerialized(&taint_transfers);
  SortBySerialized(&ownership_effects);
  SortBySerialized(&lock_effects);
  SortBySerialized(&state_transitions);
  SortBySerialized(&unknowns);
  SortBySerialized(&assumptions);
  SortBySerialized(&dependencies);
  SortBySerialized(&provenance_refs);

  for (const auto& range : range_facts) {
    *summary.add_range_facts() = range;
  }
  for (const auto& taint : taint_transfers) {
    *summary.add_taint_transfers() = taint;
  }
  for (const auto& ownership : ownership_effects) {
    *summary.add_ownership_effects() = ownership;
  }
  for (const auto& lock : lock_effects) {
    *summary.add_lock_effects() = lock;
  }
  for (const auto& state : state_transitions) {
    *summary.add_state_transitions() = state;
  }
  for (const auto& unknown : unknowns) {
    *summary.add_unknowns() = unknown;
  }
  for (const auto& assumption : assumptions) {
    *summary.add_assumptions() = assumption;
  }
  for (const auto& dependency : dependencies) {
    *summary.add_dependencies() = dependency;
  }
  for (const auto& provenance : provenance_refs) {
    *summary.add_provenance_refs() = provenance;
  }

  // Basic-block summaries map one-to-one into control-flow entries; dominator
  // facts are grouped into a single trailing control-flow entry.
  for (const auto& block : facts.basic_block_summaries) {
    summary.add_control_flow()->mutable_block()->CopyFrom(block);
  }
  if (!facts.dominator_summaries.empty()) {
    auto* control_flow = summary.add_control_flow();
    for (const auto& dominator : facts.dominator_summaries) {
      control_flow->add_dominators()->CopyFrom(dominator);
    }
  }

  // Compute component digests with the same semantic/evidence split as V1 and
  // attach them so downstream consumers can invalidate per-component.
  auto digests = ComputeComponentDigests(summary);
  for (const auto& digest : digests) {
    AppendComponentDigest(digest, &summary);
  }

  return summary;
}

}  // namespace veritas::summary
