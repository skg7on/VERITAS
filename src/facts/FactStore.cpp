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

#include <cstdlib>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "veritas/facts/FactProto.h"
#include "veritas/facts/HexCodec.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::facts {

namespace {

using summarydb::MetadataStore;

std::string IntToString(int value) { return std::to_string(value); }

// Reconstructs a binding from a run_fact_bindings row (the SELECT column order
// is fixed: confidence, producer_kind, analyzer_run_id, scope_kind, scope_id,
// selected_witness_id, is_current).
RunFactBinding ParseBinding(const std::vector<std::string>& row,
                            core::StableId run_id, core::StableId fact_id) {
  RunFactBinding binding;
  binding.run_id = run_id;
  binding.fact_id = fact_id;
  binding.confidence =
      row[0].empty() ? std::nullopt
                     : std::optional<double>(std::strtod(row[0].c_str(), nullptr));
  binding.producer_kind =
      static_cast<ProducerKind>(std::strtol(row[1].c_str(), nullptr, 10));
  binding.analyzer_run_id = row[2];
  binding.scope_kind = row[3];
  binding.scope_id = row[4];
  binding.selected_witness_id = row[5];
  binding.is_current = row[6] == "1";
  return binding;
}

}  // namespace

ProducerKind ProducerKindForEngine(EngineIdentity engine) {
  switch (engine) {
    case EngineIdentity::kSouffle:
      return ProducerKind::kWpaSouffle;
    case EngineIdentity::kCppConformance:
      return ProducerKind::kWpaCppConformance;
    case EngineIdentity::kCppEmergency:
      return ProducerKind::kWpaCppEmergency;
  }
  return ProducerKind::kWpaSouffle;
}

StatusOr<FactStore> FactStore::Open(const std::filesystem::path& db_path) {
  std::error_code ec;
  std::filesystem::create_directories(db_path, ec);
  if (ec) {
    return Status::Internal("failed to create fact store directory: " +
                            ec.message());
  }
  // Same database file the WPA run repository uses, so facts, run state, and
  // provenance live in one SummaryDB.
  auto store = MetadataStore::Open(db_path / "metadata.db");
  if (!store.ok()) {
    return store.status();
  }
  if (Status s = store->ApplySchema(); !s.ok()) {
    return s;
  }
  return FactStore(std::move(store).value());
}

FactStore::FactStore(summarydb::MetadataStore store)
    : metadata_store_(std::move(store)) {}

FactStore::FactStore(FactStore&&) noexcept = default;
FactStore& FactStore::operator=(FactStore&&) noexcept = default;
FactStore::~FactStore() = default;

Status FactStore::PutFact(const AnalysisFact& fact) {
  auto proto = ToProtoFact(fact);
  if (!proto.ok()) {
    return proto.status();
  }
  std::string serialized;
  if (!proto->SerializeToString(&serialized)) {
    return Status::Internal("failed to serialize fact");
  }
  const char* sql =
      "INSERT OR IGNORE INTO analysis_facts (fact_id, relation_name, cells_hex)"
      " VALUES (?, ?, ?)";
  return metadata_store_.Execute(
      sql, {core::ToString(fact.fact_id),
            RelationsV2().Get(fact.row.relation).name, HexEncode(serialized)});
}

Status FactStore::PutBinding(const RunFactBinding& binding) {
  Status s = metadata_store_.Execute(
      "UPDATE run_fact_bindings SET is_current = 0"
      " WHERE run_id = ? AND fact_id = ? AND is_current = 1",
      {core::ToString(binding.run_id), core::ToString(binding.fact_id)});
  if (!s.ok()) {
    return s;
  }

  const char* sql =
      "INSERT INTO run_fact_bindings (run_id, fact_id, confidence,"
      " producer_kind, analyzer_run_id, scope_kind, scope_id,"
      " selected_witness_id, is_current) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 1)";
  const std::string confidence =
      binding.confidence.has_value() ? std::to_string(*binding.confidence) : "";
  return metadata_store_.Execute(
      sql,
      {core::ToString(binding.run_id), core::ToString(binding.fact_id),
       confidence, IntToString(static_cast<int>(binding.producer_kind)),
       binding.analyzer_run_id, binding.scope_kind, binding.scope_id,
       binding.selected_witness_id});
}

