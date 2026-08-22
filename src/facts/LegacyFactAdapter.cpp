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

#include "veritas/facts/LegacyFactAdapter.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/summary/FunctionSummary.h"

namespace veritas::facts {

namespace {

namespace v1 = summary::v1;

semantic::EpistemicState MapEpistemic(v1::EpistemicState state) {
  switch (state) {
  case v1::EPISTEMIC_STATE_MUST:
    return semantic::EpistemicState::kMust;
  case v1::EPISTEMIC_STATE_MAY:
    return semantic::EpistemicState::kMay;
  case v1::EPISTEMIC_STATE_MUST_NOT:
    return semantic::EpistemicState::kMustNot;
  case v1::EPISTEMIC_STATE_INFERRED:
    return semantic::EpistemicState::kInferred;
  case v1::EPISTEMIC_STATE_ASSUMED:
    return semantic::EpistemicState::kAssumed;
  case v1::EPISTEMIC_STATE_UNKNOWN:
    return semantic::EpistemicState::kUnknown;
  case v1::EPISTEMIC_STATE_UNSPECIFIED:
  default:
    return semantic::EpistemicState::kUnknown;
  }
}

std::string_view EpistemicName(semantic::EpistemicState state) {
  switch (state) {
  case semantic::EpistemicState::kMust:
    return "MUST";
  case semantic::EpistemicState::kMay:
    return "MAY";
  case semantic::EpistemicState::kMustNot:
    return "MUST_NOT";
  case semantic::EpistemicState::kInferred:
    return "INFERRED";
  case semantic::EpistemicState::kAssumed:
    return "ASSUMED";
  case semantic::EpistemicState::kUnknown:
    return "UNKNOWN";
  }
  return "UNKNOWN";
}

StatusOr<core::StableId> ParseFunctionVariant(std::string_view text) {
  auto id = core::ParseStableId(text);
  if (!id.ok()) {
    return id.status();
  }
  if (id->kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument("expected function variant id");
  }
  return *id;
}

core::StableId StableIdFromText(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(text.data(), text.size())));
}

core::StableId MemoryRef(std::string_view text) {
  return StableIdFromText(core::IdKind::kMemoryRef, text);
}

core::StableId ValueRef(std::string_view text) {
  return StableIdFromText(core::IdKind::kValueRef, text);
}

core::StableId CallSiteRef(std::string_view text) {
  return StableIdFromText(core::IdKind::kCallSite, text);
}

bool RelationPermitsEpistemic(RelationId relation,
                              semantic::EpistemicState state) {
  switch (relation) {
  case RelationId::kLocalFlow:
    return state != semantic::EpistemicState::kMustNot;
  case RelationId::kUnknownCall:
    return state == semantic::EpistemicState::kMay ||
           state == semantic::EpistemicState::kUnknown;
  default:
    return true;
  }
}

void EmitUnsupported(LegacyProjection* projection, core::StableId origin,
                     std::string reason, semantic::EpistemicState state) {
  SemanticRow row;
  row.relation = RelationId::kUnsupportedFeature;
  row.cells = {core::ToString(origin), std::move(reason),
               std::string(EpistemicName(state))};
  projection->rows.push_back(std::move(row));
}

// Emits a relation row whose trailing epistemic column is appended here, or
// falls back to an explicit UnsupportedFeature row when the relation does not
// consume the state.
void EmitProjected(LegacyProjection* projection, RelationId relation,
                   std::vector<SemanticCellValue> cells,
                   semantic::EpistemicState state, core::StableId origin) {
  if (!RelationPermitsEpistemic(relation, state)) {
    EmitUnsupported(projection, origin, "legacy-epistemic-not-consumed", state);
    return;
  }
  cells.emplace_back(state);
  SemanticRow row;
  row.relation = relation;
  row.cells = std::move(cells);
  projection->rows.push_back(std::move(row));
}

}  // namespace

