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

#include "analysis/svf/SvfMerge.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/repeated_field.h>

#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/StableValueMapper.h"
#include "veritas/core/Ids.h"
#include "veritas/summary/ComponentHash.h"

namespace veritas::analysis::svf {
namespace {

namespace v1 = veritas::summary::v1;
namespace semantic = veritas::analysis::semantic;
using veritas::core::ToString;

// semantic::EpistemicState -> v1 proto epistemic state.
v1::EpistemicState ToV1Epistemic(semantic::EpistemicState state) {
  switch (state) {
    case semantic::EpistemicState::kMust:
      return v1::EPISTEMIC_STATE_MUST;
    case semantic::EpistemicState::kMay:
      return v1::EPISTEMIC_STATE_MAY;
    case semantic::EpistemicState::kMustNot:
      return v1::EPISTEMIC_STATE_MUST_NOT;
    case semantic::EpistemicState::kInferred:
      return v1::EPISTEMIC_STATE_INFERRED;
    case semantic::EpistemicState::kAssumed:
      return v1::EPISTEMIC_STATE_ASSUMED;
    case semantic::EpistemicState::kUnknown:
      return v1::EPISTEMIC_STATE_UNKNOWN;
  }
  return v1::EPISTEMIC_STATE_UNKNOWN;
}

std::optional<std::string> OwningFunctionVariant(
    std::string_view value_name,
    const ::veritas::analysis::llvm::OriginMap& origin_map) {
  const auto delimiter = value_name.find(':');
  if (delimiter == std::string_view::npos) return std::nullopt;
  return origin_map.GetSymbolIdByLlvmName(value_name.substr(0, delimiter));
}

bool IsOwnedBy(std::string_view value_name, std::string_view function_id,
               const ::veritas::analysis::llvm::OriginMap& origin_map) {
  auto owner = OwningFunctionVariant(value_name, origin_map);
  if (owner.has_value()) return *owner == function_id;
  // A bare caller name resolves through the origin map by exact LLVM name.
  if (auto id = origin_map.GetSymbolIdByLlvmName(value_name)) {
    return *id == function_id;
  }
  // Preserve compatibility with pre-M8 synthetic facts that embedded the
  // function-variant ID directly in the diagnostic value name.
  return value_name.find(function_id) != std::string_view::npos;
}

}  // namespace

std::vector<v1::FunctionSummary> MergeSvfFacts(
    std::vector<v1::FunctionSummary> drafts,
    const semantic::NormalizedAnalysisFacts& facts,
    const ::veritas::analysis::llvm::OriginMap& origin_map) {
  // Only facts with a recoverable per-function owner may enter a per-function
  // summary.v1 draft. Calls carry their caller's diagnostic name, so they are
  // attributed to the caller's draft. Value-flow, alias, and memory-effect
  // facts are whole-program (their stable IDs carry no recoverable owner), so
  // merging them into a draft would fabricate cross-function facts — e.g. a
  // MUST-level NO_ALIAS between two allocas in function `foo` asserted inside
  // an unrelated function `bar`'s summary. They are therefore gated out of
  // summary.v1; Task 9 re-adds them with precise per-function attribution
  // against summary.v2. Scoped unknowns are run-level diagnostics (never
  // per-function facts) and are attached to every draft, matching the
  // pre-normalization behavior.
  for (auto& draft : drafts) {
    const std::string& function_id = draft.identity().function_variant_id();

    for (const auto& call : facts.calls) {
      if (!IsOwnedBy(call.diagnostic_symbol, function_id, origin_map)) {
        continue;
      }
      auto* out = draft.add_calls();
      out->set_callee_symbol(call.callee ? ToString(*call.callee)
                                         : std::string{});
      out->set_call_site_anchor_id(ToString(call.call_site));
      out->set_epistemic(ToV1Epistemic(call.epistemic));
      out->set_provenance_ref(call.provenance_ref);
    }

    for (const auto& unknown : facts.unknowns) {
      auto* out = draft.add_unknowns();
      out->set_kind("svf");
      out->set_reason(unknown.reason);
      out->set_scope(unknown.scope);
      out->set_provenance_ref(unknown.provenance_ref);
    }
  }

  return drafts;
}

namespace {

namespace v2 = veritas::summary::v2;
using veritas::core::StableId;

// The function-value encoding is defined in exactly one place:
// StableValueMapper::FunctionValueRef (the function-constant path in
// StableValueMapper::AppendFunction builds on the same shared framing). A
// caller is attributed by recomputing that ref from the draft's
// function-variant ID, which is the same string the origin map recorded.

v2::DispatchKind ToV2Dispatch(semantic::DispatchKind kind) {
  switch (kind) {
  case semantic::DispatchKind::kDirect:
    return v2::DISPATCH_KIND_DIRECT;
  case semantic::DispatchKind::kIndirect:
    return v2::DISPATCH_KIND_INDIRECT;
  case semantic::DispatchKind::kVirtual:
    return v2::DISPATCH_KIND_VIRTUAL;
  case semantic::DispatchKind::kCallback:
    return v2::DISPATCH_KIND_CALLBACK;
  case semantic::DispatchKind::kExternal:
    return v2::DISPATCH_KIND_EXTERNAL;
  case semantic::DispatchKind::kUnknown:
    return v2::DISPATCH_KIND_UNKNOWN;
  }
  return v2::DISPATCH_KIND_UNSPECIFIED;
}

v2::AliasKind ToV2AliasKind(semantic::AliasKind kind) {
  switch (kind) {
  case semantic::AliasKind::kMustAlias:
    return v2::ALIAS_KIND_MUST_ALIAS;
  case semantic::AliasKind::kMayAlias:
    return v2::ALIAS_KIND_MAY_ALIAS;
  case semantic::AliasKind::kNoAlias:
    return v2::ALIAS_KIND_NO_ALIAS;
  case semantic::AliasKind::kUnknownAlias:
    return v2::ALIAS_KIND_UNKNOWN_ALIAS;
  }
  return v2::ALIAS_KIND_UNSPECIFIED;
}

// semantic::MemoryEffectKind carries the may-ness independently of the
// epistemic state, so the v1::EffectKind projection keeps only read/write.
v1::EffectKind ToV1EffectKind(semantic::MemoryEffectKind kind) {
  switch (kind) {
  case semantic::MemoryEffectKind::kRead:
  case semantic::MemoryEffectKind::kMayRead:
    return v1::EFFECT_KIND_READ;
  case semantic::MemoryEffectKind::kWrite:
  case semantic::MemoryEffectKind::kMayWrite:
    return v1::EFFECT_KIND_WRITE;
  case semantic::MemoryEffectKind::kUnknown:
    return v1::EFFECT_KIND_UNKNOWN;
  }
  return v1::EFFECT_KIND_UNKNOWN;
}

v2::AbstractObjectKind ToV2ObjectKind(semantic::AbstractObjectKind kind) {
  switch (kind) {
  case semantic::AbstractObjectKind::kGlobal:
    return v2::ABSTRACT_OBJECT_KIND_GLOBAL;
  case semantic::AbstractObjectKind::kStack:
    return v2::ABSTRACT_OBJECT_KIND_STACK;
  case semantic::AbstractObjectKind::kHeap:
    return v2::ABSTRACT_OBJECT_KIND_HEAP;
  case semantic::AbstractObjectKind::kArgument:
    return v2::ABSTRACT_OBJECT_KIND_ARGUMENT;
  case semantic::AbstractObjectKind::kFunction:
    return v2::ABSTRACT_OBJECT_KIND_FUNCTION;
  case semantic::AbstractObjectKind::kExternal:
    return v2::ABSTRACT_OBJECT_KIND_EXTERNAL;
  case semantic::AbstractObjectKind::kUnknown:
    return v2::ABSTRACT_OBJECT_KIND_UNKNOWN;
  case semantic::AbstractObjectKind::kLegacyOpaque:
    return v2::ABSTRACT_OBJECT_KIND_LEGACY_OPAQUE;
  }
  return v2::ABSTRACT_OBJECT_KIND_UNSPECIFIED;
}

v2::AccessPathSegment::Kind ToV2SegmentKind(
    semantic::AccessPathSegment::Kind kind) {
  switch (kind) {
  case semantic::AccessPathSegment::Kind::kField:
    return v2::AccessPathSegment::KIND_FIELD;
  case semantic::AccessPathSegment::Kind::kArrayIndex:
    return v2::AccessPathSegment::KIND_ARRAY_INDEX;
  case semantic::AccessPathSegment::Kind::kArrayRange:
    return v2::AccessPathSegment::KIND_ARRAY_RANGE;
  case semantic::AccessPathSegment::Kind::kUnknown:
    return v2::AccessPathSegment::KIND_UNKNOWN;
  }
  return v2::AccessPathSegment::KIND_UNSPECIFIED;
}

StatusOr<v2::MemoryLocation> ToProtoLocation(
    const semantic::MemoryLocation& location) {
  if (Status status = semantic::Validate(location); !status.ok()) {
    return status;
  }
  v2::MemoryLocation out;
  out.set_memory_location_id(core::ToString(location.id));
  auto* object = out.mutable_object();
  object->set_abstract_object_id(core::ToString(location.object.id));
  object->set_kind(ToV2ObjectKind(location.object.kind));
  if (location.object.owner_function.has_value()) {
    object->set_owner_function_variant_id(
        core::ToString(*location.object.owner_function));
  }
  object->set_semantic_anchor_id(location.object.semantic_anchor);
  object->set_diagnostic_name(location.object.diagnostic_name);
  for (const auto& segment : location.access_path) {
    auto* proto_segment = out.add_access_path();
    proto_segment->set_kind(ToV2SegmentKind(segment.kind));
    proto_segment->set_first(segment.first);
    proto_segment->set_last(segment.last);
  }
  auto* byte_range = out.mutable_byte_range();
  if (location.byte_range.offset.has_value()) {
    byte_range->set_offset_known(true);
    byte_range->set_offset(*location.byte_range.offset);
  }
  if (location.byte_range.size.has_value()) {
    byte_range->set_size_known(true);
    byte_range->set_size(*location.byte_range.size);
  }
  return out;
}

std::string_view ModelEffectKindToken(semantic::ModelEffectKind kind) {
  switch (kind) {
  case semantic::ModelEffectKind::kRead:
    return "read";
  case semantic::ModelEffectKind::kWrite:
    return "write";
  case semantic::ModelEffectKind::kAllocate:
    return "allocate";
  case semantic::ModelEffectKind::kDeallocate:
    return "deallocate";
  case semantic::ModelEffectKind::kUnknown:
    return "unknown";
  }
  return "unknown";
}

// Reattach component digests over the merged summary so the content hash and
// per-component hashes reflect the post-merge semantic content.
void ReplaceComponentDigests(v2::FunctionSummary* summary) {
  summary->clear_component_digests();
  for (const auto& digest : summary::ComputeComponentDigests(*summary)) {
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
}

template <typename Message>
void SortBySerialized(std::vector<Message>* items) {
  std::sort(items->begin(), items->end(),
            [](const Message& a, const Message& b) {
              return a.SerializeAsString() < b.SerializeAsString();
            });
}

// Sort a proto repeated field in place by serialized bytes so merged facts
// interleave deterministically with the local facts already present.
template <typename Message>
void SortRepeated(::google::protobuf::RepeatedPtrField<Message>* field) {
  std::vector<Message> items(field->begin(), field->end());
  SortBySerialized(&items);
  field->Clear();
  for (auto& item : items) {
    *field->Add() = std::move(item);
  }
}

}  // namespace

StatusOr<std::vector<v2::FunctionSummary>> MergeSvfFactsV2(
    std::vector<v2::FunctionSummary> drafts,
    const semantic::NormalizedAnalysisFacts& facts,
    const semantic::ModelBundle& model_bundle) {
  // Function value ref -> function-variant ID. Built once from the drafts so
  // indirect-call callees can be resolved to their function-variant identity
  // (a callee kValueRef is a hash, so it must be recomputed from a known
  // function-variant ID and looked up).
  std::map<StableId, std::string> ref_to_variant;
  for (const auto& draft : drafts) {
    const std::string& owner = draft.identity().function_variant_id();
    ref_to_variant.emplace(
        ::veritas::analysis::llvm::StableValueMapper::FunctionValueRef(owner),
        owner);
  }

  for (auto& draft : drafts) {
    const std::string& owner = draft.identity().function_variant_id();
    const StableId owner_ref =
        ::veritas::analysis::llvm::StableValueMapper::FunctionValueRef(owner);

    // Calls: attribute via the caller's function value ref.
    for (const auto& call : facts.calls) {
      if (call.caller != owner_ref) {
        continue;
      }
      auto* out = draft.add_calls();
      out->set_call_site_id(core::ToString(call.call_site));
      if (call.callee.has_value()) {
        const auto it = ref_to_variant.find(*call.callee);
        if (it != ref_to_variant.end()) {
          out->set_resolved_callee_function_variant_id(it->second);
          out->set_callee_symbol(it->second);
        } else {
          // Callee is outside this module (e.g. an external target); keep the
          // stable value-ref string as a deterministic diagnostic symbol.
          out->set_callee_symbol(core::ToString(*call.callee));
        }
      }
      out->set_dispatch(ToV2Dispatch(call.dispatch));
      out->set_epistemic(ToV1Epistemic(call.epistemic));
      out->set_provenance_ref(call.provenance_ref);
    }

    // Memory effects: attribute via location.object.owner_function.
    for (const auto& effect : facts.memory_effects) {
      const auto& owner_fn = effect.location.object.owner_function;
      if (!owner_fn.has_value() || core::ToString(*owner_fn) != owner) {
        continue;
      }
      auto location = ToProtoLocation(effect.location);
      if (!location.ok()) {
        return location.status();
      }
      auto* out = draft.add_memory_effects();
      out->set_kind(ToV1EffectKind(effect.kind));
      *out->mutable_location() = std::move(*location);
      out->set_epistemic(ToV1Epistemic(effect.epistemic));
      out->set_provenance_ref(effect.provenance_ref);
    }

    // Aliases: attribute only when BOTH endpoints are owned by this function;
    // a cross-function alias is a whole-program fact and stays gated out.
    for (const auto& alias : facts.aliases) {
      const auto& left_owner = alias.left.object.owner_function;
      const auto& right_owner = alias.right.object.owner_function;
      if (!left_owner.has_value() || !right_owner.has_value() ||
          core::ToString(*left_owner) != owner ||
          core::ToString(*right_owner) != owner) {
        continue;
      }
      auto left = ToProtoLocation(alias.left);
      auto right = ToProtoLocation(alias.right);
      if (!left.ok()) {
        return left.status();
      }
      if (!right.ok()) {
        return right.status();
      }
      auto* out = draft.add_alias_facts();
      *out->mutable_left() = std::move(*left);
      *out->mutable_right() = std::move(*right);
      out->set_kind(ToV2AliasKind(alias.kind));
      out->set_epistemic(ToV1Epistemic(alias.epistemic));
      out->set_provenance_ref(alias.provenance_ref);
    }

    // Run-level unknowns attach to every draft, matching the V1 merge.
    for (const auto& unknown : facts.unknowns) {
      auto* out = draft.add_unknowns();
      out->set_kind("svf");
      out->set_reason(unknown.reason);
      out->set_scope(unknown.scope);
      out->set_provenance_ref(unknown.provenance_ref);
    }

    // Model effects are NOT merged into the summary here. summary.v2 has no
    // model-effect field and LogicalInputHash does not exist yet, so the model
    // bundle is only loaded/validated/hashed in this milestone (its hash
    // fingerprint is available to downstream stages). Model-effect
    // materialization (ModeledEffect rows) and the LogicalInputHash threading
    // are deferred to M8R.3 / Task 10's WpaInputMaterializer. The loop below
    // therefore never fires for external models (whose functions have no local
    // summary); it only exists so that, should a model ever name a *local*
    // function, the effect is recorded as a dependency rather than dropped.
    for (const auto& model : model_bundle.Lookup(owner)) {
      auto* out = draft.add_dependencies();
      out->set_symbol(model.symbol);
      out->set_kind(std::string("model:") +
                    std::string(ModelEffectKindToken(model.effect)));
      out->set_provenance_ref(core::ToString(model.model_id));
    }

    // Re-sort every merged component and recompute component digests.
    SortRepeated(draft.mutable_calls());
    SortRepeated(draft.mutable_memory_effects());
    SortRepeated(draft.mutable_alias_facts());
    SortRepeated(draft.mutable_unknowns());
    SortRepeated(draft.mutable_dependencies());
    ReplaceComponentDigests(&draft);
  }

  return drafts;
}

}  // namespace veritas::analysis::svf
