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

#include "veritas/wpa/WpaRunRepository.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "veritas/facts/Witness.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

// ---- Minimal length-prefixed binary serialization for the cache object. ----
// This is an opaque, versioned cache format, not a durable public contract.

constexpr std::string_view kResultMagic = "veritas.wpa-result.v1";

void AppendString(std::string* out, std::string_view value) {
  std::uint64_t size = value.size();
  out->append(reinterpret_cast<const char*>(&size), sizeof(size));
  out->append(value);
}

void AppendU64(std::string* out, std::uint64_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void AppendU32(std::string* out, std::uint32_t value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(value));
}

// Serializes one semantic cell. The tag is the variant index; a stable ID is
// carried as its canonical text.
void AppendCell(std::string* out, const facts::SemanticCellValue& cell) {
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>) {
          AppendU32(out, 0);
          AppendString(out, core::ToString(value));
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          AppendU32(out, 1);
          AppendU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          AppendU32(out, 2);
          AppendU64(out, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          AppendU32(out, 3);
          AppendString(out, value);
        } else if constexpr (std::is_same_v<T, sem::DispatchKind>) {
          AppendU32(out, 4);
          AppendU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, sem::AliasKind>) {
          AppendU32(out, 5);
          AppendU64(out, static_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, sem::ByteRangeKind>) {
          AppendU32(out, 6);
          AppendU64(out, static_cast<std::uint64_t>(value));
        } else {
          AppendU32(out, 7);  // sem::EpistemicState
          AppendU64(out, static_cast<std::uint64_t>(value));
        }
      },
      cell);
}

void AppendRow(std::string* out, const facts::SemanticRow& row) {
  AppendU32(out, static_cast<std::uint32_t>(row.relation));
  AppendU32(out, static_cast<std::uint32_t>(row.cells.size()));
  for (const auto& cell : row.cells) {
    AppendCell(out, cell);
  }
}

void AppendFact(std::string* out, const facts::AnalysisFact& fact) {
  AppendString(out, core::ToString(fact.fact_id));
  AppendRow(out, fact.row);
}

void AppendWitness(std::string* out, const facts::WitnessEdge& edge) {
  AppendRow(out, edge.result.row);
  AppendString(out, edge.rule_id);
  AppendRow(out, edge.input.row);
  AppendU32(out, edge.input_ordinal);
}

std::string SerializeResult(const WpaComponentResult& result) {
  std::string out(kResultMagic);
  AppendString(&out, core::ToString(result.scc_id));
  AppendU32(&out, static_cast<std::uint32_t>(result.component));
  AppendString(&out, result.logical_input_hash);
  AppendString(&out, result.fixpoint_hash);
  AppendString(&out, result.external_hash);
  AppendU32(&out, static_cast<std::uint32_t>(result.facts.size()));
  for (const auto& fact : result.facts) {
    AppendFact(&out, fact);
  }
  AppendU32(&out, static_cast<std::uint32_t>(result.witnesses.size()));
  for (const auto& edge : result.witnesses) {
    AppendWitness(&out, edge);
  }
  AppendU32(&out, static_cast<std::uint32_t>(result.diagnostics.size()));
  for (const auto& diagnostic : result.diagnostics) {
    AppendString(&out, diagnostic);
  }
  return out;
}

// ---- Deserialization. ----

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  StatusOr<std::string_view> ReadString() {
    if (pos_ + sizeof(std::uint64_t) > data_.size()) {
      return Status::InvalidArgument("result cache object is truncated");
    }
    std::uint64_t size = 0;
    std::memcpy(&size, data_.data() + pos_, sizeof(size));
    pos_ += sizeof(size);
    if (size > data_.size() - pos_) {
      return Status::InvalidArgument("result cache object string overruns");
    }
    std::string_view value = data_.substr(pos_, size);
    pos_ += size;
    return value;
  }

  StatusOr<std::uint64_t> ReadU64() {
    if (pos_ + sizeof(std::uint64_t) > data_.size()) {
      return Status::InvalidArgument("result cache object is truncated");
    }
    std::uint64_t value = 0;
    std::memcpy(&value, data_.data() + pos_, sizeof(value));
    pos_ += sizeof(value);
    return value;
  }

  StatusOr<std::uint32_t> ReadU32() {
    auto value = ReadU64();
    if (!value.ok()) {
      return value.status();
    }
    return static_cast<std::uint32_t>(*value);
  }

 private:
  std::string_view data_;
  std::size_t pos_ = 0;
};

