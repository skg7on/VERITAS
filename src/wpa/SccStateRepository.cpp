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

#include "veritas/wpa/SccStateRepository.h"

#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veritas::wpa {
namespace {

StatusOr<std::size_t> ParseSize(const std::string &value) {
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return Status::Internal("stored SCC iteration count is invalid");
  }
  return parsed;
}

StatusOr<int> ParseInt(const std::string &value) {
  int parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return Status::Internal("stored SCC enum value is invalid");
  }
  return parsed;
}

Status RollbackWith(summarydb::MetadataStore &store, Status status) {
  auto rollback = store.RollbackTransaction();
  return rollback.ok() ? status : rollback;
}

bool IsLowercaseSha256Hex(std::string_view value) {
  if (value.size() != 64u)
    return false;
  for (char character : value) {
    if ((character < '0' || character > '9') &&
        (character < 'a' || character > 'f')) {
      return false;
    }
  }
  return true;
}

// The statement one stored state row is written with: the same columns, the
// same conflict rule, and the same `updated_at` refresh an unbatched commit
// issued, so the rows a flush commits are the rows a per-component commit
// committed. Only the transaction boundary moved.
constexpr const char *kComponentStateUpsert =
    "INSERT INTO wpa_component_states(scc_id, revision_id, build_variant_id, "
    "component_kind, input_hash, fixpoint_hash, externally_visible_hash, "
    "iteration_count, status) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(scc_id, revision_id, build_variant_id, component_kind) DO "
    "UPDATE SET input_hash=excluded.input_hash, "
    "fixpoint_hash=excluded.fixpoint_hash, "
    "externally_visible_hash=excluded.externally_visible_hash, "
    "iteration_count=excluded.iteration_count, status=excluded.status, "
    "updated_at=strftime('%s', 'now')";

Status ValidateResult(const SccResult &result) {
  if (result.scc_id.kind != core::IdKind::kScc) {
    return Status::InvalidArgument("SCC state requires an SCC ID");
  }
  if (result.component_kind != summary::v1::COMPONENT_KIND_CALLS &&
      result.component_kind != summary::v1::COMPONENT_KIND_MEMORY_EFFECTS &&
      result.component_kind != summary::v1::COMPONENT_KIND_VALUE_FLOW &&
      result.component_kind != summary::v1::COMPONENT_KIND_UNKNOWNS) {
    return Status::InvalidArgument(
        "SCC state requires a supported WPA component");
  }
  if (result.status != SccStatus::kConverged &&
      result.status != SccStatus::kApproximated) {
    return Status::InvalidArgument(
        "unsuccessful SCC results cannot be persisted");
  }
  if (result.iteration_count == 0u ||
      !IsLowercaseSha256Hex(result.input_hash) ||
      !IsLowercaseSha256Hex(result.fixpoint_hash) ||
      !IsLowercaseSha256Hex(result.externally_visible_hash)) {
    return Status::InvalidArgument("SCC convergence fields are invalid");
  }
  return Status::Ok();
}

} // namespace