StatusOr<LegacyProjection> ProjectLegacyFacts(
    const AnalysisRunManifest& /*run*/, std::span<const FactTuple> facts) {
  LegacyProjection projection;
  for (const auto& fact : facts) {
    if (Status s = ValidateFactTuple(fact); !s.ok()) {
      return s;
    }
    const auto epistemic = MapEpistemic(fact.epistemic);

    switch (fact.relation) {
    case FactRelation::kDirectCall: {
      auto caller = ParseFunctionVariant(fact.columns[0]);
      auto callee = ParseFunctionVariant(fact.columns[1]);
      if (!caller.ok()) return caller.status();
      if (!callee.ok()) return callee.status();
      EmitProjected(&projection, RelationId::kDirectCall,
                    {CallSiteRef("legacy"), *caller, *callee,
                     semantic::DispatchKind::kDirect},
                    epistemic, fact.tuple_id);
      break;
    }
    case FactRelation::kDirectRead:
    case FactRelation::kDirectWrite: {
      auto function = ParseFunctionVariant(fact.columns[0]);
      if (!function.ok()) return function.status();
      const RelationId relation = fact.relation == FactRelation::kDirectRead
                                      ? RelationId::kDirectRead
                                      : RelationId::kDirectWrite;
      EmitProjected(&projection, relation,
                    {*function, MemoryRef(fact.columns[1]),
                     semantic::ByteRangeKind::kUnknown, std::int64_t{0},
                     std::uint64_t{0}},
                    epistemic, fact.tuple_id);
      break;
    }
    case FactRelation::kLocalFlow: {
      auto function = ParseFunctionVariant(fact.columns[2]);
      if (!function.ok()) return function.status();
      EmitProjected(&projection, RelationId::kLocalFlow,
                    {*function, ValueRef(fact.columns[0]),
                     ValueRef(fact.columns[1]), std::string("legacy-flow")},
                    epistemic, fact.tuple_id);
      break;
    }
    case FactRelation::kMayAlias: {
      EmitProjected(&projection, RelationId::kAlias,
                    {MemoryRef(fact.columns[0]), MemoryRef(fact.columns[1]),
                     semantic::AliasKind::kMayAlias},
                    epistemic, fact.tuple_id);
      break;
    }
    case FactRelation::kReachableCall: {
      auto source = ParseFunctionVariant(fact.columns[0]);
      auto target = ParseFunctionVariant(fact.columns[1]);
      if (!source.ok()) return source.status();
      if (!target.ok()) return target.status();
      EmitProjected(&projection, RelationId::kReachableCall, {*source, *target},
                    epistemic, fact.tuple_id);
      break;
    }
    case FactRelation::kMayWrite: {
      auto function = ParseFunctionVariant(fact.columns[0]);
      if (!function.ok()) return function.status();
      EmitProjected(&projection, RelationId::kMayWrite,
                    {*function, MemoryRef(fact.columns[1])}, epistemic,
                    fact.tuple_id);
      break;
    }
    case FactRelation::kGlobalFlow:
      // No V2 counterpart until M10A.
      EmitUnsupported(&projection, fact.tuple_id,
                      "legacy-relation-not-consumed", epistemic);
      break;
    }
  }
  return projection;
}

StatusOr<LegacyProjection> ProjectLegacySummaries(
    const AnalysisRunManifest& /*run*/,
    std::span<const v1::FunctionSummary> summaries) {
  LegacyProjection projection;
  for (const auto& summary : summaries) {
    auto caller = ParseFunctionVariant(summary.identity().function_variant_id());
    if (!caller.ok()) {
      return caller.status();
    }
    auto summary_id = summary::ComputeFunctionSummaryId(summary);
    if (!summary_id.ok()) {
      return summary_id.status();
    }

    for (const auto& call : summary.calls()) {
      const auto epistemic = MapEpistemic(call.epistemic());
      const auto call_site = CallSiteRef(call.call_site_anchor_id());
      if (!call.resolved_callee_function_variant_id().empty()) {
        auto callee =
            ParseFunctionVariant(call.resolved_callee_function_variant_id());
        if (!callee.ok()) {
          return callee.status();
        }
        EmitProjected(&projection, RelationId::kDirectCall,
                      {call_site, *caller, *callee,
                       semantic::DispatchKind::kDirect},
                      epistemic, *summary_id);
      } else {
        EmitProjected(&projection, RelationId::kUnknownCall,
                      {call_site, *caller,
                       std::string("no-resolved-callee")},
                      epistemic, *summary_id);
      }
    }

    for (const auto& effect : summary.memory_effects()) {
      const auto epistemic = MapEpistemic(effect.epistemic());
      const RelationId relation =
          effect.kind() == v1::EFFECT_KIND_READ   ? RelationId::kDirectRead
          : effect.kind() == v1::EFFECT_KIND_WRITE ? RelationId::kDirectWrite
                                                   : RelationId::kUnsupportedFeature;
      if (relation == RelationId::kUnsupportedFeature) {
        EmitUnsupported(&projection, *summary_id,
                        "legacy-effect-kind-not-consumed", epistemic);
        continue;
      }
      EmitProjected(&projection, relation,
                    {*caller, MemoryRef(effect.location()),
                     semantic::ByteRangeKind::kUnknown, std::int64_t{0},
                     std::uint64_t{0}},
                    epistemic, *summary_id);
    }

    for (const auto& flow : summary.value_flows()) {
      const auto epistemic = MapEpistemic(flow.epistemic());
      EmitProjected(&projection, RelationId::kLocalFlow,
                    {*caller, ValueRef(flow.source()), ValueRef(flow.sink()),
                     std::string("legacy-flow")},
                    epistemic, *summary_id);
    }

    for (const auto& alias : summary.alias_facts()) {
      const auto epistemic = MapEpistemic(alias.epistemic());
      EmitProjected(&projection, RelationId::kAlias,
                    {MemoryRef(alias.location_a()), MemoryRef(alias.location_b()),
                     semantic::AliasKind::kMayAlias},
                    epistemic, *summary_id);
    }

    for (const auto& unknown : summary.unknowns()) {
      SemanticRow row;
      row.relation = RelationId::kUnsupportedFeature;
      row.cells = {unknown.scope(), unknown.kind(), unknown.reason()};
      projection.rows.push_back(std::move(row));
    }

    for (const auto& assumption : summary.assumptions()) {
      SemanticRow row;
      row.relation = RelationId::kUnsupportedFeature;
      row.cells = {core::ToString(*summary_id), std::string("assumption"),
                   assumption.description()};
      projection.rows.push_back(std::move(row));
    }
  }
  return projection;
}

}  // namespace veritas::facts
