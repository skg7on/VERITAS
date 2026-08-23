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

}  // namespace veritas::analysis::svf
