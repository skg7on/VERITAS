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
#include "veritas/facts/FactStore.h"

#include <algorithm>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include "veritas/core/Hash.h"
#include "veritas/facts/SemanticKeyCodec.h"

namespace veritas::facts {
namespace {

constexpr std::string_view kProducer = "veritas.fact-lookup.v1";
constexpr std::string_view kUnavailable =
    "UNSUPPORTED: fact lookup is missing, stale, or unsealed; rebuild the "
    "index";

std::string Hash(std::string_view bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

bool IsCanonicalSelectorValue(const SemanticCellValue &cell) {
  return std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>)
          return core::HexToDigest(value.digest_hex).has_value();
        else if constexpr (std::is_same_v<T, semantic::DispatchKind>)
          return value <= semantic::DispatchKind::kUnknown;
        else if constexpr (std::is_same_v<T, semantic::AliasKind>)
          return value <= semantic::AliasKind::kUnknownAlias;
        else if constexpr (std::is_same_v<T, semantic::ByteRangeKind>)
          return value <= semantic::ByteRangeKind::kUnknown;
        else if constexpr (std::is_same_v<T, semantic::EpistemicState>)
          return value <= semantic::EpistemicState::kUnknown;
        else
          return true;
      },
      cell);
}

std::string EncodeCell(ColumnDomain domain, const SemanticCellValue &cell) {
  std::string encoded = EncodeEnumField(static_cast<std::uint64_t>(domain));
  encoded += std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::StableId>)
          return EncodeIdField(core::ToString(value));
        else if constexpr (std::is_same_v<T, std::int64_t>)
          return EncodeNumberField(value);
        else if constexpr (std::is_same_v<T, std::uint64_t>)
          return EncodeUnsignedField(value);
        else if constexpr (std::is_same_v<T, std::string>)
          return EncodeSymbolField(value);
        else
          return EncodeEnumField(static_cast<std::uint64_t>(value));
      },
      cell);
  return encoded;
}

} // namespace

Status FactStore::InitFactLookup() {
  auto version = metadata_store_.Query("PRAGMA fact_lookup.user_version", {});
  if (!version.ok())
    return version.status();
  if ((*version)[0][0] == "1")
    return Status::Ok();
  if ((*version)[0][0] != "0")
    return Status::FailedPrecondition(
        "UNSUPPORTED: fact lookup schema version");
  // The primary key is the covering B-tree for both the unbound relation
  // stream (column = -1) and a typed equality stream. No query-time join or
  // sort.
  if (auto s = metadata_store_.Execute(
          "CREATE TABLE IF NOT EXISTS fact_lookup.fact_lookup_v1 ("
          " run_id TEXT NOT NULL, relation_id TEXT NOT NULL,"
          " column_index INTEGER NOT NULL, value_digest TEXT NOT NULL,"
          " fact_id TEXT NOT NULL,"
          " PRIMARY KEY(run_id, relation_id, column_index, value_digest, "
          "fact_id)"
          ") WITHOUT ROWID",
          {});
      !s.ok())
    return s;
  if (auto s = metadata_store_.Execute(
          "CREATE TABLE IF NOT EXISTS fact_lookup.index_meta ("
          " run_id TEXT PRIMARY KEY, batch_id TEXT NOT NULL,"
          " producer_version TEXT NOT NULL, status TEXT NOT NULL,"
          " receipt_digest TEXT NOT NULL, store_identity TEXT NOT NULL,"
          " snapshot_digest TEXT NOT NULL"
          ") WITHOUT ROWID",
          {});
      !s.ok())
    return s;
  return metadata_store_.Execute("PRAGMA fact_lookup.user_version = 1", {});
}

