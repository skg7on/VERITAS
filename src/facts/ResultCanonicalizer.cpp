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

#include "veritas/facts/ResultCanonicalizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/facts/RuleRegistry.h"

namespace veritas::facts {

namespace {

constexpr std::uint64_t kUnproven = std::numeric_limits<std::uint64_t>::max();

void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

// One interned key space: a canonical encoding in, a dense id out.
//
// The canonicalizer compares rows constantly. Both output sorts compare O(n
// log n) pairs, every witness edge names its result and its input by key, and
// the derivation fold keys on one more. A row's key is a few hundred bytes and
// two rows of one relation agree on the first few dozen of them, so one
// comparison is a byte loop over a prefix both sides already share. Interning
// each distinct key once and comparing dense ids instead is what removes that:
// no comparison in this file encodes a row any more.
//
// Ids are dense in arrival order. `Finish` ranks them in ascending byte order,
// which is the order the canonical output is defined over, so consumers order
// on `Rank` and never on `Id`. `Finish` is the last mutation -- nothing may
// intern after it -- and `Release` drops the bytes and the lookup index, which
// have no reader once every consumer holds a rank or has copied what it needs.
class KeyInterner {
 public:
  // Returned by `Find` for a key that was never interned. Callers that can see
  // an undeclared key -- a witness may name a row no one published -- have to
  // distinguish absent from present, which a dense id cannot.
  static constexpr std::uint32_t kMissing =
      std::numeric_limits<std::uint32_t>::max();

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

  // The id of `key`, or `kMissing` when it was never interned. Only valid
  // before `Finish`.
  std::uint32_t Find(std::string_view key) const {
    const auto found = index_.find(key);
    return found == index_.end() ? kMissing : found->second;
  }

  // Fixes the rank of every interned key. Rank order is the encoded keys' own
  // ascending byte order, which is what makes every later comparison -- the two
  // output sorts, the witness order, and the proof tie-break on (rule, firing
  // key) -- an integer comparison that orders rows exactly as their keys did.
  void Finish() {
    const std::size_t count = keys_.size();
    std::vector<std::uint32_t> order(count);
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::ranges::sort(order, {}, [this](std::uint32_t id) { return keys_[id]; });
    ranks_.assign(count, 0);
    by_rank_.assign(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
      // Interned keys are distinct by construction, so a rank is the position
      // the key sorts into. `by_rank_` is the inverse, which is how a rank
      // finds the bytes it stands for.
      ranks_[order[i]] = static_cast<std::uint32_t>(i);
      by_rank_[i] = order[i];
    }
  }

  // The key of rank `rank`, in ascending key order. Valid until `Release`.
  std::string_view Key(std::uint32_t rank) const {
    return keys_[by_rank_[rank]];
  }

  // The rank of the key interned as `id`. Only valid after `Finish`.
  std::uint32_t Rank(std::uint32_t id) const { return ranks_[id]; }

  // The number of distinct interned keys. Only valid after `Finish`.
  std::uint32_t Count() const {
    return static_cast<std::uint32_t>(ranks_.size());
  }

  // Frees the key bytes and the lookup index. Ranks and the rank-to-key
  // permutation survive: they are what ordering and `Key` consume, and they are
  // four bytes per key against a few hundred for the bytes.
  void Release() {
    std::vector<std::string_view>().swap(keys_);
    std::unordered_map<std::string_view, std::uint32_t>().swap(index_);
    std::vector<Chunk>().swap(chunks_);
    used_ = 0;
  }

