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

#include "veritas/wpa/RelationIo.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

#include "veritas/facts/RelationSchema.h"
#include "veritas/facts/SemanticKeyCodec.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

constexpr char kDelimiter = '\t';

bool HasStructuralCharacter(std::string_view value) {
  return value.find(kDelimiter) != std::string_view::npos ||
         value.find('\n') != std::string_view::npos ||
         value.find('\r') != std::string_view::npos;
}

StatusOr<std::string> CellText(const facts::ExecutionCellValue& cell) {
  std::string text;
  Status status = Status::Ok();
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
          if (HasStructuralCharacter(value)) {
            status = Status::InvalidArgument(
                "symbol cell contains a delimiter or newline");
            return;
          }
          text = value;
        } else if constexpr (std::is_same_v<T, std::int64_t> ||
                             std::is_same_v<T, std::uint64_t>) {
          text = std::to_string(value);
        } else if constexpr (std::is_same_v<T, facts::FunctionId> ||
                             std::is_same_v<T, facts::ValueId> ||
                             std::is_same_v<T, facts::MemoryId> ||
                             std::is_same_v<T, facts::CallSiteId> ||
                             std::is_same_v<T, facts::FactId>) {
          text = std::to_string(value.value);
        } else {
          // The typed semantic enums travel as their ordinal, which is the
          // encoding relations.v2.dl declares.
          text = std::to_string(static_cast<std::uint64_t>(value));
        }
      },
      cell);
  if (!status.ok())
    return status;
  return text;
}

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

std::vector<std::string> SplitRow(std::string_view line) {
  std::vector<std::string> cells;
  std::size_t start = 0;
  while (true) {
    const std::size_t next = line.find(kDelimiter, start);
    if (next == std::string_view::npos) {
      cells.emplace_back(line.substr(start));
      return cells;
    }
    cells.emplace_back(line.substr(start, next - start));
    start = next + 1;
  }
}

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

// Rebuilds a semantic row from a decoded key. The schema decides how each
// field is interpreted, so a key whose field tags disagree with the relation
// is rejected rather than coerced.
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

}  // namespace

Status RelationIo::WriteInput(const std::filesystem::path& directory,
                              const WpaLogicalComponentInput& input) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return Status::Internal("cannot create relation directory");
  }

  std::map<facts::RelationId, std::string> files;
  for (const auto& row : input.edb) {
    const auto& schema = facts::RelationsV2().Get(row.relation);
    if (row.cells.size() != schema.columns.size()) {
      return Status::InvalidArgument("execution row does not match its schema");
    }
    std::string line;
    for (std::size_t i = 0; i < row.cells.size(); ++i) {
      auto text = CellText(row.cells[i]);
      if (!text.ok())
        return text.status();
      if (i > 0)
        line.push_back(kDelimiter);
      line.append(*text);
    }
    line.push_back('\n');
    files[row.relation].append(line);
  }

  // Every EDB (input) relation must have a facts file, even when the component
  // produced no rows for it: the compiled bundle's .input directive loads each
  // input relation unconditionally, so an absent file fails the run. Empty
  // relations for the other component are simply not read by the bundle.
  for (std::size_t i = 0; i < facts::kRelationCountV2; ++i) {
    const auto id = static_cast<facts::RelationId>(i);
    const auto& schema = facts::RelationsV2().Get(id);
    if (schema.ownership == facts::RelationOwnership::kEdb) {
      files.try_emplace(id);
    }
  }

  for (const auto& [relation, contents] : files) {
    const auto& schema = facts::RelationsV2().Get(relation);
    std::ofstream stream(directory / (schema.name + ".facts"),
                         std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
      return Status::Internal("cannot open relation file for writing");
    stream << contents;
    if (!stream.good())
      return Status::Internal("failed writing relation file");
  }
  return Status::Ok();
}

StatusOr<facts::RawWpaEvaluation> RelationIo::ReadOutput(
    const std::filesystem::path& directory,
    const WpaLogicalComponentInput& input) {
  const bool memory = input.component == WpaComponentKind::kMemoryEffects;
  const facts::RelationId derived =
      memory ? facts::RelationId::kMayWrite : facts::RelationId::kReachableCall;
  const auto& derived_schema = facts::RelationsV2().Get(derived);

  facts::RawWpaEvaluation raw;

  // Results arrive dense. Map every id back before it can become a fact.
  std::ifstream results(directory / (derived_schema.name + ".csv"));
  if (!results.is_open()) {
    return Status::Internal("Souffle worker did not produce the result file");
  }
  std::string line;
  while (std::getline(results, line)) {
    if (line.empty())
      continue;
    const auto cells = SplitRow(line);
    if (cells.size() != derived_schema.columns.size()) {
      return Status::InvalidArgument("result row does not match its schema");
    }
    facts::SemanticRow row;
    row.relation = derived;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      auto ordinal = ParseUnsigned(cells[i]);
      if (!ordinal.ok())
        return ordinal.status();
      switch (derived_schema.columns[i].domain) {
      case facts::ColumnDomain::kFunctionId: {
        auto stable = input.mappings.functions.ToStable(
            facts::FunctionId{static_cast<std::uint32_t>(*ordinal)});
        if (!stable.ok())
          return stable.status();
        row.cells.push_back(*stable);
        break;
      }
      case facts::ColumnDomain::kMemoryId: {
        auto stable = input.mappings.memories.ToStable(
            facts::MemoryId{static_cast<std::uint32_t>(*ordinal)});
        if (!stable.ok())
          return stable.status();
        row.cells.push_back(*stable);
        break;
      }
      default:
        row.cells.push_back(static_cast<sem::EpistemicState>(*ordinal));
        break;
      }
    }
    auto valid = facts::ValidateSemanticRow(row);
    if (!valid.ok())
      return valid;
    raw.results.push_back(std::move(row));
  }
  if (results.bad())
    return Status::Internal("failed reading the Souffle result file");

  std::ifstream witnesses(directory / "Witness.csv");
  if (!witnesses.is_open()) {
    return Status::Internal("Souffle worker did not produce Witness.csv");
  }
  while (std::getline(witnesses, line)) {
    if (line.empty())
      continue;
    const auto cells = SplitRow(line);
    if (cells.size() != 4) {
      return Status::InvalidArgument("witness row must have four columns");
    }
    auto result_row = RowFromKey(cells[0]);
    if (!result_row.ok())
      return result_row.status();
    auto input_row = RowFromKey(cells[2]);
    if (!input_row.ok())
      return input_row.status();
    auto ordinal = ParseUnsigned(cells[3]);
    if (!ordinal.ok())
      return ordinal.status();

    raw.witnesses.push_back(facts::WitnessEdge{
        .result = facts::SemanticKey{std::move(*result_row)},
        .rule_id = cells[1],
        .input = facts::SemanticKey{std::move(*input_row)},
        .input_ordinal = static_cast<std::uint32_t>(*ordinal)});
  }
  if (witnesses.bad())
    return Status::Internal("failed reading Witness.csv");
  return raw;
}

}  // namespace veritas::wpa
