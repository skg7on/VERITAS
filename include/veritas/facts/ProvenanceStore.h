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

// ProvenanceStore.h — the rooted witness DAG and its bounded explanation.
//
// A witness node records the rule and producer that derived one fact; a
// witness edge records one of that rule's inputs at its argument position.
// Explain walks the DAG from a fact back to its rooted inputs, bounded by an
// ExplainBudget, and returns a ProvenanceGraph with an explicit truncation
// marker when the budget was exceeded. It never re-runs an engine.

#ifndef VERITAS_FACTS_PROVENANCE_STORE_H_
#define VERITAS_FACTS_PROVENANCE_STORE_H_

#include <cstdint>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/fact/v1/fact.pb.h"
#include "veritas/facts/FactStore.h"

namespace veritas::facts {

namespace fact_proto = veritas::fact::v1;

// One selected (or alternative) witness for a fact's derivation.
struct FactWitness {
  core::StableId run_id;
  core::StableId output_fact_id;
  std::string witness_id;
  bool selected = true;
  ProducerKind producer_kind = ProducerKind::kWpaSouffle;
  std::string producer_id;
  std::string rule_id;
  std::string rule_version;
  std::string analyzer_run_id;
  std::string source_anchor_id;
  std::string summary_id;
  std::string description;
};

// One derivation edge: an input at its argument position.
struct FactWitnessEdge {
  core::StableId run_id;
  core::StableId output_fact_id;
  std::string witness_id;
  std::string input_kind;  // "rooted" | "derived"
  std::string input_id;
  std::uint32_t input_ordinal = 0;
};

struct ExplainBudget {
  std::uint32_t max_depth = 10;
  std::uint32_t max_nodes = 100;
  bool include_source_anchors = true;
  bool include_summary_ids = true;
  bool include_datalog_derivation = true;
};

class ProvenanceStore {
 public:
  explicit ProvenanceStore(summarydb::MetadataStore& store);

  // Deduplicating inserts, idempotent under their primary keys.
  Status PutNode(const FactWitness& node);
  Status PutEdge(const FactWitnessEdge& edge);

  // Bounded explanation. Returns NotFound if the fact or its current binding
  // is absent. The returned graph carries truncated=true and a reason when the
  // traversal hit max_depth or max_nodes.
  StatusOr<fact_proto::ProvenanceGraph> Explain(
      core::StableId run_id, core::StableId fact_id,
      const ExplainBudget& budget);

 private:
  summarydb::MetadataStore& store_;
};

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_PROVENANCE_STORE_H_
