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

#include <string>

#include "analysis/llvm/OriginMap.h"
#include "analysis/svf/SvfFactMapper.h"

namespace veritas::analysis::svf {
namespace {

namespace v1 = veritas::summary::v1;

// SVF alias relationship string -> epistemic state.
v1::EpistemicState AliasEpistemic(const std::string& relationship) {
  if (relationship == "MUST_ALIAS") return v1::EPISTEMIC_STATE_MUST;
  if (relationship == "NO_ALIAS") return v1::EPISTEMIC_STATE_MUST_NOT;
  if (relationship == "UNKNOWN_ALIAS") return v1::EPISTEMIC_STATE_UNKNOWN;
  return v1::EPISTEMIC_STATE_MAY;  // MAY_ALIAS
}

// SVF memory-effect kind string -> proto EffectKind.
v1::EffectKind MemoryEffectKind(const std::string& kind) {
  if (kind == "READ") return v1::EFFECT_KIND_READ;
  if (kind == "WRITE") return v1::EFFECT_KIND_WRITE;
  return v1::EFFECT_KIND_UNKNOWN;
}

// SVF call kind string -> epistemic state.
v1::EpistemicState CallEpistemic(const std::string& kind) {
  if (kind == "MUST_CALL") return v1::EPISTEMIC_STATE_MUST;
  if (kind == "UNKNOWN_CALL") return v1::EPISTEMIC_STATE_UNKNOWN;
  return v1::EPISTEMIC_STATE_MAY;  // MAY_CALL
}

}  // namespace

std::vector<v1::FunctionSummary> MergeSvfFacts(
    std::vector<v1::FunctionSummary> drafts, const SvfFacts& facts,
    const ::veritas::analysis::llvm::OriginMap& origin_map) {
  // The placeholder SVF facts do not yet carry owning-function identity; value
  // names are LLVM names. Attribute each interprocedural fact to the draft
  // whose function name appears in one of its endpoints; append unknowns to
  // every draft so uncertainty is never silently dropped.
  for (auto& draft : drafts) {
    const std::string& function_name = draft.identity().function_variant_id();
    auto mentions = [&function_name](const std::string& name) {
      return name.find(function_name) != std::string::npos;
    };

    for (const auto& flow : facts.value_flows) {
      if (!mentions(flow.source.name) && !mentions(flow.destination.name)) {
        continue;
      }
      auto* out = draft.add_value_flows();
      out->set_source(flow.source.name);
      out->set_sink(flow.destination.name);
      out->set_epistemic(v1::EPISTEMIC_STATE_MAY);
      out->set_provenance_ref(flow.provenance);
    }

    for (const auto& alias : facts.aliases) {
      if (!mentions(alias.left.name) && !mentions(alias.right.name)) continue;
      auto* out = draft.add_alias_facts();
      out->set_location_a(alias.left.name);
      out->set_location_b(alias.right.name);
      out->set_epistemic(AliasEpistemic(alias.relationship));
      out->set_provenance_ref(alias.provenance);
    }

    for (const auto& effect : facts.refined_memory_effects) {
      if (!mentions(effect.memory.name) && !mentions(effect.operation.name)) {
        continue;
      }
      auto* out = draft.add_memory_effects();
      out->set_kind(MemoryEffectKind(effect.effect_kind));
      out->set_location(effect.memory.name);
      out->set_epistemic(v1::EPISTEMIC_STATE_MAY);
      out->set_provenance_ref(effect.provenance);
    }

    for (const auto& call : facts.refined_calls) {
      if (!mentions(call.callsite.name) && !mentions(call.target.name)) {
        continue;
      }
      auto* out = draft.add_calls();
      out->set_callee_symbol(call.target.name);
      out->set_call_site_anchor_id(call.callsite.name);
      out->set_epistemic(CallEpistemic(call.call_kind));
      out->set_provenance_ref(call.provenance);
      if (auto id = origin_map.GetSymbolIdByLlvmName(call.target.name)) {
        out->set_resolved_callee_function_variant_id(*id);
      }
    }

    for (const auto& unknown : facts.unknowns) {
      auto* out = draft.add_unknowns();
      out->set_kind("svf");
      out->set_reason(unknown.reason);
      out->set_scope(unknown.scope);
      out->set_provenance_ref(unknown.provenance);
    }
  }

  return drafts;
}

}  // namespace veritas::analysis::svf