StatusOr<facts::SemanticCellValue> ReadCell(Reader* reader) {
  auto tag = reader->ReadU32();
  if (!tag.ok()) {
    return tag.status();
  }
  switch (*tag) {
    case 0: {
      auto text = reader->ReadString();
      if (!text.ok()) {
        return text.status();
      }
      auto id = core::ParseStableId(*text);
      if (!id.ok()) {
        return id.status();
      }
      return facts::SemanticCellValue{std::move(*id)};
    }
    case 1: {
      auto value = reader->ReadU64();
      if (!value.ok()) {
        return value.status();
      }
      return facts::SemanticCellValue{static_cast<std::int64_t>(*value)};
    }
    case 2: {
      auto value = reader->ReadU64();
      if (!value.ok()) {
        return value.status();
      }
      return facts::SemanticCellValue{*value};
    }
    case 3: {
      auto text = reader->ReadString();
      if (!text.ok()) {
        return text.status();
      }
      return facts::SemanticCellValue{std::string(*text)};
    }
    case 4:
    case 5:
    case 6:
    case 7: {
      auto value = reader->ReadU64();
      if (!value.ok()) {
        return value.status();
      }
      const auto ordinal = static_cast<std::uint8_t>(*value);
      switch (*tag) {
        case 4:
          return facts::SemanticCellValue{
              static_cast<sem::DispatchKind>(ordinal)};
        case 5:
          return facts::SemanticCellValue{
              static_cast<sem::AliasKind>(ordinal)};
        case 6:
          return facts::SemanticCellValue{
              static_cast<sem::ByteRangeKind>(ordinal)};
        default:
          return facts::SemanticCellValue{
              static_cast<sem::EpistemicState>(ordinal)};
      }
    }
    default:
      return Status::InvalidArgument("result cache cell has an unknown tag");
  }
}

StatusOr<facts::SemanticRow> ReadRow(Reader* reader) {
  auto relation = reader->ReadU32();
  if (!relation.ok()) {
    return relation.status();
  }
  auto count = reader->ReadU32();
  if (!count.ok()) {
    return count.status();
  }
  facts::SemanticRow row;
  row.relation = static_cast<facts::RelationId>(*relation);
  row.cells.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto cell = ReadCell(reader);
    if (!cell.ok()) {
      return cell.status();
    }
    row.cells.push_back(std::move(*cell));
  }
  return row;
}

StatusOr<facts::AnalysisFact> ReadFact(Reader* reader) {
  auto id_text = reader->ReadString();
  if (!id_text.ok()) {
    return id_text.status();
  }
  auto id = core::ParseStableId(*id_text);
  if (!id.ok()) {
    return id.status();
  }
  auto row = ReadRow(reader);
  if (!row.ok()) {
    return row.status();
  }
  facts::AnalysisFact fact;
  fact.fact_id = std::move(*id);
  fact.row = std::move(*row);
  return fact;
}

StatusOr<facts::WitnessEdge> ReadWitness(Reader* reader) {
  auto result = ReadRow(reader);
  if (!result.ok()) {
    return result.status();
  }
  auto rule = reader->ReadString();
  if (!rule.ok()) {
    return rule.status();
  }
  auto input = ReadRow(reader);
  if (!input.ok()) {
    return input.status();
  }
  auto ordinal = reader->ReadU32();
  if (!ordinal.ok()) {
    return ordinal.status();
  }
  facts::WitnessEdge edge;
  edge.result.row = std::move(*result);
  edge.rule_id = std::string(*rule);
  edge.input.row = std::move(*input);
  edge.input_ordinal = *ordinal;
  return edge;
}

StatusOr<WpaComponentResult> DeserializeResult(std::string_view data) {
  if (!data.starts_with(kResultMagic)) {
    return Status::InvalidArgument("result cache object has no magic");
  }
  Reader reader(data.substr(kResultMagic.size()));
  WpaComponentResult result;

  auto scc = reader.ReadString();
  if (!scc.ok()) {
    return scc.status();
  }
  auto scc_id = core::ParseStableId(*scc);
  if (!scc_id.ok()) {
    return scc_id.status();
  }
  result.scc_id = std::move(*scc_id);

  auto component = reader.ReadU32();
  if (!component.ok()) {
    return component.status();
  }
  result.component = static_cast<WpaComponentKind>(*component);

  auto logical = reader.ReadString();
  if (!logical.ok()) {
    return logical.status();
  }
  result.logical_input_hash = std::string(*logical);
  auto fixpoint = reader.ReadString();
  if (!fixpoint.ok()) {
    return fixpoint.status();
  }
  result.fixpoint_hash = std::string(*fixpoint);
  auto external = reader.ReadString();
  if (!external.ok()) {
    return external.status();
  }
  result.external_hash = std::string(*external);

  auto fact_count = reader.ReadU32();
  if (!fact_count.ok()) {
    return fact_count.status();
  }
  result.facts.reserve(*fact_count);
  for (std::uint32_t i = 0; i < *fact_count; ++i) {
    auto fact = ReadFact(&reader);
    if (!fact.ok()) {
      return fact.status();
    }
    result.facts.push_back(std::move(*fact));
  }

  auto witness_count = reader.ReadU32();
  if (!witness_count.ok()) {
    return witness_count.status();
  }
  result.witnesses.reserve(*witness_count);
  for (std::uint32_t i = 0; i < *witness_count; ++i) {
    auto edge = ReadWitness(&reader);
    if (!edge.ok()) {
      return edge.status();
    }
    result.witnesses.push_back(std::move(*edge));
  }

  auto diagnostic_count = reader.ReadU32();
  if (!diagnostic_count.ok()) {
    return diagnostic_count.status();
  }
  result.diagnostics.reserve(*diagnostic_count);
  for (std::uint32_t i = 0; i < *diagnostic_count; ++i) {
    auto diagnostic = reader.ReadString();
    if (!diagnostic.ok()) {
      return diagnostic.status();
    }
    result.diagnostics.emplace_back(*diagnostic);
  }
  return result;
}

