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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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
//
// The edge itself is addressed by its *position* in the batch's witness arena,
// not by a pointer to it. The batch holds its payload compactly and the range
// that reads it yields one decoded edge per step, so the address of a witness
// read in the grouping pass is the address of a temporary that dies at the end
// of the step that produced it. A position in an append-only arena is stable
// and `AnalysisFactBatch::witness_at` decodes it again, which is what lets the
// grouping pass finish before the rows are read. The ordinal is retained
// alongside because ordering a group's edges is the only use that needs it, and
// it is four bytes where a row is hundreds.
struct WitnessEdgeRef {
  std::size_t position = 0;
  std::uint32_t input_ordinal = 0;
  core::StableId input_fact_id;
};

// The encoded result key of every witness edge, interned to a dense id and
// ranked into the keys' own byte order.
//
// The grouping is the largest container cost in this pass: a run publishes
// well over a million witness edges, one encoded result key each, and the
// string-keyed map this replaces compared those keys on every level of every
// insert. Interning each distinct key once means a lookup compares dense ids
// instead, and the rank is what the groups are visited in: a rank is a key's
// position in ascending byte order, which is the order the map iterated in, so
// the published rows keep the order they had.
//
// Ids are dense in arrival order, so a caller holding one can size its group
// vector from it. `Finish` is the last mutation -- nothing may intern or read a
// key after it -- and it releases the key bytes and the lookup index, which are
// dead the moment the ranks exist.
class ResultRanks {
 public:
  // The id of `key`, assigning the next dense id the first time it is seen.
  std::uint32_t Intern(std::string_view key) {
    const auto found = index_.find(key);
    if (found != index_.end()) {
      return found->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(keys_.size());
    char* const stored = Allocate(key.size());
    std::memcpy(stored, key.data(), key.size());
    const std::string_view view(stored, key.size());
    keys_.push_back(view);
    index_.emplace(view, id);
    return id;
  }

  // Fixes every id's rank and releases the keys. Nothing may intern after this.
  void Finish() {
    by_rank_.resize(keys_.size());
    std::iota(by_rank_.begin(), by_rank_.end(), std::uint32_t{0});
    std::ranges::sort(by_rank_, {},
                      [this](std::uint32_t id) { return keys_[id]; });
    std::vector<std::string_view>().swap(keys_);
    std::unordered_map<std::string_view, std::uint32_t>().swap(index_);
    std::vector<Chunk>().swap(chunks_);
    used_ = 0;
  }

  // The number of distinct interned keys. Only valid after `Finish`.
  std::uint32_t Count() const {
    return static_cast<std::uint32_t>(by_rank_.size());
  }

  // The id whose key sorts at `rank`, so a caller can visit the keys in order.
  // Only valid after `Finish`.
  std::uint32_t IdAtRank(std::uint32_t rank) const { return by_rank_[rank]; }

 private:
  // A bump-allocated block of key bytes. A raw array rather than a
  // `std::vector<char>`: a vector value-initializes every element on the way in
  // and destroys every element on the way out, which is two passes over a
  // megabyte per interner.
  struct Chunk {
    std::unique_ptr<char[]> bytes;
    std::size_t size = 0;
  };

  // Bump allocation out of fixed-size chunks. Offsets into one big buffer would
  // be smaller, but the interned views must survive the buffer's growth, and a
  // chunk that is never reallocated gives them somewhere stable to point.
  char* Allocate(std::size_t bytes) {
    constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
    if (chunks_.empty() || used_ + bytes > chunks_.back().size) {
      const std::size_t capacity = bytes > kChunkBytes ? bytes : kChunkBytes;
      chunks_.push_back(
          Chunk{std::unique_ptr<char[]>(new char[capacity]), capacity});
      used_ = 0;
    }
    char* const out = chunks_.back().bytes.get() + used_;
    used_ += bytes;
    return out;
  }

  std::vector<Chunk> chunks_;
  std::size_t used_ = 0;
  std::vector<std::string_view> keys_;
  std::unordered_map<std::string_view, std::uint32_t> index_;
  std::vector<std::uint32_t> by_rank_;
};

// The witness-dependent derivation identity (design §7): the semantic key of
// the result, the rule that derived it, and its ordered input semantic keys.
// Distinct derivations of the same fact produce distinct witness ids, while
// the semantic FactID stays witness-independent.
//
// The fields stream straight into the hash. Building the whole byte string
// first cost one large allocation per derivation and, on the caller's side, a
// copy of every input row just to reach the encoder.
//
// Every row is rendered from the batch's own arena, by handle, one field at a
// time: the arena produces exactly the bytes `AppendSemanticKey` produces for
// the rich row, and a decode per edge would materialise both of an edge's rows
// -- the input and the result -- for the two fields this hash is computed
// over. Retaining those keys from the grouping pass instead would be a copy of
// every input row, which is the representation this round removes.
StatusOr<std::string>
DeriveWitnessId(const AnalysisFactBatch& batch,
                const std::vector<WitnessEdgeRef>& ordered_edges) {
  const std::size_t front = ordered_edges.front().position;
  auto result_row = batch.witness_result_row_handle_at(front);
  if (!result_row.ok()) {
    return result_row.status();
  }
  auto rule_id = batch.witness_rule_id_at(front);
  if (!rule_id.ok()) {
    return rule_id.status();
  }
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
  if (Status rendered = batch.AppendWitnessRowKey(*result_row, &key);
      !rendered.ok()) {
    return rendered;
  }
  append_field(key);
  append_field(*rule_id);
  for (const WitnessEdgeRef& ref : ordered_edges) {
    auto input_row = batch.witness_input_row_handle_at(ref.position);
    if (!input_row.ok()) {
      return input_row.status();
    }
    key.clear();
    if (Status rendered = batch.AppendWitnessRowKey(*input_row, &key);
        !rendered.ok()) {
      return rendered;
    }
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
  // witness input, which may be a rooted input absent from batch.facts(). Any
  // validation failure here happens before a transaction opens, so no rollback
  // is needed.
  //
  // Membership is keyed on the identity's digest, which is the whole of what
  // distinguishes one of these ids from another: every id here is a fact id,
  // and a fact id's kind is always `kFact` because `DeriveFactId` derived it.
  // The set is asked a membership question per witness endpoint, so the
  // byte-wise comparisons down a tree are what its hashing replaces.
  std::unordered_set<std::string> fact_ids;
  for (const AnalysisFact& fact : batch.facts()) {
    fact_ids.insert(fact.fact_id.digest_hex);
  }
  std::vector<AnalysisFact> missing_input_facts;
  std::set<core::StableId> rooted_inputs(batch.rooted_input_fact_ids.begin(),
                                         batch.rooted_input_fact_ids.end());

  struct ResultWitness {
    std::string witness_id;
    // The rule of the group's first edge in canonical order, which is what both
    // the derivation identity and the provenance node name. Read off the edge
    // the group loop decodes once, so the write loop below decodes nothing.
    std::string rule_id;
    // The result's semantic fact id, derived once where the group is first
    // filled in and reused by the provenance pass below, which needs the same
    // value for the node and every edge of this proof.
    core::StableId fact_id{};
    std::vector<WitnessEdgeRef> ordered_edges;
  };
  // Input rows repeat: a rooted input is the leaf of every proof that reaches
  // it, and a derived fact is an input wherever another rule cites it. One memo
  // derives each distinct input row once instead of once per citing edge.
  FactIdentityMemo input_identity;
  // One group per interned result id, so this vector is indexed by that id and
  // grows to it as the pass discovers results. Ranks, not ids, are what the
  // passes below visit the groups in.
  ResultRanks result_ranks;
  std::vector<ResultWitness> result_witnesses;
  // One scratch key for every edge: an encoded key per edge handed to the
  // interner is an allocation per edge otherwise.
  std::string result_key;
  const WitnessRange batch_witnesses = batch.witnesses();
  std::size_t position = 0;
  for (auto it = batch_witnesses.begin(); it != batch_witnesses.end();
       ++it, ++position) {
    const WitnessEdge& edge = *it;
    auto input_fact_id = input_identity.Identify(edge.input.row);
    if (!input_fact_id.ok()) {
      return input_fact_id.status();
    }
    // The row is only copied for an input that is not already a published fact,
    // which is the only case that has to be stored from here.
    if (fact_ids.insert(input_fact_id->digest_hex).second) {
      missing_input_facts.push_back(
          AnalysisFact{*input_fact_id, edge.input.row});
    }
    result_key.clear();
    AppendSemanticKey(&result_key, edge.result.row);
    const std::uint32_t result_id = result_ranks.Intern(result_key);
    // Ids are dense in arrival order and each is handed out once, where it is
    // created, so a fresh id is the one past the last group and every later
    // edge of the same result finds its group here. Growing is conditional
    // because the ids repeat: an unconditional resize would shrink the vector
    // back to a repeated id and drop the groups past it.
    if (result_id >= result_witnesses.size()) {
      result_witnesses.resize(result_id + 1);
    }
    result_witnesses[result_id].ordered_edges.push_back(
        WitnessEdgeRef{.position = position,
                       .input_ordinal = edge.input_ordinal,
                       .input_fact_id = std::move(*input_fact_id)});
  }
  result_ranks.Finish();

  // Group the canonical witnesses by result and derive each result's
  // witness-dependent derivation identity. The selected proof's witness id is
  // distinct from the semantic FactID, so re-deriving a fact by a different
  // proof retains a distinct witness record.
  //
  // The binding pass below asks which group a published fact's proof is in. It
  // is answered with the group's own position rather than a second copy of its
  // witness id, and keyed on a view into the group's id, which this loop
  // assigns once and the vector holds for the rest of the call: the group
  // vector is sized by the interning pass and never grows again.
  std::unordered_map<std::string_view, std::uint32_t> group_by_fact;
  for (std::uint32_t rank = 0; rank < result_ranks.Count(); ++rank) {
    const std::uint32_t result_id = result_ranks.IdAtRank(rank);
    ResultWitness& entry = result_witnesses[result_id];
    std::ranges::sort(entry.ordered_edges, [](const auto& a, const auto& b) {
      return a.input_ordinal < b.input_ordinal;
    });
    auto witness_id = DeriveWitnessId(batch, entry.ordered_edges);
    if (!witness_id.ok()) {
      return witness_id.status();
    }
    entry.witness_id = std::move(*witness_id);
    // The group's result row is the same for every edge of the group -- that is
    // what grouped them -- so the front edge names it, and one decode answers
    // both the derived fact's identity and the rule the node carries.
    auto front = batch.witness_at(entry.ordered_edges.front().position);
    if (!front.ok()) {
      return front.status();
    }
    auto fact_id = DeriveFactId(front->result.row);
    if (!fact_id.ok()) {
      return fact_id.status();
    }
    entry.fact_id = *fact_id;
    entry.rule_id = std::move(front->rule_id);
    // Assigning rather than inserting keeps the last group to name a fact as
    // the one the binding selects, which is what the key-ordered map did.
    group_by_fact.insert_or_assign(entry.fact_id.digest_hex, result_id);
  }

  // Rooted evidence is keyed on the rooted input's own id digest, read from the
  // batch, which outlives every lookup below.
  std::unordered_map<std::string_view, const RootedInputFact*> root_evidence;
  for (const auto& root : batch.rooted_input_facts) {
    root_evidence.insert_or_assign(root.fact.fact_id.digest_hex, &root);
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
  for (const AnalysisFact& fact : batch.facts()) {
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
  for (const AnalysisFact& fact : batch.facts()) {
    RunFactBinding binding;
    binding.run_id = batch.run.run_id;
    binding.fact_id = fact.fact_id;
    binding.producer_kind = ProducerKindForEngine(batch.run.engine);
    binding.is_current = true;
    const auto witness = group_by_fact.find(fact.fact_id.digest_hex);
    binding.selected_witness_id =
        witness != group_by_fact.end()
            ? result_witnesses[witness->second].witness_id
            : core::ToString(fact.fact_id);
    s = AppendBinding(bindings, binding);
    if (!s.ok()) {
      return rollback(s);
    }
  }

  // The witness DAG: one node per selected proof, one edge per derivation step.
  // The groups are visited by ascending rank, which is their results' encoded
  // keys' own byte order -- the order the map this replaces iterated in -- so
  // the rows are inserted in the order they have always been inserted in.
  ProvenanceStore provenance(metadata_store_);
  for (std::uint32_t rank = 0; rank < result_ranks.Count(); ++rank) {
    const ResultWitness& entry =
        result_witnesses[result_ranks.IdAtRank(rank)];
    const core::StableId& result_fact_id = entry.fact_id;

    FactWitness node;
    node.run_id = batch.run.run_id;
    node.output_fact_id = result_fact_id;
    node.witness_id = entry.witness_id;
    node.selected = true;
    node.producer_kind = ProducerKindForEngine(batch.run.engine);
    node.rule_id = entry.rule_id;
    // Populate provenance metadata from a rooted input's structured evidence,
    // so the explanation graph reports source anchors and summaries.
    for (const WitnessEdgeRef& edge_ref : entry.ordered_edges) {
      const auto root =
          root_evidence.find(edge_ref.input_fact_id.digest_hex);
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
      FactWitnessEdge witness_edge;
      witness_edge.run_id = batch.run.run_id;
      witness_edge.output_fact_id = result_fact_id;
      witness_edge.witness_id = entry.witness_id;
      witness_edge.input_kind =
          rooted_inputs.count(edge_ref.input_fact_id) ? "rooted" : "derived";
      witness_edge.input_id = core::ToString(edge_ref.input_fact_id);
      witness_edge.input_ordinal = edge_ref.input_ordinal;
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
