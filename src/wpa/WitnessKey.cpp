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

// WitnessKey.cpp — the one decoder from an encoded witness key to a semantic
// row. Every function here was moved verbatim out of RelationIo.cpp, which is
// the file path's reader: a witness key is durable evidence, so the file-backed
// reader and the in-memory session executor must decode it with the same code
// rather than with two copies kept in step by a test.

#include "WitnessKey.h"

#include <cstddef>
#include <string>
#include <vector>

#include "veritas/facts/RelationSchema.h"
#include "veritas/facts/SemanticKeyCodec.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

const facts::RelationSchema* SchemaByName(std::string_view name,
                                          facts::RelationId* out_id) {
  for (std::size_t i = 0; i < facts::kRelationCountV2; ++i) {
    const auto id = static_cast<facts::RelationId>(i);
    const auto& schema = facts::RelationsV2().Get(id);
    if (schema.name == name) {
      *out_id = id;
      return &schema;
    }
  }
  return nullptr;
}

}  // namespace

StatusOr<std::uint64_t> ParseUnsigned(std::string_view text) {
  if (text.empty())
    return Status::InvalidArgument("empty numeric cell");
  std::uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9')
      return Status::InvalidArgument("non-numeric cell");
    value = value * 10 + static_cast<std::uint64_t>(digit - '0');
  }
  return value;
}

StatusOr<std::int64_t> ParseSigned(std::string_view text) {
  const bool negative = !text.empty() && text.front() == '-';
  auto magnitude = ParseUnsigned(negative ? text.substr(1) : text);
  if (!magnitude.ok())
    return magnitude.status();
  return negative ? -static_cast<std::int64_t>(*magnitude)
                  : static_cast<std::int64_t>(*magnitude);
}

StatusOr<facts::SemanticRow> RowFromKey(std::string_view key) {
  auto decoded = facts::DecodeKey(key);
  if (!decoded.ok())
    return decoded.status();

  facts::RelationId id{};
  const facts::RelationSchema* schema =
      SchemaByName(decoded->relation_name, &id);
  if (schema == nullptr)
    return Status::InvalidArgument("key names an unknown relation");
  if (schema->columns.size() != decoded->cells.size())
    return Status::InvalidArgument("key arity does not match the schema");

  facts::SemanticRow row;
  row.relation = id;
  for (std::size_t i = 0; i < decoded->cells.size(); ++i) {
    const auto& field = decoded->cells[i];
    switch (schema->columns[i].domain) {
    case facts::ColumnDomain::kFunctionId:
    case facts::ColumnDomain::kValueId:
    case facts::ColumnDomain::kMemoryId:
    case facts::ColumnDomain::kCallSiteId:
    case facts::ColumnDomain::kFactId:
    case facts::ColumnDomain::kModelId: {
      if (field.tag != facts::KeyFieldTag::kId)
        return Status::InvalidArgument("key field is not an identifier");
      auto parsed = core::ParseStableId(field.value);
      if (!parsed.ok())
        return parsed.status();
      row.cells.push_back(*parsed);
      break;
    }
    case facts::ColumnDomain::kString: {
      if (field.tag != facts::KeyFieldTag::kSymbol)
        return Status::InvalidArgument("key field is not a symbol");
      row.cells.push_back(field.value);
      break;
    }
    case facts::ColumnDomain::kInt64: {
      auto parsed = ParseSigned(field.value);
      if (!parsed.ok())
        return parsed.status();
      row.cells.push_back(*parsed);
      break;
    }
    case facts::ColumnDomain::kUint64: {
      auto parsed = ParseUnsigned(field.value);
      if (!parsed.ok())
        return parsed.status();
      row.cells.push_back(*parsed);
      break;
    }
    default: {
      if (field.tag != facts::KeyFieldTag::kEnum)
        return Status::InvalidArgument("key field is not an enum");
      auto ordinal = ParseUnsigned(field.value);
      if (!ordinal.ok())
        return ordinal.status();
      switch (schema->columns[i].domain) {
      case facts::ColumnDomain::kDispatchKind:
        row.cells.push_back(static_cast<sem::DispatchKind>(*ordinal));
        break;
      case facts::ColumnDomain::kAliasKind:
        row.cells.push_back(static_cast<sem::AliasKind>(*ordinal));
        break;
      case facts::ColumnDomain::kByteRangeKind:
        row.cells.push_back(static_cast<sem::ByteRangeKind>(*ordinal));
        break;
      default:
        row.cells.push_back(static_cast<sem::EpistemicState>(*ordinal));
        break;
      }
      break;
    }
    }
  }
  auto valid = facts::ValidateSemanticRow(row);
  if (!valid.ok())
    return valid;
  return row;
}

}  // namespace veritas::wpa
