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

#include "veritas/facts/AnalysisFactBus.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/core/RunMetrics.h"
#include "veritas/facts/Witness.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::facts {
namespace {

// Canonical, length-prefixed field encoding; the same self-delimiting scheme
// the logical input hash and canonicalizer use, so concatenation is injective.
void UpdateHash(core::SHA256Hasher *hasher, std::string_view value) {
  hasher->Update(std::as_bytes(std::span(value.data(), value.size())));
}

void AppendField(core::SHA256Hasher *hasher, std::string_view value) {
  UpdateHash(hasher, std::to_string(value.size()));
  UpdateHash(hasher, ":");
  UpdateHash(hasher, value);
}

// Human-readable rendering of a semantic row, for diagnostics only. A rejected
// batch has to name the row that caused the rejection: an opaque fact id and a
// bare "duplicate" leaves nothing to act on.
std::string RenderRow(const SemanticRow &row) {
  std::string out(RelationsV2().Get(row.relation).name);
  for (const auto &cell : row.cells) {
    out.push_back(' ');
    std::visit(
        [&out](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::StableId>) {
            out.append(core::ToString(value));
          } else if constexpr (std::is_same_v<T, std::string>) {
            out.append(value);
          } else if constexpr (std::is_same_v<T, std::int64_t> ||
                               std::is_same_v<T, std::uint64_t>) {
            out.append(std::to_string(value));
          } else {
            out.append(std::to_string(static_cast<int>(value)));
          }
        },
        cell);
  }
  return out;
}

// FNV-1a, 64-bit: the offset basis and the prime. A key hash is a bucket
// selector rather than an identity -- the fact index resolves every bucket by
// comparing stored rows, so a collision costs a comparison and never an answer
// -- which is why a cheap, well-spread mix is the right tool here and a
// cryptographic digest would cost more per row than the index it feeds saves.
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

// Appends one stored row's canonical key to the batch-id stream, rendered from
// the cells its arena holds. `render` is the arena-named accessor for that
// arena, so a caller cannot render a fact row against the witness arena or the
// reverse.
//
// The render cannot fail for a handle the batch's own index produced: the bytes
// it names are bytes that arena wrote, and a row one arena accepted renders the
// same way a second time. The assert states that, and a render that somehow
// failed leaves an empty field, which no stored row renders -- so the derived
// id moves and the batch-id gate rejects the batch rather than accepting an id
// that covers a row the batch does not have. That is the same choice
// `MakeAnalysisFactBatch` makes when an append it enumerated itself fails.
void AppendStoredRowKey(core::SHA256Hasher *canonical, std::string *scratch,
                        const AnalysisFactBatch &batch,
                        const StatusOr<RowHandle> &handle,
                        Status (AnalysisFactBatch::*render)(
                            RowHandle, std::string *) const) {
  scratch->clear();
  assert(handle.ok());
  if (handle.ok()) {
    const Status rendered = (batch.*render)(*handle, scratch);
    assert(rendered.ok());
    if (!rendered.ok()) {
      scratch->clear();
    }
  }
  AppendField(canonical, *scratch);
}

constexpr std::string_view kDeliveryTableSql =
    "CREATE TABLE IF NOT EXISTS wpa_fact_bus_deliveries ("
    " run_id TEXT NOT NULL,"
    " batch_id TEXT NOT NULL,"
    " sink_id TEXT NOT NULL,"
    " PRIMARY KEY (run_id, batch_id, sink_id))";

Status EnsureDeliveryTable(summarydb::MetadataStore &store) {
  return store.Execute(std::string(kDeliveryTableSql), {});
}

Status MarkDelivered(summarydb::MetadataStore &store, std::string_view run_id,
                     std::string_view batch_id, std::string_view sink_id) {
  return store.Execute(
      "INSERT OR IGNORE INTO wpa_fact_bus_deliveries "
      "(run_id, batch_id, sink_id) VALUES (?, ?, ?)",
      {std::string(run_id), std::string(batch_id), std::string(sink_id)});
}

StatusOr<bool> IsDelivered(summarydb::MetadataStore &store,
                           std::string_view run_id, std::string_view batch_id,
                           std::string_view sink_id) {
  auto rows = store.Query(
      "SELECT COUNT(*) FROM wpa_fact_bus_deliveries "
      "WHERE run_id = ? AND batch_id = ? AND sink_id = ?",
      {std::string(run_id), std::string(batch_id), std::string(sink_id)});
  if (!rows.ok()) {
    return rows.status();
  }
  return !(*rows).empty() && (*rows)[0][0] != "0";
}