std::vector<std::byte> ToBytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

}  // namespace

std::string DeriveResultCacheKey(const facts::AnalysisRunManifest& run,
                                 const WpaComponentKey& key,
                                 std::string_view logical_input_hash) {
  // Everything that identifies the exact result independent of revision and
  // run identity. The component key is covered by scc_id + component kind.
  std::string out;
  out += logical_input_hash;
  out += '|';
  out += core::ToString(key.scc_id);
  out += '|';
  out += std::to_string(static_cast<int>(key.component));
  out += '|';
  out += run.engine_toolchain_identity;
  out += '|';
  out += run.relation_schema_version;
  out += '|';
  out += run.rule_bundle_version;
  out += '|';
  out += run.model_bundle_version;
  return out;
}

WpaRunRepository::WpaRunRepository(
    summarydb::MetadataStore store,
    std::unique_ptr<summarydb::ObjectStore> results)
    : metadata_store_(std::move(store)),
      component_results_(std::move(results)) {}

WpaRunRepository::WpaRunRepository(WpaRunRepository&&) noexcept = default;
WpaRunRepository& WpaRunRepository::operator=(WpaRunRepository&&) noexcept =
    default;

StatusOr<WpaRunRepository> WpaRunRepository::Open(
    const std::filesystem::path& db_path) {
  auto store = summarydb::MetadataStore::Open(db_path);
  if (!store.ok()) {
    return store.status();
  }
  auto apply = store->ApplySchema();
  if (!apply.ok()) {
    return apply;
  }
  auto results = summarydb::CreateObjectStore(
      (db_path / "wpa-component-results").string());
  if (!results.ok()) {
    return results.status();
  }
  return WpaRunRepository(std::move(*store), std::move(*results));
}

Status WpaRunRepository::BeginRun(const facts::AnalysisRunManifest& run) {
  const std::string run_id = core::ToString(run.run_id);
  const std::string revision_id = core::ToString(run.revision_id);
  const std::string build_variant_id = core::ToString(run.build_variant_id);
  return metadata_store_.Execute(
      "INSERT OR IGNORE INTO wpa_analysis_runs "
      "(run_id, revision_id, build_variant_id, summary_schema_version, "
      " relation_schema_version, rule_bundle_version, model_bundle_version, "
      " svf_configuration_hash, wpa_configuration_hash, engine_identity, "
      " engine_toolchain_identity, status) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
      {run_id, revision_id, build_variant_id, run.summary_schema_version,
       run.relation_schema_version, run.rule_bundle_version,
       run.model_bundle_version, run.svf_configuration_hash,
       run.wpa_configuration_hash,
       std::to_string(static_cast<int>(run.engine)),
       run.engine_toolchain_identity,
       std::to_string(static_cast<int>(WpaRunStatus::kInProgress))});
}

StatusOr<std::optional<WpaComponentResult>> WpaRunRepository::LoadReusableComponent(
    const std::string& result_cache_key) {
  auto rows = metadata_store_.Query(
      "SELECT result_object_key, logical_input_hash, engine_toolchain_identity, "
      "relation_schema_version, rule_bundle_version, model_bundle_version, "
      "fixpoint_hash, external_hash "
      "FROM wpa_component_result_cache_v2 WHERE result_cache_key = ?",
      {result_cache_key});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->empty()) {
    return std::optional<WpaComponentResult>{};
  }
  const auto& row = (*rows)[0];
  if (row.size() != 8) {
    return Status::Internal("result cache row has an unexpected shape");
  }
  const std::string& object_key = row[0];
  auto bytes = component_results_->Get(object_key);
  if (!bytes.ok()) {
    return bytes.status();
  }
  auto result = DeserializeResult(
      std::string_view(reinterpret_cast<const char*>(bytes->data()),
                       bytes->size()));
  if (!result.ok()) {
    return result.status();
  }
  return std::optional<WpaComponentResult>(std::move(*result));
}