Status FactStore::Publish(const AnalysisFactBatch& batch) {
  // Collect every canonical fact: the published (derived) facts plus each
  // witness input, which may be a rooted input absent from batch.facts. Any
  // validation failure here happens before a transaction opens, so no rollback
  // is needed.
  std::vector<AnalysisFact> all_facts = batch.facts;
  std::set<core::StableId> fact_ids;
  for (const AnalysisFact& fact : all_facts) {
    fact_ids.insert(fact.fact_id);
  }
  std::set<core::StableId> rooted_inputs(batch.rooted_input_fact_ids.begin(),
                                         batch.rooted_input_fact_ids.end());
  for (const WitnessEdge& edge : batch.witnesses) {
    auto input = MakeFact(edge.input.row);
    if (!input.ok()) {
      return input.status();
    }
    if (fact_ids.insert(input->fact_id).second) {
      all_facts.push_back(*input);
    }
  }

  Status s = metadata_store_.BeginTransaction();
  if (!s.ok()) {
    return s;
  }
  auto rollback = [&](Status err) {
    metadata_store_.RollbackTransaction();
    return err;
  };

  // Canonical facts: the run's derived facts plus their rooted inputs, all
  // stored for display. Only the derived facts get an occurrence binding.
  for (const AnalysisFact& fact : all_facts) {
    s = PutFact(fact);
    if (!s.ok()) {
      return rollback(s);
    }
  }
  for (const AnalysisFact& fact : batch.facts) {
    RunFactBinding binding;
    binding.run_id = batch.run.run_id;
    binding.fact_id = fact.fact_id;
    binding.producer_kind = ProducerKindForEngine(batch.run.engine);
    binding.is_current = true;
    binding.selected_witness_id = core::ToString(fact.fact_id);
    s = PutBinding(binding);
    if (!s.ok()) {
      return rollback(s);
    }
  }

  // The witness DAG: one node per derived fact, one edge per derivation step.
  ProvenanceStore provenance(metadata_store_);
  std::set<std::string> node_seen;
  for (const WitnessEdge& edge : batch.witnesses) {
    auto result_fact = MakeFact(edge.result.row);
    if (!result_fact.ok()) {
      return rollback(result_fact.status());
    }
    auto input_fact = MakeFact(edge.input.row);
    if (!input_fact.ok()) {
      return rollback(input_fact.status());
    }

    const std::string witness_id = core::ToString(result_fact->fact_id);
    if (node_seen.insert(witness_id).second) {
      FactWitness node;
      node.run_id = batch.run.run_id;
      node.output_fact_id = result_fact->fact_id;
      node.witness_id = witness_id;
      node.selected = true;
      node.producer_kind = ProducerKindForEngine(batch.run.engine);
      node.rule_id = edge.rule_id;
      s = provenance.PutNode(node);
      if (!s.ok()) {
        return rollback(s);
      }
    }

    FactWitnessEdge witness_edge;
    witness_edge.run_id = batch.run.run_id;
    witness_edge.output_fact_id = result_fact->fact_id;
    witness_edge.witness_id = witness_id;
    witness_edge.input_kind =
        rooted_inputs.count(input_fact->fact_id) ? "rooted" : "derived";
    witness_edge.input_id = core::ToString(input_fact->fact_id);
    witness_edge.input_ordinal = edge.input_ordinal;
    s = provenance.PutEdge(witness_edge);
    if (!s.ok()) {
      return rollback(s);
    }
  }

  return metadata_store_.CommitTransaction();
}

StatusOr<AnalysisFact> FactStore::GetFact(core::StableId fact_id) {
  auto rows = metadata_store_.Query(
      "SELECT cells_hex FROM analysis_facts WHERE fact_id = ?",
      {core::ToString(fact_id)});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->empty()) {
    return Status::NotFound("fact not found");
  }
  auto decoded = HexDecode((*rows)[0][0]);
  if (!decoded.ok()) {
    return decoded.status();
  }
  fact_proto::Fact proto;
  if (!proto.ParseFromString(*decoded)) {
    return Status::Internal("failed to parse stored fact");
  }
  return FromProtoFact(proto);
}

StatusOr<RunFactBinding> FactStore::GetBinding(core::StableId run_id,
                                               core::StableId fact_id) {
  auto rows = metadata_store_.Query(
      "SELECT confidence, producer_kind, analyzer_run_id, scope_kind,"
      " scope_id, selected_witness_id, is_current FROM run_fact_bindings"
      " WHERE run_id = ? AND fact_id = ? AND is_current = 1",
      {core::ToString(run_id), core::ToString(fact_id)});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->empty()) {
    return Status::NotFound("binding not found");
  }
  return ParseBinding((*rows)[0], run_id, fact_id);
}

StatusOr<std::vector<RunFactBinding>> FactStore::GetBindings(
    core::StableId run_id, core::StableId fact_id) {
  auto rows = metadata_store_.Query(
      "SELECT confidence, producer_kind, analyzer_run_id, scope_kind,"
      " scope_id, selected_witness_id, is_current FROM run_fact_bindings"
      " WHERE run_id = ? AND fact_id = ? ORDER BY binding_id DESC",
      {core::ToString(run_id), core::ToString(fact_id)});
  if (!rows.ok()) {
    return rows.status();
  }
  std::vector<RunFactBinding> bindings;
  bindings.reserve(rows->size());
  for (const auto& row : *rows) {
    bindings.push_back(ParseBinding(row, run_id, fact_id));
  }
  return bindings;
}

StatusOr<std::vector<AnalysisFact>> FactStore::GetCurrentFacts(
    core::StableId run_id) {
  auto ids = metadata_store_.Query(
      "SELECT fact_id FROM run_fact_bindings WHERE run_id = ? AND is_current = 1",
      {core::ToString(run_id)});
  if (!ids.ok()) {
    return ids.status();
  }
  std::vector<AnalysisFact> facts;
  facts.reserve(ids->size());
  for (const auto& row : *ids) {
    auto parsed = core::ParseStableId(row[0]);
    if (!parsed.ok()) {
      return parsed.status();
    }
    auto fact = GetFact(*parsed);
    if (!fact.ok()) {
      return fact.status();
    }
    facts.push_back(std::move(*fact));
  }
  return facts;
}

}  // namespace veritas::facts
