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

#include <optional>
#include <string>
#include <string_view>

#include "analysis/llvm/OriginMap.h"
#include "veritas/core/Ids.h"

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

// semantic::MemoryEffectKind -> v1 proto EffectKind.
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
  // Normalized SVF facts carry stable IDs. Value-flow, alias, and
  // memory-effect facts are whole-program (no per-function owner is encoded in
  // a hash); calls carry their caller's diagnostic name and are attributed to
  // the caller's draft. Task 9 performs precise per-function attribution
  // against summary.v2.
  for (auto& draft : drafts) {
    const std::string& function_id = draft.identity().function_variant_id();

    for (const auto& flow : facts.value_flows) {
      auto* out = draft.add_value_flows();
      out->set_source(ToString(flow.source_value_id));
      out->set_sink(ToString(flow.destination_value_id));
      out->set_epistemic(ToV1Epistemic(flow.epistemic));
      out->set_provenance_ref(flow.provenance_ref);
    }

    for (const auto& alias : facts.aliases) {
      auto* out = draft.add_alias_facts();
      out->set_location_a(ToString(alias.left.id));
      out->set_location_b(ToString(alias.right.id));
      out->set_epistemic(ToV1Epistemic(alias.epistemic));
      out->set_provenance_ref(alias.provenance_ref);
    }

    for (const auto& effect : facts.memory_effects) {
      auto* out = draft.add_memory_effects();
      out->set_kind(ToV1EffectKind(effect.kind));
      out->set_location(ToString(effect.location.id));
      out->set_epistemic(ToV1Epistemic(effect.epistemic));
      out->set_provenance_ref(effect.provenance_ref);
    }

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

}  // namespace veritas::analysis::svf
