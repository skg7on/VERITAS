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
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "veritas/core/Hash.h"
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

// The location of one encoded key inside the shared arena below.
struct KeyRef {
  std::size_t offset = 0;
  std::size_t length = 0;
};

// Encoded keys for the rows currently being ordered, packed end to end.
//
// Ordering a million witness edges needs a key for each of its two endpoints,
// and one std::string per key spends a header and a separate allocation on each
// of the millions of keys. A slice carries the same bytes for a fraction of
// that. The slices compare exactly as the strings they were cut from -- both
// use char_traits<char>::compare -- so the resulting order is identical.
class KeyArena {
public:
  KeyRef Add(std::string_view key) {
    const std::size_t offset = bytes_.size();
    bytes_.append(key);
    return KeyRef{offset, key.size()};
  }

  std::string_view View(KeyRef ref) const {
    return std::string_view(bytes_).substr(ref.offset, ref.length);
  }

  // Returns the key storage to the allocator. Keys are needed only until the
  // payloads are ordered; the canonical output carries rows, not keys.
  void Release() { std::string().swap(bytes_); }

private:
  std::string bytes_;
};

struct KeyedFact {
  KeyRef key;
  AnalysisFact fact;
};

struct KeyedWitness {
  KeyRef result_key;
  std::string rule_id;
  KeyRef input_key;
  std::uint32_t input_ordinal = 0;
  WitnessEdge edge;
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
  KeyArena keys;
  std::vector<KeyedFact> keyed_facts;
  std::vector<KeyedWitness> keyed_witnesses;
  // One reusable encoding buffer and one reusable per-component key set: the
  // loop runs once per component, so neither belongs inside it.
  std::string scratch;
  std::set<std::string, std::less<>> overridden;
  for (auto &completion : batch.completed_components) {
    overridden.clear();
    for (auto &fact : completion.result.facts) {
      scratch.clear();
      AppendSemanticKey(&scratch, fact.row);
      if (owned.insert(fact.fact_id).second) {
        keyed_facts.push_back(
            KeyedFact{.key = keys.Add(scratch), .fact = std::move(fact)});
      } else {
        overridden.emplace(scratch);
      }
    }
    for (auto &edge : completion.result.witnesses) {
      scratch.clear();
      AppendSemanticKey(&scratch, edge.result.row);
      if (overridden.contains(std::string_view(scratch))) {
        continue;
      }
      const KeyRef result_key = keys.Add(scratch);
      scratch.clear();
      AppendSemanticKey(&scratch, edge.input.row);
      // The rule id is copied, not moved: the edge keeps its own, and the
      // published batch is hashed over it.
      keyed_witnesses.push_back(KeyedWitness{
          .result_key = result_key,
          .rule_id = edge.rule_id,
          .input_key = keys.Add(scratch),
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

  // 2. Canonical order, and the assembly boundary where uniqueness is
  // established rather than merely checked: sorting puts equal entries
  // adjacent, so the set is collapsed here and Validate's identity checks
  // describe a property this assembler guarantees.
  std::ranges::sort(keyed_facts,
                    [&keys](const KeyedFact &left, const KeyedFact &right) {
                      return keys.View(left.key) < keys.View(right.key);
                    });
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
  std::ranges::sort(keyed_witnesses, [&keys](const KeyedWitness &left,
                                             const KeyedWitness &right) {
    const std::string_view left_result = keys.View(left.result_key);
    const std::string_view right_result = keys.View(right.result_key);
    if (left_result != right_result) {
      return left_result < right_result;
    }
    if (left.rule_id != right.rule_id) {
      return left.rule_id < right.rule_id;
    }
    const std::string_view left_input = keys.View(left.input_key);
    const std::string_view right_input = keys.View(right.input_key);
    if (left_input != right_input) {
      return left_input < right_input;
    }
    return left.input_ordinal < right.input_ordinal;
  });
  batch.witnesses.reserve(keyed_witnesses.size());
  for (auto &keyed : keyed_witnesses) {
    batch.witnesses.push_back(std::move(keyed.edge));
  }
  std::vector<KeyedWitness>().swap(keyed_witnesses);
  batch.witnesses.erase(std::ranges::unique(batch.witnesses).begin(),
                        batch.witnesses.end());
  std::ranges::sort(batch.diagnostics);
  keys.Release();

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
  std::map<core::StableId, std::size_t> fact_index;
  for (std::size_t i = 0; i < batch.facts.size(); ++i) {
    const auto &fact = batch.facts[i];
    auto derived = DeriveFactId(fact.row);
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
    auto result = DeriveFactId(edge.result.row);
    if (!result.ok()) {
      return result.status();
    }
    auto input = DeriveFactId(edge.input.row);
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
  Status valid = Validate(batch);
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
    Status publish = sink->Publish(batch);
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
