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

#include "veritas/facts/FactSchema.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace veritas::facts {
namespace {

namespace v1 = summary::v1;

bool IsPositive(v1::EpistemicState state) {
  return state == v1::EPISTEMIC_STATE_MUST || state == v1::EPISTEMIC_STATE_MAY;
}

bool IsCanonicalStableId(const core::StableId &id, core::IdKind kind) {
  return id.kind == kind && core::ParseStableId(core::ToString(id)).ok();
}

bool IsBaseRelation(FactRelation relation) {
  switch (relation) {
  case FactRelation::kDirectCall:
  case FactRelation::kDirectRead:
  case FactRelation::kDirectWrite:
  case FactRelation::kLocalFlow:
  case FactRelation::kMayAlias:
    return true;
  case FactRelation::kReachableCall:
  case FactRelation::kMayWrite:
  case FactRelation::kGlobalFlow:
    return false;
  }
  return false;
}

bool HasControlCharacter(std::string_view value) {
  return value.find_first_of("\t\r\n") != std::string_view::npos;
}

void AppendField(std::string *output, std::string_view value) {
  output->append(std::to_string(value.size()));
  output->push_back(':');
  output->append(value);
}

Status ValidateColumns(FactRelation relation,
                       const std::vector<std::string> &columns) {
  auto arity = FactRelationArity(relation);
  if (!arity.ok())
    return arity.status();
  if (columns.size() != *arity) {
    return Status::InvalidArgument("fact relation has the wrong arity");
  }
  for (const auto &column : columns) {
    if (column.empty()) {
      return Status::InvalidArgument("fact columns must not be empty");
    }
    if (HasControlCharacter(column)) {
      return Status::InvalidArgument(
          "fact columns must not contain tab, CR, or LF");
    }
  }
  return Status::Ok();
}

core::StableId FactId(std::string_view canonical) {
  return core::MakeStableId(
      core::IdKind::kFact,
      std::as_bytes(std::span(canonical.data(), canonical.size())));
}

} // namespace

StatusOr<std::string_view> FactRelationName(FactRelation relation) {
  switch (relation) {
  case FactRelation::kDirectCall:
    return std::string_view{"DirectCall"};
  case FactRelation::kDirectRead:
    return std::string_view{"DirectRead"};
  case FactRelation::kDirectWrite:
    return std::string_view{"DirectWrite"};
  case FactRelation::kLocalFlow:
    return std::string_view{"LocalFlow"};
  case FactRelation::kMayAlias:
    return std::string_view{"MayAlias"};
  case FactRelation::kReachableCall:
    return std::string_view{"ReachableCall"};
  case FactRelation::kMayWrite:
    return std::string_view{"MayWrite"};
  case FactRelation::kGlobalFlow:
    return std::string_view{"GlobalFlow"};
  }
  return Status::InvalidArgument("unknown fact relation");
}

StatusOr<FactRelation> ParseFactRelation(std::string_view name) {
  if (name == "DirectCall")
    return FactRelation::kDirectCall;
  if (name == "DirectRead")
    return FactRelation::kDirectRead;
  if (name == "DirectWrite")
    return FactRelation::kDirectWrite;
  if (name == "LocalFlow")
    return FactRelation::kLocalFlow;
  if (name == "MayAlias")
    return FactRelation::kMayAlias;
  if (name == "ReachableCall")
    return FactRelation::kReachableCall;
  if (name == "MayWrite")
    return FactRelation::kMayWrite;
  if (name == "GlobalFlow")
    return FactRelation::kGlobalFlow;
  return Status::InvalidArgument("unknown fact relation name");
}

StatusOr<std::size_t> FactRelationArity(FactRelation relation) {
  switch (relation) {
  case FactRelation::kDirectCall:
  case FactRelation::kDirectRead:
  case FactRelation::kDirectWrite:
  case FactRelation::kMayAlias:
  case FactRelation::kReachableCall:
  case FactRelation::kMayWrite:
  case FactRelation::kGlobalFlow:
    return 2u;
  case FactRelation::kLocalFlow:
    return 3u;
  }
  return Status::InvalidArgument("unknown fact relation");
}

StatusOr<v1::EpistemicState> WeakenPositiveEpistemic(v1::EpistemicState left,
                                                     v1::EpistemicState right) {
  if (!IsPositive(left) || !IsPositive(right)) {
    return Status::InvalidArgument("epistemic join requires MUST or MAY");
  }
  return left == v1::EPISTEMIC_STATE_MAY || right == v1::EPISTEMIC_STATE_MAY
             ? v1::EPISTEMIC_STATE_MAY
             : v1::EPISTEMIC_STATE_MUST;
}