StatusOr<std::optional<FactStore::LookupReceipt>>
FactStore::ReadLookupReceipt(std::string_view run_id) {
  // A run can accumulate batches. The latest batch alone cannot identify the
  // accumulated bindings: two stores could have different earlier batches.
  // Hash the authoritative receipt chain, never the run's fact rows.
  auto rows = metadata_store_.Query("SELECT batch_id FROM fact_batch_receipts "
                                    "WHERE run_id = ? ORDER BY rowid",
                                    {std::string(run_id)});
  if (!rows.ok())
    return rows.status();
  if (rows->empty())
    return std::optional<LookupReceipt>{};
  core::SHA256Hasher hasher;
  const auto append = [&](std::string_view bytes) {
    hasher.Update(std::as_bytes(std::span(bytes.data(), bytes.size())));
  };
  append(kProducer);
  append(EncodeIdField(run_id));
  for (const auto &row : *rows)
    append(EncodeIdField(row[0]));
  return std::optional<LookupReceipt>{
      LookupReceipt{rows->back()[0], core::DigestToHex(hasher.Finalize())}};
}

StatusOr<std::optional<std::string>>
FactStore::ReadyLookupFingerprint(std::string_view run_id,
                                  const LookupReceipt &receipt) {
  auto version = metadata_store_.Query("PRAGMA fact_lookup.user_version", {});
  if (!version.ok())
    return version.status();
  if ((*version)[0][0] != "1")
    return std::optional<std::string>{};
  auto rows = metadata_store_.Query(
      "SELECT snapshot_digest FROM fact_lookup.index_meta WHERE run_id = ? AND "
      "batch_id = ? "
      "AND producer_version = ? AND status = 'READY' AND receipt_digest = ? "
      "AND store_identity = ?",
      {std::string(run_id), receipt.batch_id, std::string(kProducer),
       receipt.snapshot_digest, store_identity_});
  if (!rows.ok())
    return rows.status();
  return rows->empty() ? std::optional<std::string>{}
                       : std::optional<std::string>{(*rows)[0][0]};
}

Status FactStore::AppendLookupFact(summarydb::BulkInsertBatcher &rows,
                                   const std::string &run_id,
                                   const AnalysisFact &fact) {
  // Publication and backfill both validated the canonical fact before this.
  const auto &schema = RelationsV2().Get(fact.row.relation);
  const auto id = core::ToString(fact.fact_id);
  if (auto s = rows.Add({run_id, schema.name, "-1", "", id}); !s.ok())
    return s;
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (auto s = rows.Add(
            {run_id, schema.name, std::to_string(i),
             Hash(EncodeCell(schema.columns[i].domain, fact.row.cells[i])),
             id});
        !s.ok())
      return s;
  }
  return Status::Ok();
}

StatusOr<std::vector<std::vector<std::string>>>
FactStore::CurrentFactKeys(std::string_view run_id, std::string_view after) {
  return metadata_store_.Query(
      "SELECT fact_id FROM run_fact_bindings WHERE run_id = ? AND "
      "is_current = 1 AND fact_id > ? ORDER BY fact_id LIMIT 4096",
      {std::string(run_id), std::string(after)});
}

Status FactStore::ClearFactLookup(std::string_view run_id) {
  return metadata_store_.Execute(
      "DELETE FROM fact_lookup.fact_lookup_v1 WHERE run_id = ?",
      {std::string(run_id)});
}

Status FactStore::SealFactLookup(std::string_view run_id,
                                 const LookupReceipt &receipt) {
  // On the write side only, bind exact membership as well as the receipt
  // chain. A rebuilt legacy store can contain bindings older than its first
  // v4 receipt. Both fresh publication and backfill derive the same identity
  // from ordered authoritative IDs, independent of physical insertion order.
  core::SHA256Hasher hasher;
  const auto append = [&](std::string_view bytes) {
    hasher.Update(std::as_bytes(std::span(bytes.data(), bytes.size())));
  };
  append("veritas.fact-lookup.snapshot.v1");
  append(receipt.snapshot_digest);
  std::string after;
  for (;;) {
    auto ids = CurrentFactKeys(run_id, after);
    if (!ids.ok())
      return ids.status();
    if (ids->empty())
      break;
    for (const auto &id : *ids)
      append(EncodeIdField(id[0]));
    after = ids->back()[0];
  }
  return metadata_store_.Execute(
      "INSERT OR REPLACE INTO fact_lookup.index_meta "
      "VALUES (?, ?, ?, 'READY', ?, ?, ?)",
      {std::string(run_id), receipt.batch_id, std::string(kProducer),
       receipt.snapshot_digest, store_identity_,
       core::DigestToHex(hasher.Finalize())});
}

