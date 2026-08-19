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

#include "veritas/summary/LocalSummaryBuilder.h"

#include <algorithm>
#include <vector>

namespace veritas::summary {
namespace {

// Sort a vector of proto messages by serialized bytes so equivalent input
// produces byte-identical summaries regardless of insertion order.
template <typename Message>
void SortBySerialized(std::vector<Message>* items) {
  std::sort(items->begin(), items->end(), [](const Message& a, const Message& b) {
    return a.SerializeAsString() < b.SerializeAsString();
  });
}

}  // namespace

StatusOr<v1::FunctionSummary> BuildLocalSummary(
    const FunctionLocalFacts& facts,
    const build::ProgramContext& context) {
  v1::FunctionSummary summary;

  auto* header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(0);  // pinned to keep FunctionSummaryID stable

  auto* identity = summary.mutable_identity();
  identity->set_repository_id(context.repository_id);
  identity->set_revision_id(context.revision_id);
  identity->set_build_variant_id(context.build_variant_id);
  identity->set_function_variant_id(facts.function_variant_id);

  // Copy, sort, then append so serialization order is deterministic.
  std::vector<v1::Call> calls = facts.calls;
  std::vector<v1::MemoryEffect> memory_effects = facts.memory_effects;
  std::vector<v1::ValueFlow> value_flows = facts.value_flows;
  std::vector<v1::RangeFact> range_facts = facts.range_facts;
  std::vector<v1::Unknown> unknowns = facts.unknowns;
  SortBySerialized(&calls);
  SortBySerialized(&memory_effects);
  SortBySerialized(&value_flows);
  SortBySerialized(&range_facts);
  SortBySerialized(&unknowns);

  for (const auto& call : calls) {
    *summary.add_calls() = call;
  }
  for (const auto& effect : memory_effects) {
    *summary.add_memory_effects() = effect;
  }
  for (const auto& flow : value_flows) {
    *summary.add_value_flows() = flow;
  }
  for (const auto& range : range_facts) {
    *summary.add_range_facts() = range;
  }
  for (const auto& unknown : unknowns) {
    *summary.add_unknowns() = unknown;
  }

  // Basic-block summaries map one-to-one into control-flow entries; dominator
  // facts are grouped into a single trailing control-flow entry.
  for (const auto& block : facts.basic_block_summaries) {
    summary.add_control_flow()->mutable_block()->CopyFrom(block);
  }
  if (!facts.dominator_summaries.empty()) {
    auto* control_flow = summary.add_control_flow();
    for (const auto& dominator : facts.dominator_summaries) {
      control_flow->add_dominators()->CopyFrom(dominator);
    }
  }

  return summary;
}

}  // namespace veritas::summary