Status SccStateRepository::PublishGraph(const SccContext &context,
                                        const CallGraph &call_graph,
                                        const SccGraph &scc_graph) {
  auto begun = metadata_store_.BeginTransaction();
  if (!begun.ok())
    return begun;
  for (std::string_view table : {"wpa_scc_edges", "wpa_scc_members"}) {
    auto deleted = metadata_store_.Execute(
        "DELETE FROM " + std::string(table) +
            " WHERE revision_id = ? AND build_variant_id = ?",
        {context.revision_id, context.build_variant_id});
    if (!deleted.ok())
      return RollbackWith(metadata_store_, deleted);
  }

  for (const auto &scc_id : scc_graph.ReverseTopologicalOrder()) {
    auto inserted = metadata_store_.Execute(
        "INSERT INTO wpa_sccs(scc_id, revision_id, build_variant_id) "
        "VALUES(?, ?, ?) ON CONFLICT(scc_id, revision_id, build_variant_id) "
        "DO NOTHING",
        {core::ToString(scc_id), context.revision_id,
         context.build_variant_id});
    if (!inserted.ok())
      return RollbackWith(metadata_store_, inserted);
    auto members = scc_graph.Members(scc_id);
    if (!members.ok())
      return RollbackWith(metadata_store_, members.status());
    for (const auto &member : *members) {
      inserted = metadata_store_.Execute(
          "INSERT INTO wpa_scc_members(scc_id, revision_id, build_variant_id, "
          "function_variant_id) VALUES(?, ?, ?, ?)",
          {core::ToString(scc_id), context.revision_id,
           context.build_variant_id, core::ToString(member)});
      if (!inserted.ok())
        return RollbackWith(metadata_store_, inserted);
    }
  }

  std::map<std::pair<core::StableId, core::StableId>,
           summary::v1::EpistemicState>
      edges;
  for (const auto &caller : call_graph.Functions()) {
    auto caller_scc = scc_graph.SccForFunction(caller);
    if (!caller_scc.ok()) {
      return RollbackWith(metadata_store_, caller_scc.status());
    }
    for (const auto &edge : call_graph.Outgoing(caller)) {
      auto callee_scc = scc_graph.SccForFunction(edge.callee);
      if (!callee_scc.ok()) {
        return RollbackWith(metadata_store_, callee_scc.status());
      }
      if (*caller_scc == *callee_scc)
        continue;
      auto [it, inserted] =
          edges.emplace(std::pair{*caller_scc, *callee_scc}, edge.epistemic);
      if (!inserted && edge.epistemic == summary::v1::EPISTEMIC_STATE_MAY) {
        it->second = summary::v1::EPISTEMIC_STATE_MAY;
      }
    }
  }
  for (const auto &[edge, epistemic] : edges) {
    auto inserted = metadata_store_.Execute(
        "INSERT INTO wpa_scc_edges(caller_scc_id, callee_scc_id, revision_id, "
        "build_variant_id, epistemic) VALUES(?, ?, ?, ?, ?)",
        {core::ToString(edge.first), core::ToString(edge.second),
         context.revision_id, context.build_variant_id,
         std::to_string(static_cast<int>(epistemic))});
    if (!inserted.ok())
      return RollbackWith(metadata_store_, inserted);
  }
  auto deleted = metadata_store_.Execute(
      "DELETE FROM wpa_component_states WHERE revision_id = ? AND "
      "build_variant_id = ? AND NOT EXISTS (SELECT 1 FROM wpa_scc_members "
      "WHERE wpa_scc_members.scc_id = wpa_component_states.scc_id AND "
      "wpa_scc_members.revision_id = wpa_component_states.revision_id AND "
      "wpa_scc_members.build_variant_id = "
      "wpa_component_states.build_variant_id)",
      {context.revision_id, context.build_variant_id});
  if (!deleted.ok())
    return RollbackWith(metadata_store_, deleted);
  deleted = metadata_store_.Execute(
      "DELETE FROM wpa_sccs WHERE revision_id = ? AND build_variant_id = ? "
      "AND NOT EXISTS (SELECT 1 FROM wpa_scc_members WHERE "
      "wpa_scc_members.scc_id = wpa_sccs.scc_id AND "
      "wpa_scc_members.revision_id = wpa_sccs.revision_id AND "
      "wpa_scc_members.build_variant_id = wpa_sccs.build_variant_id)",
      {context.revision_id, context.build_variant_id});
  if (!deleted.ok())
    return RollbackWith(metadata_store_, deleted);
  auto committed = metadata_store_.CommitTransaction();
  return committed.ok() ? committed : RollbackWith(metadata_store_, committed);
}

StatusOr<std::optional<StoredSccState>>
SccStateRepository::LoadState(const SccContext &context, core::StableId scc_id,
                              summary::v1::ComponentKind component_kind) const {
  auto rows = metadata_store_.Query(
      "SELECT input_hash, fixpoint_hash, externally_visible_hash, "
      "iteration_count, status FROM wpa_component_states WHERE scc_id = ? "
      "AND revision_id = ? AND build_variant_id = ? AND component_kind = ?",
      {core::ToString(scc_id), context.revision_id, context.build_variant_id,
       std::to_string(static_cast<int>(component_kind))});
  if (!rows.ok())
    return rows.status();
  if (rows->empty())
    return std::optional<StoredSccState>{};
  if (rows->size() != 1u || (*rows)[0].size() != 5u) {
    return Status::Internal("stored SCC state has an invalid row shape");
  }
  auto iterations = ParseSize((*rows)[0][3]);
  auto status_value = ParseInt((*rows)[0][4]);
  if (!iterations.ok())
    return iterations.status();
  if (!status_value.ok())
    return status_value.status();
  if (*status_value < static_cast<int>(SccStatus::kConverged) ||
      *status_value > static_cast<int>(SccStatus::kUnsupported)) {
    return Status::Internal("stored SCC status is out of range");
  }
  return std::optional<StoredSccState>{
      StoredSccState{.scc_id = std::move(scc_id),
                     .component_kind = component_kind,
                     .input_hash = (*rows)[0][0],
                     .fixpoint_hash = (*rows)[0][1],
                     .externally_visible_hash = (*rows)[0][2],
                     .iteration_count = *iterations,
                     .status = static_cast<SccStatus>(*status_value)}};
}

