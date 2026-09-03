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

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

core::StableId DeriveBatchId(const AnalysisFactBatch& batch) {
  std::string canonical = "veritas.analysis-fact-batch.v1";
  AppendField(&canonical, core::ToString(batch.run.run_id));
  for (const auto& component : batch.expected_components) {
    AppendField(&canonical, core::ToString(component.scc_id));
    AppendField(&canonical, std::to_string(static_cast<int>(component.component)));
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
  for (const auto& id : batch.rooted_input_fact_ids) {
    AppendField(&canonical, core::ToString(id));
  }
  return core::MakeStableId(
      core::IdKind::kFact,
      std::as_bytes(std::span(canonical.data(), canonical.size())));
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

AnalysisFactBatch MakeAnalysisFactBatch(const wpa::WpaRunResult& result) {
  AnalysisFactBatch batch;
  batch.run = result.run;
  batch.expected_components = result.expected_components;
  batch.completed_components = result.completed_components;
  batch.rooted_input_fact_ids = result.rooted_input_fact_ids;
  batch.facts = result.facts;
  batch.witnesses = result.witnesses;
  batch.diagnostics = result.diagnostics;

  std::ranges::sort(batch.expected_components);
  std::ranges::sort(batch.completed_components,
                    [](const auto& left, const auto& right) {
                      return left.key < right.key;
                    });
  std::ranges::sort(batch.rooted_input_fact_ids);
  std::ranges::sort(batch.facts, [](const AnalysisFact& left,
                                    const AnalysisFact& right) {
    return EncodeSemanticKey(left.row) < EncodeSemanticKey(right.row);
  });
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

  batch.batch_id = DeriveBatchId(batch);
  return batch;
}

AnalysisFactBus::AnalysisFactBus(wpa::WpaRunRepository& delivery_state)
    : delivery_state_(delivery_state) {}

void AnalysisFactBus::AddSink(std::string sink_id, AnalysisFactSink& sink) {
  sinks_.emplace_back(std::move(sink_id), &sink);
}

Status AnalysisFactBus::Validate(const AnalysisFactBatch& batch) const {
  // Exact expected/completed component equality.
  std::set<wpa::WpaComponentKey> expected(batch.expected_components.begin(),
                                          batch.expected_components.end());
  std::set<wpa::WpaComponentKey> completed;
  for (const auto& completion : batch.completed_components) {
    completed.insert(completion.key);
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
      return Status::FailedPrecondition("duplicate fact_id");
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
