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
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "veritas/facts/Witness.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::facts {
namespace {

// Canonical, length-prefixed field encoding; the same self-delimiting scheme
// the logical input hash and canonicalizer use, so concatenation is injective.
void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

// Human-readable rendering of a semantic row, for diagnostics only. A rejected
// batch has to name the row that caused the rejection: an opaque fact id and a
// bare "duplicate" leaves nothing to act on.
std::string RenderRow(const SemanticRow& row) {
  std::string out(RelationsV2().Get(row.relation).name);
  for (const auto& cell : row.cells) {
    out.push_back(' ');
    std::visit(
        [&out](const auto& value) {
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

Status EnsureDeliveryTable(summarydb::MetadataStore& store) {
  return store.Execute(std::string(kDeliveryTableSql), {});
}

Status MarkDelivered(summarydb::MetadataStore& store, std::string_view run_id,
                     std::string_view batch_id, std::string_view sink_id) {
  return store.Execute(
      "INSERT OR IGNORE INTO wpa_fact_bus_deliveries "
      "(run_id, batch_id, sink_id) VALUES (?, ?, ?)",
      {std::string(run_id), std::string(batch_id), std::string(sink_id)});
}

StatusOr<bool> IsDelivered(summarydb::MetadataStore& store,
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

}  // namespace

core::StableId DeriveBatchId(const AnalysisFactBatch& batch) {
  std::string canonical = "veritas.analysis-fact-batch.v2";
  AppendField(&canonical, core::ToString(batch.run.run_id));
  for (const auto& component : batch.expected_components) {
    AppendField(&canonical, core::ToString(component.scc_id));
    AppendField(&canonical, std::to_string(static_cast<int>(component.component)));
  }
  for (const auto& completion : batch.completed_components) {
    AppendField(&canonical, core::ToString(completion.key.scc_id));
    AppendField(&canonical,
                std::to_string(static_cast<int>(completion.key.component)));
    AppendField(&canonical, completion.result_object_key);
    AppendField(&canonical, completion.result.logical_input_hash);
    AppendField(&canonical, completion.result.fixpoint_hash);
    AppendField(&canonical, completion.result.external_hash);
  }
  for (const auto& id : batch.rooted_input_fact_ids) {
    AppendField(&canonical, core::ToString(id));
  }
  for (const auto& fact : batch.facts) {
    AppendField(&canonical, EncodeSemanticKey(fact.row));
  }
  for (const auto& edge : batch.witnesses) {
    AppendField(&canonical, EncodeSemanticKey(edge.result.row));
    AppendField(&canonical, edge.rule_id);
    AppendField(&canonical, EncodeSemanticKey(edge.input.row));
    AppendField(&canonical, std::to_string(edge.input_ordinal));
  }
  for (const auto& diagnostic : batch.diagnostics) {
    AppendField(&canonical, diagnostic);
  }
  return core::MakeStableId(
      core::IdKind::kFact,
      std::as_bytes(std::span(canonical.data(), canonical.size())));
}

AnalysisFactBatch MakeAnalysisFactBatch(const wpa::WpaRunResult& result) {
  AnalysisFactBatch batch;
  batch.run = result.run;
  batch.expected_components = result.expected_components;
  batch.completed_components = result.completed_components;
  batch.rooted_input_fact_ids = result.rooted_input_fact_ids;
  batch.rooted_input_facts = result.rooted_input_facts;
  batch.diagnostics = result.diagnostics;

  std::ranges::sort(batch.expected_components);
  // Sorted before the ownership pass below, so which component owns a shared
  // fact depends on the set of completed components rather than on the order
  // the orchestrator happened to visit them in.
  std::ranges::sort(batch.completed_components,
                    [](const auto& left, const auto& right) {
                      return left.key < right.key;
                    });
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
  for (const auto& completion : batch.completed_components) {
    std::set<std::string> overridden;
    for (const auto& fact : completion.result.facts) {
      if (owned.insert(fact.fact_id).second) {
        batch.facts.push_back(fact);
      } else {
        overridden.insert(EncodeSemanticKey(fact.row));
      }
    }
    for (const auto& edge : completion.result.witnesses) {
      if (!overridden.contains(EncodeSemanticKey(edge.result.row))) {
        batch.witnesses.push_back(edge);
      }
    }
  }

  // 2. Canonical order, and the assembly boundary where uniqueness is
  // established rather than merely checked: sorting puts equal entries
  // adjacent, so the set is collapsed here and Validate's identity checks
  // describe a property this assembler guarantees.
  std::ranges::sort(batch.facts, [](const AnalysisFact& left,
                                    const AnalysisFact& right) {
    return EncodeSemanticKey(left.row) < EncodeSemanticKey(right.row);
  });
  batch.facts.erase(
      std::ranges::unique(batch.facts,
                          [](const AnalysisFact& left,
                             const AnalysisFact& right) {
                            return left.fact_id == right.fact_id;
                          })
          .begin(),
      batch.facts.end());
  std::ranges::sort(batch.witnesses, [](const WitnessEdge& left,
                                        const WitnessEdge& right) {
    const auto left_result = EncodeSemanticKey(left.result.row);
    const auto right_result = EncodeSemanticKey(right.result.row);
    if (left_result != right_result) {
      return left_result < right_result;
    }
    if (left.rule_id != right.rule_id) {
      return left.rule_id < right.rule_id;
    }
    const auto left_input = EncodeSemanticKey(left.input.row);
    const auto right_input = EncodeSemanticKey(right.input.row);
    if (left_input != right_input) {
      return left_input < right_input;
    }
    return left.input_ordinal < right.input_ordinal;
  });
  batch.witnesses.erase(std::ranges::unique(batch.witnesses).begin(),
                        batch.witnesses.end());
  std::ranges::sort(batch.diagnostics);

  batch.batch_id = DeriveBatchId(batch);
  return batch;
}

AnalysisFactBus::AnalysisFactBus(wpa::WpaRunRepository& delivery_state)
    : delivery_state_(delivery_state) {}

void AnalysisFactBus::AddSink(std::string sink_id, AnalysisFactSink& sink) {
  sinks_.emplace_back(std::move(sink_id), &sink);
}

Status AnalysisFactBus::Validate(const AnalysisFactBatch& batch) const {
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
  for (const auto& completion : batch.completed_components) {
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
  std::set<core::StableId> fact_ids;
  std::set<std::string> published_keys;
  for (const auto& fact : batch.facts) {
    auto derived = MakeFact(fact.row);
    if (!derived.ok()) {
      return derived.status();
    }
    if (derived->fact_id != fact.fact_id) {
      return Status::FailedPrecondition("fact_id does not match its row");
    }
    if (!fact_ids.insert(fact.fact_id).second) {
      return Status::FailedPrecondition("duplicate fact_id " +
                                        core::ToString(fact.fact_id) +
                                        " for row " + RenderRow(fact.row));
    }
    published_keys.insert(EncodeSemanticKey(fact.row));
  }

  // Rooted witness closure: every published fact has a derivation, and every
  // witness leaf is either another published fact or a declared rooted input.
  std::set<std::string> witnessed_keys;
  for (const auto& edge : batch.witnesses) {
    witnessed_keys.insert(EncodeSemanticKey(edge.result.row));
  }
  for (const auto& fact : batch.facts) {
    if (!witnessed_keys.contains(EncodeSemanticKey(fact.row))) {
      return Status::FailedPrecondition("fact without a closed witness");
    }
  }

  std::set<core::StableId> roots(batch.rooted_input_fact_ids.begin(),
                                 batch.rooted_input_fact_ids.end());
  for (const auto& edge : batch.witnesses) {
    const std::string input_key = EncodeSemanticKey(edge.input.row);
    if (published_keys.contains(input_key)) {
      continue;
    }
    auto derived = MakeFact(edge.input.row);
    if (!derived.ok()) {
      return derived.status();
    }
    if (!roots.contains(derived->fact_id)) {
      return Status::FailedPrecondition("witness leaf outside the root set");
    }
  }

  // Every witness result must be a published fact.
  for (const auto& edge : batch.witnesses) {
    if (!published_keys.contains(EncodeSemanticKey(edge.result.row))) {
      return Status::FailedPrecondition(
          "witness result is not a published fact");
    }
  }

  // The witness DAG must be acyclic: every published fact's proof is a finite
  // tree rooted in declared inputs. A cycle would let a fact justify itself.
  std::map<std::string, std::vector<std::string>> dependencies;
  std::map<std::string, int> input_count;
  for (const auto& fact : batch.facts) {
    input_count[EncodeSemanticKey(fact.row)] = 0;
  }
  for (const auto& edge : batch.witnesses) {
    const std::string result_key = EncodeSemanticKey(edge.result.row);
    const std::string input_key = EncodeSemanticKey(edge.input.row);
    if (input_count.contains(input_key)) {
      dependencies[input_key].push_back(result_key);
      input_count[result_key] += 1;
    }
  }
  std::vector<std::string> ready;
  for (const auto& [key, count] : input_count) {
    if (count == 0) {
      ready.push_back(key);
    }
  }
  std::size_t processed = 0;
  for (std::size_t i = 0; i < ready.size(); ++i) {
    ++processed;
    for (const auto& dependent : dependencies[ready[i]]) {
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

Status AnalysisFactBus::Publish(AnalysisFactBatch batch) const {
  Status valid = Validate(batch);
  if (!valid.ok()) {
    return valid;
  }

  summarydb::MetadataStore& store = delivery_state_.metadata_store();
  Status schema = EnsureDeliveryTable(store);
  if (!schema.ok()) {
    return schema;
  }

  const std::string run_id = core::ToString(batch.run.run_id);
  const std::string batch_id = core::ToString(batch.batch_id);

  for (const auto& [sink_id, sink] : sinks_) {
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

}  // namespace veritas::facts