StatusOr<ExternalChange>
SccStateRepository::StoreState(const SccContext &context,
                               const SccResult &result) {
  auto valid = ValidateResult(result);
  if (!valid.ok())
    return valid;
  auto topology = metadata_store_.Query(
      "SELECT 1 FROM wpa_sccs WHERE scc_id = ? AND revision_id = ? AND "
      "build_variant_id = ?",
      {core::ToString(result.scc_id), context.revision_id,
       context.build_variant_id});
  if (!topology.ok())
    return topology.status();
  if (topology->size() != 1u) {
    return Status::NotFound("SCC is not present in the published topology");
  }
  // The classification is read from the committed row, exactly as it was when
  // this store committed per component. A stored row that no flush has covered
  // yet is invisible here, which is safe for this repository's only caller: a
  // run visits each `(scc, component kind)` key once, so no call reads back a
  // row an open batch is holding. See the load-bearing note on `QueueStateRows`.
  auto previous = LoadState(context, result.scc_id, result.component_kind);
  if (!previous.ok())
    return previous.status();

  QueueStateRows(context, result);
  if (pending_state_rows_.size() >= kStateBatchSize) {
    // The batch is full, so this call pays the commit for the rows the batch
    // already holds rather than one commit per component.
    Status flushed = FlushStateCache();
    if (!flushed.ok())
      return flushed;
  }
  return !previous->has_value() || (*previous)->externally_visible_hash !=
                                       result.externally_visible_hash
             ? ExternalChange::kChanged
             : ExternalChange::kUnchanged;
}

// The one place a stored convergence row's key could collide with an open
// batch. `WpaOrchestrator::Run` visits each `(scc_id, component)` key exactly
// once per run: `SccGraph::ReverseTopologicalOrder` yields each SCC once (Kahn
// over the condensation DAG, where a node is enqueued only on the decrement
// that reaches zero, and the size check rejects any duplicate), and the run's
// component list is a set of distinct kinds. A caller that stored the same key
// twice inside one batch would have its second call read the row from the
// previous run rather than the row the batch holds, and would classify the
// component against it; flush between such stores.
void SccStateRepository::QueueStateRows(const SccContext &context,
                                        const SccResult &result) {
  pending_state_rows_.push_back(
      {core::ToString(result.scc_id), context.revision_id,
       context.build_variant_id,
       std::to_string(static_cast<int>(result.component_kind)),
       result.input_hash, result.fixpoint_hash, result.externally_visible_hash,
       std::to_string(result.iteration_count),
       std::to_string(static_cast<int>(result.status))});
}

Status SccStateRepository::FlushStateCache() {
  if (pending_state_rows_.empty())
    return Status::Ok();

  Status begun = metadata_store_.BeginTransaction();
  if (!begun.ok()) {
    // No transaction was opened, so there is nothing to roll back — but the
    // batch is abandoned here exactly as it is below, because leaving it queued
    // would let a later flush commit rows for a run that has already failed.
    pending_state_rows_.clear();
    return begun;
  }
  // A failed flush abandons the batch: the rows queued with it are rolled back
  // with the transaction, so nothing half-written survives and nothing stays
  // queued for a later flush, and the caller's failure path fails the run. What
  // is lost is one batch of convergence state for components the next run
  // re-executes.
  auto abandon = [&](Status status) {
    metadata_store_.RollbackTransaction();
    pending_state_rows_.clear();
    return status;
  };

  // The statement text, the columns, and the conflict rule are unchanged from
  // the per-component commit — only the transaction boundary moved. A
  // `BulkInsertBatcher` is not used here: its statement is a prefix plus value
  // tuples, so it cannot carry this row's `ON CONFLICT ... DO UPDATE` suffix,
  // and a batch of `kStateBatchSize` rows is far below the bind-parameter budget
  // at which its multi-row path engages, so it would issue one statement per row
  // inside this transaction anyway.
  for (std::vector<std::string> &row : pending_state_rows_) {
    Status stored = metadata_store_.Execute(kComponentStateUpsert, row);
    if (!stored.ok())
      return abandon(stored);
  }
  Status committed = metadata_store_.CommitTransaction();
  if (!committed.ok())
    return abandon(committed);
  pending_state_rows_.clear();
  return Status::Ok();
}

} // namespace veritas::wpa