StatusOr<FactTuple> MakeBaseFact(FactRelation relation,
                                 std::vector<std::string> columns,
                                 v1::EpistemicState epistemic,
                                 BaseFactOrigin origin) {
  auto name = FactRelationName(relation);
  if (!name.ok())
    return name.status();
  if (!IsBaseRelation(relation)) {
    return Status::InvalidArgument("base fact requires a base relation");
  }
  auto column_status = ValidateColumns(relation, columns);
  if (!column_status.ok())
    return column_status;
  if (!IsPositive(epistemic)) {
    return Status::InvalidArgument("base fact requires MUST or MAY");
  }
  if (!IsCanonicalStableId(origin.function_summary_id,
                           core::IdKind::kFunctionSummary)) {
    return Status::InvalidArgument(
        "base fact origin requires a function-summary ID");
  }
  if (HasControlCharacter(origin.anchor) ||
      HasControlCharacter(origin.provenance_ref)) {
    return Status::InvalidArgument(
        "base fact origin must not contain tab, CR, or LF");
  }

  std::string canonical;
  AppendField(&canonical, "veritas.fact.base.v1");
  AppendField(&canonical, *name);
  for (const auto &column : columns)
    AppendField(&canonical, column);
  AppendField(&canonical, std::to_string(static_cast<int>(epistemic)));
  AppendField(&canonical, core::ToString(origin.function_summary_id));
  AppendField(&canonical, origin.anchor);
  AppendField(&canonical, origin.provenance_ref);

  FactTuple tuple{.tuple_id = FactId(canonical),
                  .relation = relation,
                  .columns = std::move(columns),
                  .epistemic = epistemic,
                  .rule_id = {},
                  .input_tuple_ids = {}};
  auto status = ValidateFactTuple(tuple);
  if (!status.ok())
    return status;
  return tuple;
}

StatusOr<FactTuple>
MakeDerivedFact(FactRelation relation, std::vector<std::string> columns,
                v1::EpistemicState epistemic, std::string rule_id,
                std::vector<core::StableId> input_tuple_ids) {
  auto name = FactRelationName(relation);
  if (!name.ok())
    return name.status();
  if (IsBaseRelation(relation)) {
    return Status::InvalidArgument("derived fact requires a derived relation");
  }
  auto column_status = ValidateColumns(relation, columns);
  if (!column_status.ok())
    return column_status;
  if (!IsPositive(epistemic)) {
    return Status::InvalidArgument("derived fact requires MUST or MAY");
  }
  if (rule_id.empty() || HasControlCharacter(rule_id)) {
    return Status::InvalidArgument(
        "derived fact requires a valid non-empty rule ID");
  }
  if (input_tuple_ids.empty()) {
    return Status::InvalidArgument(
        "derived fact requires immediate input tuple IDs");
  }
  for (const auto &input_id : input_tuple_ids) {
    if (!IsCanonicalStableId(input_id, core::IdKind::kFact)) {
      return Status::InvalidArgument("derived input requires a fact ID");
    }
  }
  std::sort(input_tuple_ids.begin(), input_tuple_ids.end());

  std::string canonical;
  AppendField(&canonical, "veritas.fact.derived.v1");
  AppendField(&canonical, *name);
  for (const auto &column : columns)
    AppendField(&canonical, column);
  AppendField(&canonical, std::to_string(static_cast<int>(epistemic)));
  AppendField(&canonical, rule_id);
  for (const auto &input_id : input_tuple_ids) {
    AppendField(&canonical, core::ToString(input_id));
  }

  FactTuple tuple{.tuple_id = FactId(canonical),
                  .relation = relation,
                  .columns = std::move(columns),
                  .epistemic = epistemic,
                  .rule_id = std::move(rule_id),
                  .input_tuple_ids = std::move(input_tuple_ids)};
  auto status = ValidateFactTuple(tuple);
  if (!status.ok())
    return status;
  return tuple;
}

Status ValidateFactTuple(const FactTuple &tuple) {
  auto columns_status = ValidateColumns(tuple.relation, tuple.columns);
  if (!columns_status.ok())
    return columns_status;
  if (!IsCanonicalStableId(tuple.tuple_id, core::IdKind::kFact)) {
    return Status::InvalidArgument("tuple ID must be a fact ID");
  }
  if (!IsPositive(tuple.epistemic)) {
    return Status::InvalidArgument("fact requires MUST or MAY");
  }
  if (IsBaseRelation(tuple.relation)) {
    if (!tuple.rule_id.empty() || !tuple.input_tuple_ids.empty()) {
      return Status::InvalidArgument(
          "base fact must not carry derived provenance");
    }
    return Status::Ok();
  }
  if (tuple.rule_id.empty() || HasControlCharacter(tuple.rule_id) ||
      tuple.input_tuple_ids.empty()) {
    return Status::InvalidArgument(
        "derived fact requires rule and immediate inputs");
  }
  if (!std::is_sorted(tuple.input_tuple_ids.begin(),
                      tuple.input_tuple_ids.end())) {
    return Status::InvalidArgument("derived input tuple IDs must be sorted");
  }
  for (const auto &input_id : tuple.input_tuple_ids) {
    if (!IsCanonicalStableId(input_id, core::IdKind::kFact)) {
      return Status::InvalidArgument("derived input requires a fact ID");
    }
  }
  return Status::Ok();
}

} // namespace veritas::facts
