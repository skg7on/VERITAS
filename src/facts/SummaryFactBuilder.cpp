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

#include "veritas/facts/SummaryFactBuilder.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/FunctionSummary.h"

namespace veritas::facts {
namespace {

namespace v1 = summary::v1;

bool IsPositive(v1::EpistemicState epistemic) {
  return epistemic == v1::EPISTEMIC_STATE_MUST ||
         epistemic == v1::EPISTEMIC_STATE_MAY;
}

Status AppendFact(StatusOr<FactTuple> fact, std::vector<FactTuple> *output) {
  if (!fact.ok())
    return fact.status();
  output->push_back(std::move(*fact));
  return Status::Ok();
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

StatusOr<std::vector<FactTuple>>
BuildBaseFacts(std::span<const v1::FunctionSummary> summaries) {
  std::vector<core::StableId> function_ids;
  function_ids.reserve(summaries.size());
  std::set<core::StableId> available_function_ids;
  for (const auto &function_summary : summaries) {
    auto function_id = ParseFunctionVariantId(
        function_summary.identity().function_variant_id(),
        "invalid base-fact summary identity");
    if (!function_id.ok())
      return function_id.status();
    available_function_ids.insert(*function_id);
    function_ids.push_back(std::move(*function_id));
  }

  std::vector<FactTuple> output;
  for (std::size_t i = 0; i < summaries.size(); ++i) {
    const auto &function_summary = summaries[i];
    const auto &function_id = function_ids[i];
    auto summary_id = summary::ComputeFunctionSummaryId(function_summary);
    if (!summary_id.ok())
      return summary_id.status();
    const std::string function_text = core::ToString(function_id);

    for (const auto &call : function_summary.calls()) {
      if (call.resolved_callee_function_variant_id().empty()) {
        continue;
      }
      auto callee =
          ParseFunctionVariantId(call.resolved_callee_function_variant_id(),
                                 "invalid resolved callee identity");
      if (!callee.ok())
        return callee.status();
      if (!IsPositive(call.epistemic()) ||
          !available_function_ids.contains(*callee)) {
        continue;
      }
      auto status = AppendFact(
          MakeBaseFact(
              FactRelation::kDirectCall,
              {function_text, core::ToString(*callee)}, call.epistemic(),
              {*summary_id, call.call_site_anchor_id(), call.provenance_ref()}),
          &output);
      if (!status.ok())
        return status;
    }

    for (const auto &effect : function_summary.memory_effects()) {
      if (!IsPositive(effect.epistemic()))
        continue;
      FactRelation relation;
      if (effect.kind() == v1::EFFECT_KIND_READ) {
        relation = FactRelation::kDirectRead;
      } else if (effect.kind() == v1::EFFECT_KIND_WRITE) {
        relation = FactRelation::kDirectWrite;
      } else {
        continue;
      }
      auto status = AppendFact(
          MakeBaseFact(
              relation, {function_text, effect.location()}, effect.epistemic(),
              {*summary_id, effect.location(), effect.provenance_ref()}),
          &output);
      if (!status.ok())
        return status;
    }

    for (const auto &flow : function_summary.value_flows()) {
      if (!IsPositive(flow.epistemic()))
        continue;
      auto status = AppendFact(
          MakeBaseFact(
              FactRelation::kLocalFlow,
              {flow.source(), flow.sink(), function_text}, flow.epistemic(),
              {*summary_id, "flow:" + flow.source() + ":" + flow.sink(),
               flow.provenance_ref()}),
          &output);
      if (!status.ok())
        return status;
    }

    for (const auto &alias : function_summary.alias_facts()) {
      if (!IsPositive(alias.epistemic()))
        continue;
      auto status = AppendFact(
          MakeBaseFact(
              FactRelation::kMayAlias, {alias.location_a(), alias.location_b()},
              alias.epistemic(),
              {*summary_id,
               "alias:" + alias.location_a() + ":" + alias.location_b(),
               alias.provenance_ref()}),
          &output);
      if (!status.ok())
        return status;
    }
  }

  std::ranges::sort(output, {}, &FactTuple::tuple_id);
  output.erase(std::unique(output.begin(), output.end(),
                           [](const FactTuple &left, const FactTuple &right) {
                             return left.tuple_id == right.tuple_id;
                           }),
               output.end());
  return output;
}

} // namespace veritas::facts