 private:
  // A bump-allocated block of key bytes. A raw array rather than a
  // `std::vector<char>`: a vector value-initializes every element on the way in
  // and destroys every element on the way out, which is two passes over a
  // megabyte per interner -- and an interner is built per component, so the
  // uncovered region is the common case. `Intern` overwrites exactly the bytes
  // it is handed, and no reader looks past them.
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
  std::vector<std::uint32_t> ranks_;
  std::vector<std::uint32_t> by_rank_;
};

// One candidate derivation: a single rule applied to an ordered argument list.
// At most one derivation exists per (result, rule, derivation key), because a
// rule cannot bind two different inputs at the same argument position.
struct Derivation {
  // The result row this proves, as an interned row id.
  std::uint32_t result = 0;
  // The rule that produced it and the key of the single firing that did. Both
  // are interned ids while the fold builds them and ranks once every key is
  // known, because it is the ranked order of (rule, firing key) that the proof
  // tie-break is defined over.
  std::uint32_t rule = 0;
  std::uint32_t derivation = 0;
  // The rule itself, for its priority and its id. The registry is a process
  // lifetime constant, so a derivation names the rule rather than copying its
  // id string out of the interner.
  const RuleSpec* spec = nullptr;
  // Ordinal -> input row id, ascending by ordinal. Keyed by ordinal rather than
  // by input key because one input may legitimately occupy two ordinals: a
  // self-join -- a single Datalog row satisfying two argument positions of the
  // same rule -- is a legal firing, and an input-keyed map cannot represent it.
  // Every declared arity is one or two, so the vector is scanned, not indexed.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> inputs;
  std::uint64_t cost = kUnproven;
};

// What identifies a derivation while the fold is building it: the result, the
// rule, and the key of the single firing. Interned ids, so one lookup per edge
// compares three integers instead of three encoded strings.
struct DerivationKey {
  std::uint32_t result = 0;
  std::uint32_t rule = 0;
  std::uint32_t derivation = 0;

  auto operator<=>(const DerivationKey&) const = default;
};

// The order two derivations of one result are compared in once their costs and
// priorities tie: the ordered (rule, firing key) the nested maps supplied by
// iterating in key order.
bool DerivationOrder(const Derivation& left, const Derivation& right) {
  if (left.rule != right.rule) {
    return left.rule < right.rule;
  }
  return left.derivation < right.derivation;
}

// The four fields the canonical witness order is defined over, as dense ranks
// and the ordinal. Computed once where each edge is built, so the sort that
// consumes it compares integers rather than encoding two rows per comparison.
struct WitnessOrder {
  std::uint32_t result = 0;
  std::uint32_t rule = 0;
  std::uint32_t ordinal = 0;
  std::uint32_t input = 0;