Status FactStore::RebuildFactLookup(core::StableId run_id) {
  if (run_id.kind != core::IdKind::kAnalysisRun)
    return Status::InvalidArgument("expected AnalysisRunID");
  if (auto s = metadata_store_.BeginTransaction(); !s.ok())
    return s;
  auto rollback = [&](Status s) {
    metadata_store_.RollbackTransaction();
    return s;
  };
  const auto run = core::ToString(run_id);
  auto receipt = ReadLookupReceipt(run);
  if (!receipt.ok())
    return rollback(receipt.status());
  if (!*receipt)
    return rollback(
        Status::FailedPrecondition("UNSUPPORTED: no sealed native fact "
                                   "receipt; reanalyze into a new store"));
  if (auto s = InitFactLookup(); !s.ok())
    return rollback(s);
  if (auto s = ClearFactLookup(run); !s.ok())
    return rollback(s);
  summarydb::BulkInsertBatcher rows(
      metadata_store_, "INSERT INTO fact_lookup.fact_lookup_v1 VALUES", 5);
  std::string after;
  for (;;) {
    auto ids = CurrentFactKeys(run, after);
    if (!ids.ok())
      return rollback(ids.status());
    if (ids->empty())
      break;
    for (const auto &id : *ids) {
      auto parsed = core::ParseStableId(id[0]);
      if (!parsed.ok())
        return rollback(parsed.status());
      auto fact = GetFact(*parsed);
      if (!fact.ok())
        return rollback(fact.status());
      if (auto s = AppendLookupFact(rows, run, *fact); !s.ok())
        return rollback(s);
    }
    after = ids->back()[0];
  }
  if (auto s = rows.Flush(); !s.ok())
    return rollback(s);
  if (auto s = SealFactLookup(run, **receipt); !s.ok())
    return rollback(s);
  auto s = metadata_store_.CommitTransaction();
  return s.ok() ? s : rollback(s);
}

StatusOr<std::string> FactStore::FactLookupFingerprint(core::StableId run_id) {
  if (!metadata_store_.InTransaction())
    return Status::FailedPrecondition(
        "fact lookup requires a pinned transaction");
  const auto run = core::ToString(run_id);
  auto receipt = ReadLookupReceipt(run);
  if (!receipt.ok())
    return receipt.status();
  if (!*receipt)
    return Status::FailedPrecondition(std::string(kUnavailable));
  auto ready = ReadyLookupFingerprint(run, **receipt);
  if (!ready.ok())
    return ready.status();
  if (!*ready)
    return Status::FailedPrecondition(std::string(kUnavailable));
  return std::move(**ready);
}