// One entry of the canonical order the emit pass produces: the rank the sorts
// order by, and where the entry itself still lives. Deliberately not a decoded
// row. Assembly reads every owned row exactly once, in canonical order, into
// the batch's own arena, so holding the row here as well would keep a second,
// rich copy of the whole payload alive across both sorts for no reader to
// consume -- which is what the batch's arena exists to delete.
//
// `source` borrows the completed component's arena. That is sound because the
// completion vector is fully sorted before this pass and never resized during
// it, and because round 4 releases no payload, so the arena outlives the
// assembly that reads it.
struct KeyedFact {
  std::uint32_t key_rank = 0;
  const RowArena *source = nullptr;
  RowHandle entry;
};

struct KeyedWitness {
  std::uint32_t result_rank = 0;
  std::uint32_t rule_rank = 0;
  std::uint32_t input_rank = 0;
  std::uint32_t input_ordinal = 0;
  const RowArena *source = nullptr;
  RowHandle entry;
};

// Dense ranks over the distinct encoded semantic keys of one assembly call,
// used in place of the keys themselves.
//
// Why ranks and not the keys: a batch is ordered by the encoded key of every
// fact row and of both endpoints of every witness edge. Held as `std::string`s
// that is ~2.75M simultaneous heap allocations on the reference fixture, and
// the ordering is then a lexicographic string comparison through two large
// comparison sorts. A dense `uint32_t` per occurrence deletes the allocations,
// the per-comparison string dereference, and the bytes the sorts move.
//
// The substitution is only sound if it is an order-isomorphism, and the claim
// is exact: ranks are assigned by ascending byte order over the distinct key
// values, so `rank(a) < rank(b)` iff the encoded bytes of `a` precede those of
// `b`, and equal keys take equal ranks. Byte order here is
// `std::char_traits<char>::compare`, the same comparison `std::string` uses, so
// ranking is not an approximation of the string comparison -- it is the same
// ordering on a smaller alphabet of symbols.
//
// Equal keys taking equal ranks is what preserves ties, and it is not the only
// thing that has to come out identical: `std::sort` permutes a sequence as a
// function of the comparison outcomes and the element count alone, so an
// isomorphic comparator over the same elements yields the same permutation,
// including the relative order of elements it cannot separate. That matters
// because `std::unique` runs on the sorted output and keeps the first element
// of each run of equals.
//
// One pool serves both the semantic keys and the rule ids. That is safe because
// the comparator compares position-wise: a rank is a monotone function of the
// bytes over the whole interned set, so two values in the same position compare
// exactly as their bytes do, and positions are never compared across domains.
class FactRanks {
public:
  // Interns `encoded_key`, returning a dense id. Ids are dense but not ranked
  // until `Finish`, and identical bytes always intern to the same id.
  std::uint32_t Intern(std::string_view encoded_key) {
    const auto it = index_.find(encoded_key);
    if (it != index_.end()) {
      return it->second;
    }
    const std::uint32_t id = static_cast<std::uint32_t>(keys_.size());
    char *const stored = Allocate(encoded_key.size());
    std::memcpy(stored, encoded_key.data(), encoded_key.size());
    const std::string_view view(stored, encoded_key.size());
    keys_.push_back(view);
    index_.emplace(view, id);
    return id;
  }

  // Ranks every interned key by ascending byte order and releases the key bytes
  // and the lookup index, which are dead the moment the ranks exist. Releasing
  // them here rather than at destruction keeps ~0.3 GiB from sitting underneath
  // the two sorts that consume the ranks.
  //
  // No `Intern` may follow this call. `chunks_`, `keys_`, and `index_` are
  // released but `ranks_` is not, so a later `Intern` would restart ids at 0
  // while `Rank` reads the stale, too-short `ranks_` those new ids index past.
  void Finish() {
    std::vector<std::uint32_t> order(keys_.size());
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::ranges::sort(order, {}, [this](std::uint32_t id) { return keys_[id]; });
    ranks_.assign(keys_.size(), 0);
    std::uint32_t rank = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (i != 0 && keys_[order[i]] != keys_[order[i - 1]]) {
        ++rank;
      }
      ranks_[order[i]] = rank;
    }
    std::vector<std::string_view>().swap(keys_);
    std::unordered_map<std::string_view, std::uint32_t>().swap(index_);
    std::vector<std::vector<char>>().swap(chunks_);
    used_ = 0;
  }

  // The rank of the key interned as `id`. Only valid after `Finish`.
  std::uint32_t Rank(std::uint32_t id) const { return ranks_[id]; }

