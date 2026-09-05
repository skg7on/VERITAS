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

#include "veritas/facts/FactProto.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace veritas::facts {

namespace {

namespace sem = analysis::semantic;
namespace fp = fact_proto;

// --- Enum conversions -----------------------------------------------------

fp::EpistemicState ToProtoEpistemic(sem::EpistemicState s) {
  switch (s) {
    case sem::EpistemicState::kMust:
      return fp::EPISTEMIC_MUST;
    case sem::EpistemicState::kMay:
      return fp::EPISTEMIC_MAY;
    case sem::EpistemicState::kMustNot:
      return fp::EPISTEMIC_MUST_NOT;
    case sem::EpistemicState::kInferred:
      return fp::EPISTEMIC_INFERRED;
    case sem::EpistemicState::kAssumed:
      return fp::EPISTEMIC_ASSUMED;
    case sem::EpistemicState::kUnknown:
      return fp::EPISTEMIC_UNKNOWN;
  }
  return fp::EPISTEMIC_UNSPECIFIED;
}

StatusOr<sem::EpistemicState> FromProtoEpistemic(fp::EpistemicState s) {
  switch (s) {
    case fp::EPISTEMIC_MUST:
      return sem::EpistemicState::kMust;
    case fp::EPISTEMIC_MAY:
      return sem::EpistemicState::kMay;
    case fp::EPISTEMIC_MUST_NOT:
      return sem::EpistemicState::kMustNot;
    case fp::EPISTEMIC_INFERRED:
      return sem::EpistemicState::kInferred;
    case fp::EPISTEMIC_ASSUMED:
      return sem::EpistemicState::kAssumed;
    case fp::EPISTEMIC_UNKNOWN:
      return sem::EpistemicState::kUnknown;
    case fp::EPISTEMIC_UNSPECIFIED:
    default:
      return Status::InvalidArgument("unrecognized epistemic state");
  }
}

fp::DispatchKind ToProtoDispatch(sem::DispatchKind k) {
  switch (k) {
    case sem::DispatchKind::kDirect:
      return fp::DISPATCH_DIRECT;
    case sem::DispatchKind::kIndirect:
      return fp::DISPATCH_INDIRECT;
    case sem::DispatchKind::kVirtual:
      return fp::DISPATCH_VIRTUAL;
    case sem::DispatchKind::kCallback:
      return fp::DISPATCH_CALLBACK;
    case sem::DispatchKind::kExternal:
      return fp::DISPATCH_EXTERNAL;
    case sem::DispatchKind::kUnknown:
      return fp::DISPATCH_UNKNOWN;
  }
  return fp::DISPATCH_UNSPECIFIED;
}

StatusOr<sem::DispatchKind> FromProtoDispatch(fp::DispatchKind k) {
  switch (k) {
    case fp::DISPATCH_DIRECT:
      return sem::DispatchKind::kDirect;
    case fp::DISPATCH_INDIRECT:
      return sem::DispatchKind::kIndirect;
    case fp::DISPATCH_VIRTUAL:
      return sem::DispatchKind::kVirtual;
    case fp::DISPATCH_CALLBACK:
      return sem::DispatchKind::kCallback;
    case fp::DISPATCH_EXTERNAL:
      return sem::DispatchKind::kExternal;
    case fp::DISPATCH_UNKNOWN:
      return sem::DispatchKind::kUnknown;
    case fp::DISPATCH_UNSPECIFIED:
    default:
      return Status::InvalidArgument("unrecognized dispatch kind");
  }
}

fp::AliasKind ToProtoAlias(sem::AliasKind k) {
  switch (k) {
    case sem::AliasKind::kMustAlias:
      return fp::ALIAS_MUST;
    case sem::AliasKind::kMayAlias:
      return fp::ALIAS_MAY;
    case sem::AliasKind::kNoAlias:
      return fp::ALIAS_NO;
    case sem::AliasKind::kUnknownAlias:
      return fp::ALIAS_UNKNOWN;
  }
  return fp::ALIAS_UNSPECIFIED;
}

StatusOr<sem::AliasKind> FromProtoAlias(fp::AliasKind k) {
  switch (k) {
    case fp::ALIAS_MUST:
      return sem::AliasKind::kMustAlias;
    case fp::ALIAS_MAY:
      return sem::AliasKind::kMayAlias;
    case fp::ALIAS_NO:
      return sem::AliasKind::kNoAlias;
    case fp::ALIAS_UNKNOWN:
      return sem::AliasKind::kUnknownAlias;
    case fp::ALIAS_UNSPECIFIED:
    default:
      return Status::InvalidArgument("unrecognized alias kind");
  }
}

fp::ByteRangeKind ToProtoByteRange(sem::ByteRangeKind k) {
  switch (k) {
    case sem::ByteRangeKind::kKnown:
      return fp::BYTE_RANGE_KNOWN;
    case sem::ByteRangeKind::kUnknown:
      return fp::BYTE_RANGE_UNKNOWN;
  }
  return fp::BYTE_RANGE_UNSPECIFIED;
}

StatusOr<sem::ByteRangeKind> FromProtoByteRange(fp::ByteRangeKind k) {
  switch (k) {
    case fp::BYTE_RANGE_KNOWN:
      return sem::ByteRangeKind::kKnown;
    case fp::BYTE_RANGE_UNKNOWN:
      return sem::ByteRangeKind::kUnknown;
    case fp::BYTE_RANGE_UNSPECIFIED:
    default:
      return Status::InvalidArgument("unrecognized byte range kind");
  }
}

// --- Cell conversion ------------------------------------------------------

void FillCell(fp::Cell* out, const SemanticCellValue& cell) {
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          out->set_stable_id(core::ToString(value));
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          out->set_int64_value(value);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          out->set_uint64_value(value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          out->set_string_value(value);
        } else if constexpr (std::is_same_v<T, sem::DispatchKind>) {
          out->set_dispatch_kind(ToProtoDispatch(value));
        } else if constexpr (std::is_same_v<T, sem::AliasKind>) {
          out->set_alias_kind(ToProtoAlias(value));
        } else if constexpr (std::is_same_v<T, sem::ByteRangeKind>) {
          out->set_byte_range_kind(ToProtoByteRange(value));
        } else if constexpr (std::is_same_v<T, sem::EpistemicState>) {
          out->set_epistemic(ToProtoEpistemic(value));
        }
      },
      cell);
}