StatusOr<FactPage> FactStore::ReadFacts(core::StableId run_id,
                                        std::string_view snapshot_digest,
                                        const FactSelector &selector,
                                        const PageRequest &request) {
  if (!metadata_store_.InTransaction() || snapshot_digest.empty())
    return Status::FailedPrecondition(
        "fact page requires a pinned lookup snapshot");
  if (request.limit == 0 || request.limit > kMaxFactPageRows ||
      request.max_examined == 0 || request.max_output_bytes == 0)
    return Status::InvalidArgument(
        "invalid fact page row, work, or byte limit");
  if (static_cast<std::size_t>(selector.relation) >= kRelationCountV2)
    return Status::InvalidArgument("unknown fact selector relation");

  const auto &schema = RelationsV2().Get(selector.relation);
  if (selector.equalities.size() > schema.columns.size())
    return Status::InvalidArgument("too many fact selector equalities");
  auto equalities = selector.equalities;
  std::ranges::sort(equalities, {}, &CellEquality::column);
  std::string selector_key = EncodeKeyHeader(schema.name, equalities.size());
  std::string driving_column = "-1";
  std::string driving_digest;
  for (std::size_t i = 0; i < equalities.size(); ++i) {
    const auto &eq = equalities[i];
    if (eq.column >= schema.columns.size() ||
        schema.columns[eq.column].domain != eq.domain ||
        (i != 0 && equalities[i - 1].column == eq.column))
      return Status::InvalidArgument(
          "invalid or repeated fact selector column");
    if (auto s = ValidateSemanticCell(eq.domain, eq.value); !s.ok())
      return s;
    if (!IsCanonicalSelectorValue(eq.value))
      return Status::InvalidArgument("noncanonical fact selector value");
    const auto encoded = EncodeCell(eq.domain, eq.value);
    selector_key += EncodeUnsignedField(eq.column);
    selector_key += encoded;
    if (i == 0) {
      driving_column = std::to_string(eq.column);
      driving_digest = Hash(encoded);
    }
  }
  const auto selector_digest = Hash(selector_key);
  FactPage page;
  std::string after;
  if (request.after) {
    const auto &cursor = *request.after;
    if (cursor.snapshot_digest != snapshot_digest ||
        cursor.selector_digest != selector_digest ||
        cursor.ordering_version != kFactPageOrderingVersion ||
        cursor.last_consumed.kind != core::IdKind::kFact)
      return Status::InvalidArgument(
          "fact cursor does not match snapshot, selector, or order");
    after = core::ToString(cursor.last_consumed);
    auto canonical = core::ParseStableId(after);
    if (!canonical.ok() || *canonical != cursor.last_consumed ||
        !core::HexToDigest(cursor.last_consumed.digest_hex))
      return Status::InvalidArgument("invalid fact cursor key");
    page.continuation = cursor;
  }
  const auto run = core::ToString(run_id);
  // Seek one candidate at a time. Stop at the first unreturned matching row
  // (the limit+1 probe), at the work frontier, or on actual index exhaustion.
  // No prefetched rows are hidden from the work counter.
  while (page.examined < request.max_examined) {
    auto rows = metadata_store_.Query(
        "SELECT fact_id FROM fact_lookup.fact_lookup_v1 WHERE "
        "run_id = ? AND relation_id = ? AND column_index = ? AND "
        "value_digest = ? AND fact_id > ? ORDER BY fact_id LIMIT 1",
        {run, schema.name, driving_column, driving_digest, after});
    if (!rows.ok())
      return rows.status();
    if (rows->empty()) {
      page.exhausted = true;
      page.continuation.reset();
      return page;
    }
    ++page.examined;
    const auto &id_text = (*rows)[0][0];
    auto id = core::ParseStableId(id_text);
    if (!id.ok())
      return id.status();
    std::uint64_t bytes = 0;
    auto fact = ReadCanonicalFact(*id, &bytes);
    if (!fact.ok())
      return fact.status();
    if (fact->row.relation != selector.relation)
      return Status::FailedPrecondition(
          "fact lookup relation disagrees with canonical row");
    if (std::ranges::all_of(equalities, [&](const CellEquality &eq) {
          return fact->row.cells[eq.column] == eq.value;
        })) {
      if (page.facts.size() == request.limit)
        return page;
      if (bytes > request.max_output_bytes - page.output_bytes) {
        if (bytes >
            std::numeric_limits<std::uint64_t>::max() - page.output_bytes)
          return Status::Internal("fact page byte size overflow");
        page.required_output_bytes = page.output_bytes + bytes;
        return page;
      }
      // The sidecar supplies candidates only. Recheck the current native
      // membership before accepting an indexed fact as evidence for this run.
      auto binding = GetBinding(run_id, *id);
      if (!binding.ok())
        return binding.status();
      page.output_bytes += bytes;
      page.facts.push_back(std::move(*fact));
    }
    after = id_text;
    page.continuation =
        PageCursor{std::string(snapshot_digest), selector_digest, *id};
  }
  page.work_exhausted = true;
  return page;
}

} // namespace veritas::facts