StatusOr<WpaComponentCompletion> WpaRunRepository::StoreSuccessfulComponent(
    const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
    const WpaComponentResult& result) {
  const std::string cache_key =
      DeriveResultCacheKey(run, key, result.logical_input_hash);
  const std::string serialized = SerializeResult(result);
  const auto bytes = ToBytes(serialized);

  Status put = component_results_->PutIfAbsent(cache_key, bytes);
  if (!put.ok()) {
    return put;
  }

  Status begin = metadata_store_.BeginTransaction();
  if (!begin.ok()) {
    return begin;
  }
  auto rollback = [&](Status s) {
    metadata_store_.RollbackTransaction();
    return s;
  };

  Status cache = metadata_store_.Execute(
      "INSERT OR IGNORE INTO wpa_component_result_cache_v2 "
      "(result_cache_key, logical_input_hash, engine_toolchain_identity, "
      " relation_schema_version, rule_bundle_version, model_bundle_version, "
      " result_object_key, fixpoint_hash, external_hash) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
      {cache_key, result.logical_input_hash, run.engine_toolchain_identity,
       run.relation_schema_version, run.rule_bundle_version,
       run.model_bundle_version, cache_key, result.fixpoint_hash,
       result.external_hash});
  if (!cache.ok()) {
    return rollback(cache);
  }

  Status state = metadata_store_.Execute(
      "INSERT OR REPLACE INTO wpa_component_states_v2 "
      "(run_id, scc_id, component_kind, logical_input_hash, fixpoint_hash, "
      " external_hash, result_cache_key, result_object_key, status, diagnostics) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
      {core::ToString(run.run_id), core::ToString(key.scc_id),
       std::to_string(static_cast<int>(key.component)),
       result.logical_input_hash, result.fixpoint_hash, result.external_hash,
       cache_key, cache_key,
       std::to_string(static_cast<int>(WpaComponentStatus::kSucceeded)), ""});
  if (!state.ok()) {
    return rollback(state);
  }

  Status commit = metadata_store_.CommitTransaction();
  if (!commit.ok()) {
    return commit;
  }

  WpaComponentCompletion completion;
  completion.key = key;
  completion.result_object_key = cache_key;
  completion.result = result;
  return completion;
}

Status WpaRunRepository::RecordComponentFailure(
    const facts::AnalysisRunManifest& run, const WpaComponentKey& key,
    std::string diagnostics) {
  return metadata_store_.Execute(
      "INSERT OR REPLACE INTO wpa_component_states_v2 "
      "(run_id, scc_id, component_kind, logical_input_hash, fixpoint_hash, "
      " external_hash, result_cache_key, result_object_key, status, diagnostics) "
      "VALUES (?, ?, ?, '', '', '', '', '', ?, ?)",
      {core::ToString(run.run_id), core::ToString(key.scc_id),
       std::to_string(static_cast<int>(key.component)),
       std::to_string(static_cast<int>(WpaComponentStatus::kFailed)),
       diagnostics});
}

Status WpaRunRepository::CompleteRun(const facts::AnalysisRunManifest& run) {
  return metadata_store_.Execute(
      "UPDATE wpa_analysis_runs SET status = ?, completed_at = strftime('%s', "
      "'now') WHERE run_id = ?",
      {std::to_string(static_cast<int>(WpaRunStatus::kComplete)),
       core::ToString(run.run_id)});
}

Status WpaRunRepository::MarkIncomplete(const facts::AnalysisRunManifest& run) {
  return metadata_store_.Execute(
      "UPDATE wpa_analysis_runs SET status = ? WHERE run_id = ?",
      {std::to_string(static_cast<int>(WpaRunStatus::kIncomplete)),
       core::ToString(run.run_id)});
}

StatusOr<WpaRunStatus> WpaRunRepository::RunStatus(core::StableId run_id) {
  auto rows = metadata_store_.Query(
      "SELECT status FROM wpa_analysis_runs WHERE run_id = ?",
      {core::ToString(run_id)});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->empty()) {
    return Status::NotFound("no WPA run with the given id");
  }
  const std::string& text = (*rows)[0][0];
  int value = 0;
  for (const char digit : text) {
    value = value * 10 + static_cast<int>(digit - '0');
  }
  return static_cast<WpaRunStatus>(value);
}

StatusOr<std::optional<std::string>> WpaRunRepository::ResultObjectKey(
    core::StableId run_id, const WpaComponentKey& key) {
  auto rows = metadata_store_.Query(
      "SELECT result_object_key FROM wpa_component_states_v2 "
      "WHERE run_id = ? AND scc_id = ? AND component_kind = ?",
      {core::ToString(run_id), core::ToString(key.scc_id),
       std::to_string(static_cast<int>(key.component))});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->empty()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>((*rows)[0][0]);
}

}  // namespace veritas::wpa
