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
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
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

struct KeyedFact {
  std::uint32_t key_rank = 0;
  AnalysisFact fact;
};

struct KeyedWitness {
  std::uint32_t result_rank = 0;
  std::uint32_t rule_rank = 0;
  std::uint32_t input_rank = 0;
  std::uint32_t input_ordinal = 0;
  WitnessEdge edge;
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
  for (const auto &fact : batch.facts) {
    key.clear();
    AppendSemanticKey(&key, fact.row);
    AppendField(&canonical, key);
  }
  for (const auto &edge : batch.witnesses) {
    key.clear();
    AppendSemanticKey(&key, edge.result.row);
    AppendField(&canonical, key);
    AppendField(&canonical, edge.rule_id);
    key.clear();
    AppendSemanticKey(&key, edge.input.row);
    AppendField(&canonical, key);
    AppendField(&canonical, std::to_string(edge.input_ordinal));
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
    for (auto &fact : completion.result.facts) {
      fact_key.clear();
      AppendSemanticKey(&fact_key, fact.row);
      if (owned.insert(fact.fact_id).second) {
        keyed_facts.push_back(KeyedFact{.key_rank = ranks.Intern(fact_key),
                                        .fact = std::move(fact)});
      } else {
        overridden.insert(ranks.Intern(fact_key));
      }
    }
    for (auto &edge : completion.result.witnesses) {
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
          .edge = std::move(edge),
      });
    }
    for (auto &diagnostic : completion.result.diagnostics) {
      batch.diagnostics.push_back(std::move(diagnostic));
    }
    // Release the stripped payload vectors, not merely their elements. Moving
    // each row out empties the row, but the vector keeps the buffer that held
    // it; across thirteen thousand components that retained capacity is a
    // second copy of the whole payload living until the batch is destroyed.
    std::vector<AnalysisFact>().swap(completion.result.facts);
    std::vector<WitnessEdge>().swap(completion.result.witnesses);
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
  std::ranges::sort(keyed_facts, {}, &KeyedFact::key_rank);
  batch.facts.reserve(keyed_facts.size());
  for (auto &keyed : keyed_facts) {
    batch.facts.push_back(std::move(keyed.fact));
  }
  std::vector<KeyedFact>().swap(keyed_facts);
  batch.facts.erase(std::ranges::unique(batch.facts,
                                        [](const AnalysisFact &left,
                                           const AnalysisFact &right) {
                                          return left.fact_id == right.fact_id;
                                        })
                        .begin(),
                    batch.facts.end());
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
  batch.witnesses.reserve(keyed_witnesses.size());
  for (auto &keyed : keyed_witnesses) {
    batch.witnesses.push_back(std::move(keyed.edge));
  }
  std::vector<KeyedWitness>().swap(keyed_witnesses);
  batch.witnesses.erase(std::ranges::unique(batch.witnesses).begin(),
                        batch.witnesses.end());
  std::ranges::sort(batch.diagnostics);

  batch.batch_id = DeriveBatchId(batch);
  return batch;
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
  // facts share an ID.
  //
  // One identity memo serves this loop and both endpoints of every witness
  // edge below: the witness rows are drawn from exactly the rows the fact list
  // already carries, so a batch that derives per occurrence derives the same
  // row's identity three times over. The memo changes no comparison -- a hit
  // returns the value `DeriveFactId` returned for that same row -- and it
  // caches only successful derivations, so a row that fails validation fails
  // identically whether or not it has been seen before.
  FactIdentityMemo identity;
  std::map<core::StableId, std::size_t> fact_index;
  for (std::size_t i = 0; i < batch.facts.size(); ++i) {
    const auto &fact = batch.facts[i];
    auto derived = identity.Identify(fact.row);
    if (!derived.ok()) {
      return derived.status();
    }
    if (*derived != fact.fact_id) {
      return Status::FailedPrecondition("fact_id does not match its row");
    }
    if (!fact_index.emplace(fact.fact_id, i).second) {
      return Status::FailedPrecondition("duplicate fact_id " +
                                        core::ToString(fact.fact_id) +
                                        " for row " + RenderRow(fact.row));
    }
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
  std::vector<WitnessEndpoints> endpoints;
  endpoints.reserve(batch.witnesses.size());
  std::vector<bool> witnessed(batch.facts.size(), false);
  bool input_outside_root_set = false;
  bool result_outside_published_set = false;
  for (const auto &edge : batch.witnesses) {
    auto result = identity.Identify(edge.result.row);
    if (!result.ok()) {
      return result.status();
    }
    auto input = identity.Identify(edge.input.row);
    if (!input.ok()) {
      return input.status();
    }

    WitnessEndpoints refs;
    const auto result_it = fact_index.find(*result);
    if (result_it == fact_index.end() ||
        batch.facts[result_it->second].row != edge.result.row) {
      result_outside_published_set = true;
    } else {
      refs.result = result_it->second;
      witnessed[refs.result] = true;
    }

    const auto input_it = fact_index.find(*input);
    if (input_it != fact_index.end() &&
        batch.facts[input_it->second].row == edge.input.row) {
      refs.input = input_it->second;
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
  std::vector<std::vector<std::size_t>> dependencies(batch.facts.size());
  std::vector<std::size_t> input_count(batch.facts.size(), 0);
  for (const auto &edge : endpoints) {
    if (edge.input != kNoFact) {
      dependencies[edge.input].push_back(edge.result);
      ++input_count[edge.result];
    }
  }
  std::vector<std::size_t> ready;
  ready.reserve(batch.facts.size());
  for (std::size_t i = 0; i < input_count.size(); ++i) {
    if (input_count[i] == 0) {
      ready.push_back(i);
    }
  }
  std::size_t processed = 0;
  for (std::size_t i = 0; i < ready.size(); ++i) {
    ++processed;
    for (const auto &dependent : dependencies[ready[i]]) {
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
