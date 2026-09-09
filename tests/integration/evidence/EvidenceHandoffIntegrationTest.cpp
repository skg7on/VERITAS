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

// EvidenceHandoffIntegrationTest.cpp — HND-006, the cross-translation-unit
// summary fixture built in two independent checkout roots.
//
// The summary fixture's source value (packet.length) is read in entry.cpp and
// reaches the memcpy sink in copy.cpp across a function-call summary boundary.
// HND-006 requires the resulting semantic input and IDs to be deterministic
// across checkout roots and insertion orders, with summary references present
// and no source text copied into the handoff.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/FactStoreEvidenceBackend.h"
#include "evidence/RealEvidencePipeline.h"
#include "veritas/core/Ids.h"
#include "veritas/facts/ProvenanceStore.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::testing {
namespace {

std::vector<core::StableId> SortedFactIds(
    const std::vector<facts::AnalysisFact>& facts) {
  std::vector<core::StableId> ids;
  ids.reserve(facts.size());
  for (const auto& fact : facts) {
    ids.push_back(fact.fact_id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool HasRelation(const std::vector<facts::AnalysisFact>& facts,
                 facts::RelationId relation) {
  for (const auto& fact : facts) {
    if (fact.row.relation == relation) {
      return true;
    }
  }
  return false;
}

TEST(EvidenceHandoffIntegrationTest, SummaryFixtureIsDeterministicAcrossRoots) {
  // Two independent materializations in different temporary roots.
  auto first = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(first.ok()) << first.status().message();
  auto second = AnalyzeRealFixture("evidence_overflow_summary");
  ASSERT_TRUE(second.ok()) << second.status().message();

  auto first_facts = first->fact_store.GetCurrentFacts(first->run_id);
  ASSERT_TRUE(first_facts.ok()) << first_facts.status().message();
  auto second_facts = second->fact_store.GetCurrentFacts(second->run_id);
  ASSERT_TRUE(second_facts.ok()) << second_facts.status().message();

  // Determinism: identical run identity and identical canonical fact IDs
  // across the two checkout roots. The semantic identity (analysis run, facts,
  // revision/build-variant/repository) is content-addressed and therefore
  // path-independent.
  EXPECT_EQ(first->run_id, second->run_id);
  EXPECT_EQ(first->descriptor.repository, second->descriptor.repository);
  EXPECT_EQ(first->descriptor.revision, second->descriptor.revision);
  EXPECT_EQ(first->descriptor.build_variant, second->descriptor.build_variant);
  EXPECT_EQ(SortedFactIds(*first_facts), SortedFactIds(*second_facts));

  // NOTE: the CPG projection_id (and its module_hash) is NOT asserted equal
  // here. The `-g` flag embeds the absolute source path in the LLVM module's
  // debug info, so module_hash — and therefore projection_id — is
  // path-dependent across checkout roots. That is a real HND-006 gap for the
  // snapshot fingerprint (which folds module_hash into
  // cpg_projection_fingerprint), reported in the task-3 report; the semantic
  // fact identity asserted above is unaffected.

  // The source and sink cross translation units, so the analysis must have
  // crossed the call boundary: a reachable-call fact plus value flow exist.
  EXPECT_TRUE(HasRelation(*first_facts, facts::RelationId::kReachableCall));
  EXPECT_TRUE(HasRelation(*first_facts, facts::RelationId::kGlobalFlow));

  // Summary references are present in the provenance of a derived flow fact,
  // while no source text is copied into the handoff (facts are semantic rows).
  core::StableId flow_fact_id;
  for (const auto& fact : *first_facts) {
    if (fact.row.relation == facts::RelationId::kGlobalFlow) {
      flow_fact_id = fact.fact_id;
      break;
    }
  }
  ASSERT_FALSE(flow_fact_id.digest_hex.empty());

  facts::ExplainBudget budget;
  budget.max_depth = 8;
  budget.max_nodes = 128;
  facts::ProvenanceStore provenance(first->fact_store.metadata_store());
  auto graph = provenance.Explain(first->run_id, flow_fact_id, budget);
  ASSERT_TRUE(graph.ok()) << graph.status().message();

  bool saw_summary_ref = false;
  for (const auto& node : graph->nodes()) {
    if (!node.summary_id().empty()) {
      saw_summary_ref = true;
      break;
    }
  }
  EXPECT_TRUE(saw_summary_ref) << "no summary reference in provenance closure";
}

}  // namespace
}  // namespace veritas::testing