private:
  // Bump allocation out of fixed-size chunks. Offsets into one big buffer would
  // be smaller, but the interned views must survive the buffer's growth, and a
  // chunk that is never reallocated gives them somewhere stable to point.
  char *Allocate(std::size_t bytes) {
    constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
    if (chunks_.empty() || used_ + bytes > chunks_.back().size()) {
      chunks_.emplace_back(bytes > kChunkBytes ? bytes : kChunkBytes);
      used_ = 0;
    }
    char *const out = chunks_.back().data() + used_;
    used_ += bytes;
    return out;
  }

  std::vector<std::vector<char>> chunks_;
  std::size_t used_ = 0;
  std::vector<std::string_view> keys_;
  std::unordered_map<std::string_view, std::uint32_t> index_;
  std::vector<std::uint32_t> ranks_;
};

} // namespace

std::uint64_t DefaultKeyHash(const core::StableId &id) {
  // An id is already a digest of the row's canonical preimage, so reducing it
  // to 64 bits is a pure function of the row that costs nothing the row has not
  // paid. FNV over the canonical text the id carries, which is one pass over 64
  // characters and mixes the whole digest; parsing those characters into digest
  // bytes to read a slice of them would cost more and spread no better.
  std::uint64_t hash = kFnvOffsetBasis;
  for (char c : id.digest_hex) {
    hash ^= static_cast<std::uint8_t>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

core::StableId DeriveBatchId(const AnalysisFactBatch &batch) {
  core::SHA256Hasher canonical;
  // One scratch key for every row in the batch. The batch id covers a million
  // facts and more than a million witness endpoints, so a fresh encoding string
  // per row would be millions of allocations whose only purpose is to be handed
  // straight to the hash.
  std::string key;
  UpdateHash(&canonical, "veritas.analysis-fact-batch.v2");
  AppendField(&canonical, core::ToString(batch.run.run_id));
  for (const auto &component : batch.expected_components) {
    AppendField(&canonical, core::ToString(component.scc_id));
    AppendField(&canonical,
                std::to_string(static_cast<int>(component.component)));
  }
  for (const auto &completion : batch.completed_components) {
    AppendField(&canonical, core::ToString(completion.key.scc_id));
    AppendField(&canonical,
                std::to_string(static_cast<int>(completion.key.component)));
    AppendField(&canonical, completion.result_object_key);
    AppendField(&canonical, completion.result.logical_input_hash);
    AppendField(&canonical, completion.result.fixpoint_hash);
    AppendField(&canonical, completion.result.external_hash);
  }
  for (const auto &id : batch.rooted_input_fact_ids) {
    AppendField(&canonical, core::ToString(id));
  }
  // The rows are rendered from the batch's own arenas, not decoded: `AppendKey`
  // produces exactly the bytes `AppendSemanticKey` produces for the rich row,
  // and this function runs twice per run over every fact and both endpoints of
  // every witness, so a decode per row is precisely the cost the batch's arenas
  // exist to remove.
  for (std::size_t i = 0; i < batch.fact_count(); ++i) {
    AppendStoredRowKey(&canonical, &key, batch, batch.fact_row_handle_at(i),
                       &AnalysisFactBatch::AppendFactKey);
  }
  for (std::size_t i = 0; i < batch.witness_count(); ++i) {
    AppendStoredRowKey(&canonical, &key, batch,
                       batch.witness_result_row_handle_at(i),
                       &AnalysisFactBatch::AppendWitnessRowKey);
    // The rule id and the ordinal are the two witness fields that are not a
    // row. Both are read from the entry's own bytes, so no edge is decoded for
    // them either, and both fail exactly as the renders above do: asserted,
    // with an empty field on the impossible failure.
    const auto rule_id = batch.witness_rule_id_at(i);
    assert(rule_id.ok());
    std::string rule_id_text;
    if (rule_id.ok()) {
      rule_id_text = *rule_id;
    }
    AppendField(&canonical, rule_id_text);
    AppendStoredRowKey(&canonical, &key, batch,
                       batch.witness_input_row_handle_at(i),
                       &AnalysisFactBatch::AppendWitnessRowKey);
    const auto ordinal = batch.witness_ordinal_at(i);
    assert(ordinal.ok());
    std::string ordinal_text;
    if (ordinal.ok()) {
      ordinal_text = std::to_string(*ordinal);
    }
    AppendField(&canonical, ordinal_text);
  }
  for (const auto &diagnostic : batch.diagnostics) {
    AppendField(&canonical, diagnostic);
  }
  return core::StableId{core::IdKind::kFact,
                        core::DigestToHex(canonical.Finalize())};
}

AnalysisFactBatch MakeAnalysisFactBatch(wpa::WpaRunResult result) {
  AnalysisFactBatch batch;
  batch.run = std::move(result.run);
  batch.expected_components = std::move(result.expected_components);
  batch.completed_components = std::move(result.completed_components);
  batch.rooted_input_fact_ids = std::move(result.rooted_input_fact_ids);
  batch.rooted_input_facts = std::move(result.rooted_input_facts);

  std::ranges::sort(batch.expected_components);
  // Sorted before the ownership pass below, so which component owns a shared
  // fact depends on the set of completed components rather than on the order
  // the orchestrator happened to visit them in.
  std::ranges::sort(
      batch.completed_components,
      [](const auto &left, const auto &right) { return left.key < right.key; });
  std::ranges::sort(batch.rooted_input_fact_ids);

  // 1. A derived fact can be proven independently by more than one component.
  // The derived relations project away the identity that separates their
  // proofs -- `GlobalFlow(s, d) :- ParameterFlow(_, s, d, e)` discards the call
  // site -- so two callers of one callee materialise the same base row and each
  // derives and publishes the same fact. A run's provenance records exactly one
  // proof per fact, so a fact goes to the first component in canonical
  // completion order and every later component contributes neither the fact nor
  // its witness edges. Dropping the whole alternative derivation (rather than
  // the individual edges) keeps each published result backed by one well-formed
  // proof instead of a mixture of two.
  std::set<core::StableId> owned;
  // The ranks replace the ordering keys, so a fact or witness carries a
  // `uint32_t` per compared field instead of a heap string per endpoint. The
  // three scratch buffers are reused across every row: encoding a row into a
  // fresh `std::string` per occurrence is an allocation this pass does not need
  // and, at ~250 encoded bytes a row, one the small-string optimization cannot
  // absorb.
  FactRanks ranks;
  std::vector<KeyedFact> keyed_facts;
  std::vector<KeyedWitness> keyed_witnesses;
  std::string fact_key;
  std::string result_key;
  std::string input_key;
  // One reusable per-component key set, holding interned ids rather than the
  // keys: the loop runs once per component, so it does not belong inside it,
  // and set membership is the only thing it is asked.
  std::set<std::uint32_t> overridden;
  for (auto &completion : batch.completed_components) {
    overridden.clear();
    // The payload is an arena, so this pass reads it through the range and
    // records *where* each owned entry lives rather than a copy of it. The row
    // is decoded once here, for its id and its sort key, and once more in the
    // emit pass below -- in canonical order, where it is appended to the
    // batch's own arena. The component's arena keeps its own copy, because
    // round 4 releases no payload.
    const AnalysisFactRange facts_range(&completion.result.facts);
    for (auto it = facts_range.begin(); it != facts_range.end(); ++it) {
      // The decode is the iterator's, and it is transient: the row it
      // materialises is released at the end of this step, so no owned fact is
      // ever held in the rich form across the two sorts below.
      const AnalysisFact &fact = *it;
      fact_key.clear();
      AppendSemanticKey(&fact_key, fact.row);
      if (owned.insert(fact.fact_id).second) {
        keyed_facts.push_back(KeyedFact{.key_rank = ranks.Intern(fact_key),
                                        .source = &completion.result.facts,
                                        .entry = it.handle()});
      } else {
        overridden.insert(ranks.Intern(fact_key));
      }
    }
    const WitnessRange witnesses_range(&completion.result.witnesses);
    for (auto it = witnesses_range.begin(); it != witnesses_range.end(); ++it) {
      const WitnessEdge &edge = *it;
      result_key.clear();
      AppendSemanticKey(&result_key, edge.result.row);
      // The overridden set's member is the interned id, not the key bytes: the
      // set holds the facts a later component re-derived, and a witness's
      // result row is one of those facts under that same encoding.
      const std::uint32_t result_id = ranks.Intern(result_key);
      if (overridden.contains(result_id)) {
        continue;
      }
      input_key.clear();
      AppendSemanticKey(&input_key, edge.input.row);
      // The rule id is interned, not moved: the edge keeps its own, and the
      // published batch is hashed over it.
      keyed_witnesses.push_back(KeyedWitness{
          .result_rank = result_id,
          .rule_rank = ranks.Intern(edge.rule_id),
          .input_rank = ranks.Intern(input_key),
          .input_ordinal = edge.input_ordinal,
          .source = &completion.result.witnesses,
          .entry = it.handle(),
      });
    }
    for (auto &diagnostic : completion.result.diagnostics) {
      batch.diagnostics.push_back(std::move(diagnostic));
    }
    // The facts and witnesses need no release: their payload is a compact arena
    // now, so there is no rich vector buffer left behind to reclaim. The
    // diagnostics are still a rich vector, and moving each string out of one
    // leaves its buffer behind exactly as before, so that release stays.
    std::vector<std::string>().swap(completion.result.diagnostics);
  }

  // Every occurrence now holds a dense id; only the distinct keys have been
  // encoded once each. Ranking them turns the ids into the comparison domain
  // the sorts below use, and releases the key bytes and the lookup index, which
  // no longer have a reader.
  ranks.Finish();
  for (auto &keyed : keyed_facts) {
    keyed.key_rank = ranks.Rank(keyed.key_rank);
  }
  for (auto &keyed : keyed_witnesses) {
    keyed.result_rank = ranks.Rank(keyed.result_rank);
    keyed.rule_rank = ranks.Rank(keyed.rule_rank);
    keyed.input_rank = ranks.Rank(keyed.input_rank);
  }

  // 2. Canonical order, and the assembly boundary where uniqueness is
  // established rather than merely checked: sorting puts equal entries
  // adjacent, so the set is collapsed here and Validate's identity checks
  // describe a property this assembler guarantees.
  //
  // The collapse is the `std::ranges::unique` this code used to run on the
  // assembled vector, restated for an append-only arena: the arena cannot
  // erase, so an entry the vector path would have removed is never appended.
  // `unique` removes an element equal to the last element it *kept*, so both
  // loops below compare against the last entry appended rather than against the
  // previous candidate.
  //
  // Both loops decode and append. Neither step can fail for an entry this
  // function enumerated from the source arena's own index: the bytes the handle
  // names are bytes that arena wrote, and a row one arena accepted encodes the
  // same way a second time. A failure is an internal invariant violation rather
  // than input, so it asserts, and the fallback leaves the row out -- which
  // leaves its witness unclosed, or its fact unproved, and makes `Validate`
  // reject the batch rather than publish a row that was never stored.
  std::ranges::sort(keyed_facts, {}, &KeyedFact::key_rank);
  bool kept_a_fact = false;
  std::uint32_t last_fact_rank = 0;
  for (const KeyedFact &keyed : keyed_facts) {
    // Equal ranks are contiguous: the comparator orders by rank alone, so
    // entries with equal keys are equivalent under the sort. Two such entries
    // carry the same row and therefore the same fact id, which is why
    // collapsing on the rank removes exactly what the old `fact_id` comparison
    // removed.
    if (kept_a_fact && keyed.key_rank == last_fact_rank) {
      continue;
    }
    auto fact = keyed.source->DecodeFact(keyed.entry);
    assert(fact.ok());
    if (!fact.ok()) {
      continue;
    }
    auto appended = batch.facts_.AppendFact(*fact);
    assert(appended.ok());
    if (!appended.ok()) {
      continue;
    }
    last_fact_rank = keyed.key_rank;
    kept_a_fact = true;
  }
  // The same four fields, in the same order, as the comparator over the encoded
  // keys: ranks preserve the byte order of the values they stand for, so this
  // decides every pair exactly as the string comparison did, ties included.
  std::ranges::sort(keyed_witnesses,
                    [](const KeyedWitness &left, const KeyedWitness &right) {
                      return std::tie(left.result_rank, left.rule_rank,
                                      left.input_rank, left.input_ordinal) <
                             std::tie(right.result_rank, right.rule_rank,
                                      right.input_rank, right.input_ordinal);
                    });
  bool kept_an_edge = false;
  WitnessEdge last_edge;
  for (const KeyedWitness &keyed : keyed_witnesses) {
    auto edge = keyed.source->DecodeWitness(keyed.entry);
    assert(edge.ok());
    if (!edge.ok()) {
      continue;
    }
    // A witness collapses on the whole edge, not on the four fields the sort
    // orders by: `WitnessEdge::operator==` also compares `derivation_key`,
    // which the ordering does not carry, so two edges the sort cannot separate
    // are not necessarily one proof. Comparing the decoded edge against the
    // last one appended is exactly the `std::ranges::unique` this replaces.
    if (kept_an_edge && *edge == last_edge) {
      continue;
    }
    auto appended = batch.witnesses_.AppendWitness(*edge);
    assert(appended.ok());
    if (!appended.ok()) {
      continue;
    }
    last_edge = std::move(*edge);
    kept_an_edge = true;
  }
  std::ranges::sort(batch.diagnostics);

  batch.batch_id = DeriveBatchId(batch);
  return batch;
}

void AnalysisFactBatch::SetFacts(const std::vector<AnalysisFact> &facts) {
  facts_ = RowArena{};
  for (const AnalysisFact &fact : facts) {
    auto appended = facts_.AppendFact(fact);
    if (!appended.ok()) {
      // An append fails only for a row whose stable ID carries a digest that
      // is not canonical hexadecimal, and this signature has no channel to
      // report one: it replaces a payload a caller used to assign, which could
      // not fail either. Leaving the row out keeps the two arenas consistent
      // with each other -- `Validate` then rejects a batch whose witness no
      // longer closes -- rather than storing a half-written entry, and debug
      // builds assert because every other producer of a row in this pipeline
      // cannot make one that reaches here.
      assert(appended.ok());
    }
  }
}

void AnalysisFactBatch::SetWitnesses(
    const std::vector<WitnessEdge> &witnesses) {
  witnesses_ = RowArena{};
  for (const WitnessEdge &edge : witnesses) {
    auto appended = witnesses_.AppendWitness(edge);
    if (!appended.ok()) {
      // Fails exactly as `SetFacts` does, and for the same reason.
      assert(appended.ok());
    }
  }
}

void AnalysisFactBatch::ClearPayload() {
  facts_ = RowArena{};
  witnesses_ = RowArena{};
}

AnalysisFactBus::AnalysisFactBus(wpa::WpaRunRepository &delivery_state)
    : delivery_state_(delivery_state) {}

void AnalysisFactBus::AddSink(std::string sink_id, AnalysisFactSink &sink) {
  sinks_.emplace_back(std::move(sink_id), &sink);
}

Status AnalysisFactBus::Validate(const AnalysisFactBatch &batch) const {
  // The supplied batch id must equal the recomputed canonical id, so a tampered
  // or mis-assembled batch is rejected before it reaches any sink.
  if (batch.batch_id != DeriveBatchId(batch)) {
    return Status::FailedPrecondition(
        "supplied batch_id does not match the canonical id");
  }

  // Exact expected/completed component equality, with no duplicates.
  std::set<wpa::WpaComponentKey> expected(batch.expected_components.begin(),
                                          batch.expected_components.end());
  if (expected.size() != batch.expected_components.size()) {
    return Status::FailedPrecondition("duplicate expected component");
  }
  std::set<wpa::WpaComponentKey> completed;
  for (const auto &completion : batch.completed_components) {
    if (!completed.insert(completion.key).second) {
      return Status::FailedPrecondition("duplicate completed component");
    }
  }
  if (expected != completed) {
    return Status::FailedPrecondition(
        "expected and completed component sets differ");
  }

  // Stable fact identity: every fact's ID matches its semantic row, and no two
  // facts share an identity. The index below enforces the second by comparing
  // rows rather than ids, which the first check makes the same statement: an id
  // is a function of its row, so two facts sharing a row share an id.
  //
  // One identity memo serves this loop and both endpoints of every witness
  // edge below: the witness rows are drawn from exactly the rows the fact list
  // already carries, so a batch that derives per occurrence derives the same
  // row's identity three times over. The memo changes no comparison -- a hit
  // returns the value `DeriveFactId` returned for that same row -- and it
  // caches only successful derivations, so a row that fails validation fails
  // identically whether or not it has been seen before.
  // The payload is an arena, so a fact is addressed by its position and read
  // through the range. Every check below is the one this pass has always made,
  // in the same order: the position a fact is indexed at is its position in the
  // range, which was its index in the vector.
  //
  // The index is keyed on a 64-bit reduction of the row's identity and resolved
  // by comparing stored rows, never on the identity itself. An identity is a
  // 64-character string, so a container keyed on one allocates a string per
  // fact and compares those strings on every lookup, and this pass indexes
  // every fact and looks up both endpoints of every witness. The reduction
  // decides nothing: two rows can reduce alike, and the bucket is resolved by
  // the exact comparison the arenas support, which is what lets a test force
  // every row into one bucket and still require these same answers.
  FactIdentityMemo identity;
  std::unordered_map<std::uint64_t, std::vector<std::size_t>> fact_index;
  std::size_t position = 0;
  for (const auto &fact : batch.facts()) {
    auto derived = identity.Identify(fact.row);
    if (!derived.ok()) {
      return derived.status();
    }
    if (*derived != fact.fact_id) {
      return Status::FailedPrecondition("fact_id does not match its row");
    }
    auto row = batch.fact_row_handle_at(position);
    if (!row.ok()) {
      return row.status();
    }
    std::vector<std::size_t> &bucket = fact_index[key_hash_(*derived)];
    bool duplicate = false;
    for (std::size_t indexed : bucket) {
      auto indexed_row = batch.fact_row_handle_at(indexed);
      if (!indexed_row.ok()) {
        return indexed_row.status();
      }
      if (batch.FactRowsEqual(*indexed_row, *row)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      return Status::FailedPrecondition("duplicate fact_id " +
                                        core::ToString(fact.fact_id) +
                                        " for row " + RenderRow(fact.row));
    }
    bucket.push_back(position);
    ++position;
  }

  // Rooted witness closure: every published fact has a derivation, and every
  // witness leaf is either another published fact or a declared rooted input.
  const std::set<core::StableId> roots(batch.rooted_input_fact_ids.begin(),
                                       batch.rooted_input_fact_ids.end());
  constexpr std::size_t kNoFact = std::numeric_limits<std::size_t>::max();
  struct WitnessEndpoints {
    std::size_t result = kNoFact;
    std::size_t input = kNoFact;
  };
  // The published fact a row names, or `kNoFact`. A derived id lands in a
  // bucket, and the bucket is answered by comparing rows: the position a
  // candidate names carries a stored row, and the entry that matches the
  // witness's endpoint byte for byte is the one that answers. Both sides are
  // addressed as rows rather than compared as decoded values -- the published
  // row lives in the batch's fact arena and the endpoint row in its witness
  // arena, and `RowsEqual` routes each handle to its own arena -- and the
  // comparison decides exactly what the structural comparison decided, because
  // a row is stored in its canonical, injective encoding. A bucket that holds
  // no such entry, including a bucket two distinct rows reduced into, reports
  // the same absence the identity lookup reported.
  const auto published_fact = [&](const core::StableId &id,
                                  RowHandle endpoint_row)
      -> StatusOr<std::size_t> {
    const auto bucket = fact_index.find(key_hash_(id));
    if (bucket == fact_index.end()) {
      return kNoFact;
    }
    for (std::size_t indexed : bucket->second) {
      auto published = batch.fact_row_handle_at(indexed);
      if (!published.ok()) {
        return published.status();
      }
      if (batch.RowsEqual(*published, endpoint_row)) {
        return indexed;
      }
    }
    return kNoFact;
  };

  std::vector<WitnessEndpoints> endpoints;
  endpoints.reserve(batch.witness_count());
  std::vector<bool> witnessed(batch.fact_count(), false);
  bool input_outside_root_set = false;
  bool result_outside_published_set = false;
  const WitnessRange witness_range = batch.witnesses();
  for (auto it = witness_range.begin(); it != witness_range.end(); ++it) {
    const WitnessEdge &edge = *it;
    auto result = identity.Identify(edge.result.row);
    if (!result.ok()) {
      return result.status();
    }
    auto input = identity.Identify(edge.input.row);
    if (!input.ok()) {
      return input.status();
    }

    WitnessEndpoints refs;
    auto result_fact = published_fact(*result, it.result_row_handle());
    if (!result_fact.ok()) {
      return result_fact.status();
    }
    if (*result_fact == kNoFact) {
      result_outside_published_set = true;
    } else {
      refs.result = *result_fact;
      witnessed[refs.result] = true;
    }

    auto input_fact = published_fact(*input, it.input_row_handle());
    if (!input_fact.ok()) {
      return input_fact.status();
    }
    if (*input_fact != kNoFact) {
      refs.input = *input_fact;
    } else if (!roots.contains(*input)) {
      input_outside_root_set = true;
    }
    endpoints.push_back(refs);
  }
  for (bool has_witness : witnessed) {
    if (!has_witness) {
      return Status::FailedPrecondition("fact without a closed witness");
    }
  }
  if (input_outside_root_set) {
    return Status::FailedPrecondition("witness leaf outside the root set");
  }
  if (result_outside_published_set) {
    return Status::FailedPrecondition("witness result is not a published fact");
  }

  // The witness DAG must be acyclic: every published fact's proof is a finite
  // tree rooted in declared inputs. A cycle would let a fact justify itself.
  //
  // The edges are two flat arrays and an offset table rather than a vector per
  // fact: a batch holds a position per derived row, so a vector of vectors
  // allocates once per fact and most of those vectors are empty, where the
  // targets and the offsets are one block each. The slice
  // `depend_target[depend_offset[i] .. depend_offset[i + 1])` is exactly the
  // list of results fact `i` proves, so the traversal below is the one this
  // pass has always run, over index arithmetic instead of a nested vector.
  std::vector<std::size_t> depend_offset(batch.fact_count() + 1, 0);
  std::vector<std::size_t> depend_target;
  std::vector<std::size_t> input_count(batch.fact_count(), 0);
  for (const auto &edge : endpoints) {
    if (edge.input != kNoFact) {
      ++depend_offset[edge.input + 1];
      ++input_count[edge.result];
    }
  }
  for (std::size_t i = 0; i < batch.fact_count(); ++i) {
    depend_offset[i + 1] += depend_offset[i];
  }
  depend_target.resize(depend_offset.back());
  // The fill replays the pass that counted: `cursor` is a private copy of the
  // offsets, so each input's slice fills from its own start and a slice keeps
  // the order the edges were recorded in.
  std::vector<std::size_t> cursor(depend_offset.begin(),
                                  depend_offset.end() - 1);
  for (const auto &edge : endpoints) {
    if (edge.input != kNoFact) {
      depend_target[cursor[edge.input]++] = edge.result;
    }
  }
  std::vector<std::size_t> ready;
  ready.reserve(batch.fact_count());
  for (std::size_t i = 0; i < input_count.size(); ++i) {
    if (input_count[i] == 0) {
      ready.push_back(i);
    }
  }
  std::size_t processed = 0;
  for (std::size_t i = 0; i < ready.size(); ++i) {
    ++processed;
    const std::size_t fact = ready[i];
    for (std::size_t at = depend_offset[fact]; at < depend_offset[fact + 1];
         ++at) {
      const std::size_t dependent = depend_target[at];
      if (--input_count[dependent] == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (processed != input_count.size()) {
    return Status::FailedPrecondition("witness DAG contains a cycle");
  }

  return Status::Ok();
}

Status AnalysisFactBus::Publish(const AnalysisFactBatch &batch) const {
  Status valid = [&] {
    core::PhaseSpan span(metrics_, "facts.publish.validate",
                         core::SpanMode::kBearing);
    return Validate(batch);
  }();
  if (!valid.ok()) {
    return valid;
  }

  summarydb::MetadataStore &store = delivery_state_.metadata_store();
  Status schema = EnsureDeliveryTable(store);
  if (!schema.ok()) {
    return schema;
  }

  const std::string run_id = core::ToString(batch.run.run_id);
  const std::string batch_id = core::ToString(batch.batch_id);

  for (const auto &[sink_id, sink] : sinks_) {
    auto delivered = IsDelivered(store, run_id, batch_id, sink_id);
    if (!delivered.ok()) {
      return delivered.status();
    }
    if (*delivered) {
      continue;
    }
    Status publish = [&] {
      core::PhaseSpan span(metrics_, "facts.publish.sink." + sink_id,
                           core::SpanMode::kBearing);
      return sink->Publish(batch);
    }();
    if (!publish.ok()) {
      return publish;
    }
    Status mark = MarkDelivered(store, run_id, batch_id, sink_id);
    if (!mark.ok()) {
      return mark;
    }
  }
  return Status::Ok();
}

} // namespace veritas::facts
