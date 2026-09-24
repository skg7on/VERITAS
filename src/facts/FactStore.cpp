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
#include <cstdlib>
#include <map>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/facts/FactProto.h"
#include "veritas/facts/HexCodec.h"
#include "veritas/facts/ProvenanceStore.h"
#include "veritas/facts/Witness.h"

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

// A witness edge plus the identity of the fact it cites as input. The identity
// is derived once, where the edge is first seen, and then reused: it is needed
// to find rooted evidence, to classify the edge's input, and no part of the
// publication path needs to re-derive it.
struct WitnessEdgeRef {
  const WitnessEdge* edge;
  core::StableId input_fact_id;
};

// The witness-dependent derivation identity (design §7): the semantic key of
// the result, the rule that derived it, and its ordered input semantic keys.
// Distinct derivations of the same fact produce distinct witness ids, while
// the semantic FactID stays witness-independent.
//
// The fields stream straight into the hash. Building the whole byte string
// first cost one large allocation per derivation and, on the caller's side, a
// copy of every input row just to reach the encoder.
std::string DeriveWitnessId(const std::vector<WitnessEdgeRef>& ordered_edges) {
  core::SHA256Hasher hasher;
  auto update = [&hasher](std::string_view value) {
    hasher.Update(std::as_bytes(std::span(value.data(), value.size())));
  };
  auto append_field = [&update](std::string_view value) {
    update(std::to_string(value.size()));
    update(":");
    update(value);
  };
  std::string key;
  update("veritas.witness.derivation.v1");
  AppendSemanticKey(&key, ordered_edges.front().edge->result.row);
  append_field(key);
  append_field(ordered_edges.front().edge->rule_id);
  for (const WitnessEdgeRef& ref : ordered_edges) {
    key.clear();
    AppendSemanticKey(&key, ref.edge->input.row);
    append_field(key);
  }
  return core::DigestToHex(hasher.Finalize());
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

Status FactStore::AppendFact(summarydb::BulkInsertBatcher& facts,
                             const AnalysisFact& fact) {
  auto proto = ToProtoFact(fact);
  if (!proto.ok()) {
    return proto.status();
  }
  std::string serialized;
  if (!proto->SerializeToString(&serialized)) {
    return Status::Internal("failed to serialize fact");
  }
  return facts.Add({core::ToString(fact.fact_id),
                    RelationsV2().Get(fact.row.relation).name,
                    HexEncode(serialized)});
}

Status FactStore::AppendBinding(summarydb::BulkInsertBatcher& bindings,
                                const RunFactBinding& binding) {
  Status s = metadata_store_.Execute(
      "UPDATE run_fact_bindings SET is_current = 0"
      " WHERE run_id = ? AND fact_id = ? AND is_current = 1",
      {core::ToString(binding.run_id), core::ToString(binding.fact_id)});
  if (!s.ok()) {
    return s;
  }
  const std::string confidence =
      binding.confidence.has_value() ? std::to_string(*binding.confidence) : "";
  return bindings.Add({core::ToString(binding.run_id),
                       core::ToString(binding.fact_id), confidence,
                       IntToString(static_cast<int>(binding.producer_kind)),
                       binding.analyzer_run_id, binding.scope_kind,
                       binding.scope_id, binding.selected_witness_id, "1"});
}

Status FactStore::Publish(const AnalysisFactBatch& batch) {
  // Collect every canonical fact: the published (derived) facts plus each
  // witness input, which may be a rooted input absent from batch.facts. Any
  // validation failure here happens before a transaction opens, so no rollback
  // is needed.
  std::set<core::StableId> fact_ids;
  for (const AnalysisFact& fact : batch.facts) {
    fact_ids.insert(fact.fact_id);
  }
  std::vector<AnalysisFact> missing_input_facts;
  std::set<core::StableId> rooted_inputs(batch.rooted_input_fact_ids.begin(),
                                         batch.rooted_input_fact_ids.end());

  struct ResultWitness {
    std::string witness_id;
    std::vector<WitnessEdgeRef> ordered_edges;
  };
  std::map<std::string, ResultWitness> result_witnesses;
  for (const WitnessEdge& edge : batch.witnesses) {
    auto input_fact_id = DeriveFactId(edge.input.row);
    if (!input_fact_id.ok()) {
      return input_fact_id.status();
    }
    // The row is only copied for an input that is not already a published fact,
    // which is the only case that has to be stored from here.
    if (fact_ids.insert(*input_fact_id).second) {
      missing_input_facts.push_back(
          AnalysisFact{*input_fact_id, edge.input.row});
    }
    result_witnesses[EncodeSemanticKey(edge.result.row)].ordered_edges.push_back(
        WitnessEdgeRef{.edge = &edge, .input_fact_id = std::move(*input_fact_id)});
  }

  // Group the canonical witnesses by result and derive each result's
  // witness-dependent derivation identity. The selected proof's witness id is
  // distinct from the semantic FactID, so re-deriving a fact by a different
  // proof retains a distinct witness record.
  std::map<core::StableId, std::string> witness_id_by_fact;
  for (auto& [result_key, entry] : result_witnesses) {
    std::ranges::sort(entry.ordered_edges, [](const auto& a, const auto& b) {
      return a.edge->input_ordinal < b.edge->input_ordinal;
    });
    entry.witness_id = DeriveWitnessId(entry.ordered_edges);
    auto fact_id = DeriveFactId(entry.ordered_edges.front().edge->result.row);
    if (!fact_id.ok()) {
      return fact_id.status();
    }
    witness_id_by_fact[*fact_id] = entry.witness_id;
  }

  std::map<core::StableId, const RootedInputFact*> root_evidence;
  for (const auto& root : batch.rooted_input_facts) {
    root_evidence[root.fact.fact_id] = &root;
  }

  // Idempotent redelivery: a batch already durably published is a successful
  // no-op (schema v4 receipt keyed by (run_id, batch_id)).
  const std::string run_id = core::ToString(batch.run.run_id);
  const std::string batch_id = core::ToString(batch.batch_id);
  auto already = metadata_store_.Query(
      "SELECT 1 FROM fact_batch_receipts WHERE run_id = ? AND batch_id = ?",
      {run_id, batch_id});
  if (!already.ok()) {
    return already.status();
  }
  if (!already->empty()) {
    return Status::Ok();
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
  //
  // Both loops write through bulk writers: a run publishes well over a million
  // facts and bindings, and one statement per row is dominated by the
  // per-statement step rather than by the row.
  summarydb::BulkInsertBatcher facts(
      metadata_store_,
      "INSERT OR IGNORE INTO analysis_facts (fact_id, relation_name, cells_hex)"
      " VALUES",
      3);
  summarydb::BulkInsertBatcher bindings(
      metadata_store_,
      "INSERT INTO run_fact_bindings (run_id, fact_id, confidence,"
      " producer_kind, analyzer_run_id, scope_kind, scope_id,"
      " selected_witness_id, is_current) VALUES",
      9);
  for (const AnalysisFact& fact : batch.facts) {
    s = AppendFact(facts, fact);
    if (!s.ok()) {
      return rollback(s);
    }
  }
  for (const AnalysisFact& fact : missing_input_facts) {
    s = AppendFact(facts, fact);
    if (!s.ok()) {
      return rollback(s);
    }
  }
  // Flushed before any binding is written: run_fact_bindings carries a foreign
  // key onto analysis_facts, which this store enforces.
  s = facts.Flush();
  if (!s.ok()) {
    return rollback(s);
  }
  for (const AnalysisFact& fact : batch.facts) {
    RunFactBinding binding;
    binding.run_id = batch.run.run_id;
    binding.fact_id = fact.fact_id;
    binding.producer_kind = ProducerKindForEngine(batch.run.engine);
    binding.is_current = true;
    const auto witness = witness_id_by_fact.find(fact.fact_id);
    binding.selected_witness_id =
        witness != witness_id_by_fact.end() ? witness->second
                                            : core::ToString(fact.fact_id);
    s = AppendBinding(bindings, binding);
    if (!s.ok()) {
      return rollback(s);
    }
  }

  // The witness DAG: one node per selected proof, one edge per derivation step.
  ProvenanceStore provenance(metadata_store_);
  for (const auto& [result_key, entry] : result_witnesses) {
    auto result_fact_id =
        DeriveFactId(entry.ordered_edges.front().edge->result.row);
    if (!result_fact_id.ok()) {
      return rollback(result_fact_id.status());
    }

    FactWitness node;
    node.run_id = batch.run.run_id;
    node.output_fact_id = *result_fact_id;
    node.witness_id = entry.witness_id;
    node.selected = true;
    node.producer_kind = ProducerKindForEngine(batch.run.engine);
    node.rule_id = entry.ordered_edges.front().edge->rule_id;
    // Populate provenance metadata from a rooted input's structured evidence,
    // so the explanation graph reports source anchors and summaries.
    for (const WitnessEdgeRef& edge_ref : entry.ordered_edges) {
      const auto root = root_evidence.find(edge_ref.input_fact_id);
      if (root != root_evidence.end()) {
        node.producer_id = root->second->producer_id;
        node.source_anchor_id = root->second->source_anchor_id;
        node.summary_id = root->second->summary_id;
        node.description = root->second->description;
        break;
      }
    }
    s = provenance.AddNode(node);
    if (!s.ok()) {
      return rollback(s);
    }

    for (const WitnessEdgeRef& edge_ref : entry.ordered_edges) {
      const WitnessEdge& edge = *edge_ref.edge;
      FactWitnessEdge witness_edge;
      witness_edge.run_id = batch.run.run_id;
      witness_edge.output_fact_id = *result_fact_id;
      witness_edge.witness_id = entry.witness_id;
      witness_edge.input_kind =
          rooted_inputs.count(edge_ref.input_fact_id) ? "rooted" : "derived";
      witness_edge.input_id = core::ToString(edge_ref.input_fact_id);
      witness_edge.input_ordinal = edge.input_ordinal;
      s = provenance.AddEdge(witness_edge);
      if (!s.ok()) {
        return rollback(s);
      }
    }
  }
  s = bindings.Flush();
  if (!s.ok()) {
    return rollback(s);
  }
  s = provenance.Flush();
  if (!s.ok()) {
    return rollback(s);
  }

  // Commit the receipt with the facts, bindings, and witnesses in one
  // transaction, so a crash leaves neither a receipt nor partial facts.
  s = metadata_store_.Execute(
      "INSERT INTO fact_batch_receipts (run_id, batch_id, wpa_run_id) "
      "VALUES (?, ?, ?)",
      {run_id, batch_id, run_id});
  if (!s.ok()) {
    return rollback(s);
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