StatusOr<SemanticCellValue> FromProtoCell(const fp::Cell& cell) {
  switch (cell.value_case()) {
    case fp::Cell::kStableId: {
      auto id = core::ParseStableId(cell.stable_id());
      if (!id.ok()) {
        return id.status();
      }
      return SemanticCellValue{*id};
    }
    case fp::Cell::kInt64Value:
      return SemanticCellValue{cell.int64_value()};
    case fp::Cell::kUint64Value:
      return SemanticCellValue{cell.uint64_value()};
    case fp::Cell::kStringValue:
      return SemanticCellValue{cell.string_value()};
    case fp::Cell::kDispatchKind: {
      auto k = FromProtoDispatch(cell.dispatch_kind());
      if (!k.ok()) {
        return k.status();
      }
      return SemanticCellValue{*k};
    }
    case fp::Cell::kAliasKind: {
      auto k = FromProtoAlias(cell.alias_kind());
      if (!k.ok()) {
        return k.status();
      }
      return SemanticCellValue{*k};
    }
    case fp::Cell::kByteRangeKind: {
      auto k = FromProtoByteRange(cell.byte_range_kind());
      if (!k.ok()) {
        return k.status();
      }
      return SemanticCellValue{*k};
    }
    case fp::Cell::kEpistemic: {
      auto k = FromProtoEpistemic(cell.epistemic());
      if (!k.ok()) {
        return k.status();
      }
      return SemanticCellValue{*k};
    }
    case fp::Cell::VALUE_NOT_SET:
    default:
      return Status::InvalidArgument("empty fact cell");
  }
}

}  // namespace

StatusOr<fact_proto::Fact> ToProtoFact(const AnalysisFact& fact) {
  // Re-derive the identity so a fact carrying an ID inconsistent with its
  // semantic row is rejected rather than serialized.
  auto expected = MakeFact(fact.row);
  if (!expected.ok()) {
    return expected.status();
  }
  if (expected->fact_id != fact.fact_id) {
    return Status::InvalidArgument("fact_id does not match its semantic row");
  }

  fact_proto::Fact proto;
  proto.set_fact_id(core::ToString(fact.fact_id));
  proto.set_relation_name(RelationsV2().Get(fact.row.relation).name);
  proto.set_relation_schema_version("relations.v2");
  for (const auto& cell : fact.row.cells) {
    FillCell(proto.add_cells(), cell);
  }
  return proto;
}

StatusOr<AnalysisFact> FromProtoFact(const fact_proto::Fact& proto) {
  auto stored_id = core::ParseStableId(proto.fact_id());
  if (!stored_id.ok()) {
    return stored_id.status();
  }
  if (stored_id->kind != core::IdKind::kFact) {
    return Status::InvalidArgument("fact_id has the wrong IdKind");
  }

  auto relation = RelationsV2().FindByName(proto.relation_name());
  if (!relation.has_value()) {
    return Status::InvalidArgument("unknown relation name");
  }

  SemanticRow row;
  row.relation = *relation;
  row.cells.reserve(static_cast<std::size_t>(proto.cells_size()));
  for (const auto& cell : proto.cells()) {
    auto converted = FromProtoCell(cell);
    if (!converted.ok()) {
      return converted.status();
    }
    row.cells.push_back(*converted);
  }

  auto fact = MakeFact(row);
  if (!fact.ok()) {
    return fact.status();
  }
  if (fact->fact_id != *stored_id) {
    return Status::InvalidArgument("fact_id does not match reconstructed row");
  }
  return fact;
}

}  // namespace veritas::facts