  auto operator<=>(const WitnessOrder&) const = default;
};

// Reorders `values` by `keys` ascending, permuting both together. The keys were
// computed once where the entries were built, so nothing here encodes a row.
// Ties keep their relative order, which the canonical output does not depend
// on: equal keys mean equal facts and equal edges, which hash to the same
// bytes.
template <typename T, typename Key>
void SortByKeys(std::vector<T>* values, std::vector<Key>* keys) {
  std::vector<std::uint32_t> order(values->size());
  std::iota(order.begin(), order.end(), std::uint32_t{0});
  std::ranges::sort(order, {},
                    [keys](std::uint32_t index) { return (*keys)[index]; });
  std::vector<T> sorted_values;
  std::vector<Key> sorted_keys;
  sorted_values.reserve(values->size());
  sorted_keys.reserve(keys->size());
  for (std::uint32_t index : order) {
    sorted_values.push_back(std::move((*values)[index]));
    sorted_keys.push_back((*keys)[index]);
  }
  *values = std::move(sorted_values);
  *keys = std::move(sorted_keys);
}

std::string HashOf(const std::string& bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

}  // namespace

CanonicalResultHashes ComputeCanonicalResultHashes(
    std::span<const AnalysisFact> input_facts,
    std::span<const WitnessEdge> input_witnesses) {
  // The inputs are ordered here rather than trusted, so a caller cannot hash a
  // producer-dependent iteration order. Both orders are decided by interned
  // keys: the previous comparators encoded both rows of every pair they looked
  // at, while one interner encodes each distinct key once. A witness's result
  // is always a row the fact list holds, but its input is frequently a root row
  // that no fact carries, so both endpoints go through the same table.
  KeyInterner rows;
  KeyInterner labels;
  std::string key;

  std::vector<std::uint32_t> fact_ids(input_facts.size());
  for (std::size_t i = 0; i < input_facts.size(); ++i) {
    key.clear();
    AppendSemanticKey(&key, input_facts[i].row);
    fact_ids[i] = rows.Intern(key);
  }
  std::vector<WitnessOrder> witness_ids(input_witnesses.size());
  for (std::size_t i = 0; i < input_witnesses.size(); ++i) {
    const WitnessEdge& edge = input_witnesses[i];
    key.clear();
    AppendSemanticKey(&key, edge.result.row);
    const std::uint32_t result = rows.Intern(key);
    key.clear();
    AppendSemanticKey(&key, edge.input.row);
    witness_ids[i] = WitnessOrder{result, labels.Intern(edge.rule_id),
                                  edge.input_ordinal, rows.Intern(key)};
  }
  rows.Finish();
  labels.Finish();

  // Rank order is key order, and the hash is over the ordered bytes, so the
  // ranks are the whole of what the sorts below were sorting for.
  std::vector<std::uint32_t> fact_ranks(fact_ids.size());
  for (std::size_t i = 0; i < fact_ids.size(); ++i) {
    fact_ranks[i] = rows.Rank(fact_ids[i]);
  }
  std::ranges::sort(fact_ranks);
  std::vector<WitnessOrder> witness_ranks(witness_ids.size());
  for (std::size_t i = 0; i < witness_ids.size(); ++i) {
    witness_ranks[i] =
        WitnessOrder{rows.Rank(witness_ids[i].result),
                     labels.Rank(witness_ids[i].rule), witness_ids[i].ordinal,
                     rows.Rank(witness_ids[i].input)};
  }
  std::ranges::sort(witness_ranks);

  std::string external_bytes;
  AppendField(&external_bytes, "veritas.wpa.external.v1");
  for (std::uint32_t rank : fact_ranks) {
    AppendField(&external_bytes, rows.Key(rank));
  }
  CanonicalResultHashes hashes;
  hashes.external_hash = HashOf(external_bytes);

  std::string fixpoint_bytes;
  AppendField(&fixpoint_bytes, "veritas.wpa.fixpoint.v1");
  AppendField(&fixpoint_bytes, hashes.external_hash);
  for (const WitnessOrder& edge : witness_ranks) {
    AppendField(&fixpoint_bytes, rows.Key(edge.result));
    AppendField(&fixpoint_bytes, labels.Key(edge.rule));
    AppendField(&fixpoint_bytes, rows.Key(edge.input));
    AppendField(&fixpoint_bytes, std::to_string(edge.ordinal));
  }
  hashes.fixpoint_hash = HashOf(fixpoint_bytes);
  return hashes;
}

StatusOr<CanonicalizedResult> ResultCanonicalizer::Canonicalize(
    const CanonicalizationRequest& request) {
  if (request.evaluation == nullptr) {
    return Status::InvalidArgument("missing raw evaluation");
  }
  const RawWpaEvaluation& raw = *request.evaluation;

  // Every key this call compares is interned once. `rows` covers the declared
  // roots and the asserted results, which are the only rows a witness may name;
  // `labels` covers the two witness fields that are not rows. An id is a row,
  // a rule, or a firing key for the rest of this function.
  KeyInterner rows;
  KeyInterner labels;
  std::string key;
  // Row id -> the row itself. A row that is both a root and a result appears in
  // both, and the root is the one a witness cites, exactly as the two keyed
  // maps this replaces decided it.
  std::vector<const SemanticRow*> root_rows;
  std::vector<const SemanticRow*> result_rows;
  // Result index -> row id, in the order the results were asserted, which is
  // how the deduplicating map counted its distinct keys.
  std::vector<std::uint32_t> result_row_ids;

  // 1. Roots ground every proof. A root's key is what a witness edge cites.
  for (const auto& span : {request.local_roots, request.successor_roots}) {
    for (const auto& root : span) {
      auto valid = ValidateSemanticRow(root.fact.row);
      if (!valid.ok()) {
        return valid;
      }
      key.clear();
      AppendSemanticKey(&key, root.fact.row);
      const std::uint32_t id = rows.Intern(key);
      if (root_rows.size() <= id) {
        root_rows.resize(id + 1, nullptr);
        result_rows.resize(id + 1, nullptr);
      }
      // A repeated root keeps the first, which is the row the keyed map
      // returned. The rows are equal either way.
      if (root_rows[id] == nullptr) {
        root_rows[id] = &root.fact.row;
      }
    }
  }

  // 2. Index the asserted results. A witness may only speak about a result the
  // engine actually published.
  for (const auto& row : raw.results) {
    auto valid = ValidateSemanticRow(row);
    if (!valid.ok()) {
      return valid;
    }
    key.clear();
    AppendSemanticKey(&key, row);
    const std::uint32_t id = rows.Intern(key);
    if (root_rows.size() <= id) {
      root_rows.resize(id + 1, nullptr);
      result_rows.resize(id + 1, nullptr);
    }
    if (result_rows[id] == nullptr) {
      result_rows[id] = &row;
      result_row_ids.push_back(id);
    }
  }

  // 3. Fold witness edges into derivations, rejecting anything unverifiable.
  // A derivation is keyed by (result, rule, derivation key): the derivation key
  // distinguishes alternative proof steps that share a result and rule. The key
  // is three interned ids rather than three encoded strings, so the fold's one
  // lookup per edge compares integers.
  std::vector<Derivation> derivations;
  std::map<DerivationKey, std::size_t> derivation_index;
  for (const auto& edge : raw.witnesses) {
    key.clear();
    AppendSemanticKey(&key, edge.result.row);
    const std::uint32_t result = rows.Find(key);
    if (result == KeyInterner::kMissing || result_rows[result] == nullptr) {
      return Status::InvalidArgument(
          "witness names a result that was not published");
    }
    const RuleSpec* rule = RulesV2().Find(edge.rule_id);
    if (rule == nullptr) {
      return Status::InvalidArgument("witness names an unregistered rule");
    }
    if (rule->result != edge.result.row.relation) {
      return Status::InvalidArgument(
          "witness rule does not derive this relation");
    }
    auto valid = ValidateSemanticRow(edge.input.row);
    if (!valid.ok()) {
      return valid;
    }

    key.clear();
    AppendSemanticKey(&key, edge.input.row);
    const std::uint32_t input = rows.Find(key);
    if (input == KeyInterner::kMissing ||
        (root_rows[input] == nullptr && result_rows[input] == nullptr)) {
      return Status::InvalidArgument(
          "witness cites an input that is neither a root nor a result");
    }

    const std::uint32_t rule_id = labels.Intern(edge.rule_id);
    const std::uint32_t derivation_id = labels.Intern(edge.derivation_key);
    const auto [found, inserted] = derivation_index.emplace(
        DerivationKey{result, rule_id, derivation_id}, derivations.size());
    if (inserted) {
      derivations.push_back(Derivation{
          .result = result, .rule = rule_id, .derivation = derivation_id,
          .spec = rule});
    }
    // Taken after the possible push_back: the vector's storage moves.
    Derivation& derivation = derivations[found->second];
    // With the derivation key, alternative proof steps land in separate groups,
    // so two different inputs at one ordinal indicate an engine defect rather
    // than a legal alternative proof. One input at two ordinals is the legal
    // self-join and is recorded as such.
    const auto slot = std::ranges::find_if(
        derivation.inputs, [&](const auto& entry) {
          return entry.first == edge.input_ordinal;
        });
    if (slot != derivation.inputs.end()) {
      if (slot->second != input) {
        return Status::InvalidArgument(
            "witness binds two inputs at one ordinal");
      }
    } else {
      const auto position = std::ranges::find_if(
          derivation.inputs, [&](const auto& entry) {
            return entry.first > edge.input_ordinal;
          });
      derivation.inputs.insert(position, {edge.input_ordinal, input});
    }
  }

  // Every key is interned, so the ranks -- the order the canonical output is
  // defined over -- can be fixed and the ids can become them. Nothing interns
  // past this point and nothing reads a key back except the ids already copied
  // out of the fold, so the key bytes and the lookup indices are released
  // before the relaxation and the selection walk the derivations.
  rows.Finish();
  labels.Finish();
  for (Derivation& derivation : derivations) {
    derivation.rule = labels.Rank(derivation.rule);
    derivation.derivation = labels.Rank(derivation.derivation);
  }
  rows.Release();
  labels.Release();

  std::vector<std::uint32_t> result_order(result_row_ids.size());
  std::iota(result_order.begin(), result_order.end(), std::uint32_t{0});
  std::ranges::sort(result_order, {}, [&](std::uint32_t result) {
    return rows.Rank(result_row_ids[result]);
  });

  // The derivations in the order the proof selection reads them: by result,
  // then rule, then firing key, all in ascending key order. That is the order
  // the three nested maps iterated, and it is what a tie on cost and priority
  // falls back on, so it is also the order the arity check below must report a
  // first failure in.
  std::ranges::sort(derivations,
                    [&](const Derivation& left, const Derivation& right) {
                      if (left.result != right.result) {
                        return rows.Rank(left.result) <
                               rows.Rank(right.result);
                      }
                      return DerivationOrder(left, right);
                    });

  // 4. Validate derivation arity: every derivation's input ordinals must be
  // exactly {0, ..., rule.arity - 1}. The per-edge checks above already reject a
  // duplicated ordinal, so a derivation that holds its rule's declared arity in
  // ascending distinct ordinals holds precisely those.
  for (const Derivation& derivation : derivations) {
    if (derivation.inputs.size() != derivation.spec->arity) {
      return Status::InvalidArgument(
          "witness derivation has the wrong number of inputs");
    }
    for (std::uint32_t ordinal = 0; ordinal < derivation.spec->arity;
         ++ordinal) {
      if (derivation.inputs[ordinal].first != ordinal) {
        return Status::InvalidArgument(
            "witness derivation is missing an input ordinal");
      }
    }
  }

  // 5. Relax derivation costs to a fixpoint. A root costs nothing; a derivation
  // costs one edge per input plus the cost of proving each derived input. A
  // result reachable only through a cycle never leaves kUnproven, which is what
  // rejects unrooted cycles without needing a cycle search.
  //
  // The relaxation is a worklist, not a sweep of every derivation for up to one
  // round per result. A derivation can only become cheaper when one of its
  // inputs does, so its consumers are the only edges the worklist has to
  // follow: every derivation is seeded once -- which is the sweep's first round
  // -- and every later visit is bought by a cost that strictly fell. A sweep
  // spends its passes re-deriving derivations nothing has changed.
  std::vector<std::uint64_t> cost(rows.Count(), kUnproven);
  for (std::uint32_t id = 0; id < rows.Count(); ++id) {
    if (root_rows[id] != nullptr) {
      cost[id] = 0;
    }
  }

  // For each row, the derivations that read it as an input, as one flat array:
  // a vector per row would be a header per row for a graph of out-degree one or
  // two. Counting sort over the input ordinals.
  std::vector<std::uint32_t> consumer_begin(rows.Count() + 1, 0);
  for (const Derivation& derivation : derivations) {
    for (const auto& [ordinal, input] : derivation.inputs) {
      ++consumer_begin[input + 1];
    }
  }
  for (std::size_t i = 1; i < consumer_begin.size(); ++i) {
    consumer_begin[i] += consumer_begin[i - 1];
  }
  std::vector<std::uint32_t> consumers(consumer_begin.back());
  {
    std::vector<std::uint32_t> fill(consumer_begin.begin(),
                                    consumer_begin.end() - 1);
    for (std::uint32_t index = 0; index < derivations.size(); ++index) {
      for (const auto& [ordinal, input] : derivations[index].inputs) {
        consumers[fill[input]++] = index;
      }
    }
  }

  std::vector<std::uint32_t> pending;
  std::vector<std::uint8_t> queued(derivations.size(), 0);
  auto enqueue = [&](std::uint32_t index) {
    if (queued[index] != 0) {
      return;
    }
    queued[index] = 1;
    pending.push_back(index);
  };
  for (std::uint32_t index = 0; index < derivations.size(); ++index) {
    enqueue(index);
  }
  for (std::size_t head = 0; head < pending.size(); ++head) {
    const std::uint32_t index = pending[head];
    // Cleared on the way out rather than never set, so a derivation whose
    // inputs fall again is queued a second time.
    queued[index] = 0;
    Derivation& derivation = derivations[index];
    std::uint64_t total = 0;
    bool provable = true;
    for (const auto& [ordinal, input] : derivation.inputs) {
      total += 1;
      if (cost[input] == kUnproven) {
        provable = false;
        break;
      }
      total += cost[input];
    }
    // A cost that did not fall cannot lower its result's below the result's
    // own. The invariant is `cost[derivation.result] <= derivation.cost`: a
    // derivation's cost is never below its result's, because the result's cost
    // is the cheapest of its derivations. So a total at or above this
    // derivation's cost is at or above its result's cost too, and this skip
    // loses nothing.
    if (!provable || total >= derivation.cost) {
      continue;
    }
    // The two are not set together: this lowers the derivation alone, and a
    // total below the derivation's old cost can still be at or above its
    // result's, so the result is re-tested below rather than assumed. This
    // guard is not the one above restated; dropping it and assigning
    // unconditionally would let a dearer derivation raise a result's cost over
    // a cheaper one's, changing the selected proof and both hashes.
    derivation.cost = total;
    if (total >= cost[derivation.result]) {
      continue;
    }
    cost[derivation.result] = total;
    for (std::uint32_t i = consumer_begin[derivation.result];
         i < consumer_begin[derivation.result + 1]; ++i) {
      enqueue(consumers[i]);
    }
  }

  // 6. Select one canonical proof per result: fewest derived edges, then lower
  // rule priority, then lexicographic input keys.
  CanonicalizedResult canonical;
  canonical.diagnostics = raw.diagnostics;
  std::vector<std::uint32_t> fact_order_keys;
  std::vector<WitnessOrder> witness_order;
  fact_order_keys.reserve(result_row_ids.size());
  witness_order.reserve(raw.witnesses.size());
  std::size_t next = 0;
  for (std::uint32_t result : result_order) {
    const std::uint32_t result_id = result_row_ids[result];
    // The derivations are grouped by result in rank order, so this result's run
    // is the contiguous one at the cursor.
    const std::size_t begin = next;
    while (next < derivations.size() &&
           derivations[next].result == result_id) {
      ++next;
    }
    if (cost[result_id] == kUnproven) {
      return Status::FailedPrecondition(
          "published result has no finite proof rooted in declared inputs");
    }
    const Derivation* selected = nullptr;
    for (std::size_t index = begin; index < next; ++index) {
      const Derivation& derivation = derivations[index];
      if (derivation.cost == kUnproven) {
        continue;
      }
      if (selected == nullptr || derivation.cost < selected->cost ||
          (derivation.cost == selected->cost &&
           derivation.spec->priority < selected->spec->priority)) {
        selected = &derivation;
      }
      // Equal cost and equal priority fall back to the ordered (rule id,
      // derivation key), which the run above already supplies deterministically.
    }
    if (selected == nullptr) {
      return Status::FailedPrecondition(
          "published result has no finite proof rooted in declared inputs");
    }

    const SemanticRow& row = *result_rows[result_id];
    auto fact = MakeFact(row);
    if (!fact.ok()) {
      return fact.status();
    }
    canonical.facts.push_back(std::move(*fact));
    const std::uint32_t result_rank = rows.Rank(result_id);
    fact_order_keys.push_back(result_rank);

    for (const auto& [ordinal, input] : selected->inputs) {
      const SemanticRow* input_row =
          root_rows[input] != nullptr ? root_rows[input] : result_rows[input];
      canonical.witnesses.push_back(WitnessEdge{.result = SemanticKey{row},
                                                .rule_id = selected->spec->id,
                                                .input = SemanticKey{*input_row},
                                                .input_ordinal = ordinal});
      witness_order.push_back(WitnessOrder{result_rank, selected->rule, ordinal,
                                           rows.Rank(input)});
    }
  }

  // 7. Canonical order, then the two hashes. The emitted order is already this
  // order, because the results are walked in key order and a result's witnesses
  // in ordinal order; the sorts run because the order is a property of the
  // published output rather than of the loop that filled it, and they now
  // compare the dense keys computed once above instead of encoding a row per
  // comparison.
  SortByKeys(&canonical.facts, &fact_order_keys);
  SortByKeys(&canonical.witnesses, &witness_order);

  // ExternalHash covers only what a predecessor can see: the published
  // semantics. Witness edges are deliberately excluded so re-proving a fact
  // does not schedule predecessors that cannot observe the difference.
  const auto hashes = ComputeCanonicalResultHashes(canonical.facts,
                                                   canonical.witnesses);
  canonical.fixpoint_hash = hashes.fixpoint_hash;
  canonical.external_hash = hashes.external_hash;
  return canonical;
}

}  // namespace veritas::facts
