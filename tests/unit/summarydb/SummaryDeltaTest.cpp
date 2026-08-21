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

#include "veritas/summarydb/SummaryDelta.h"

#include <gtest/gtest.h>

namespace veritas::summarydb {
namespace {

using summary::v1::ComponentKind;

// Build a synthetic summary whose only semantic content is a single range fact.
// Identity and header are pinned so two summaries differ only where the caller
// changes them, keeping component hashes deterministic.
summary::v1::FunctionSummary MakeRangeSummary(int64_t max_value) {
  summary::v1::FunctionSummary summary;

  auto* header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(0);

  auto* identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id("rev:sha256:def");
  identity->set_build_variant_id("variant:sha256:ghi");
  identity->set_function_variant_id("funcvar:sha256:jkl");
  identity->set_function_body_id("funcbody:sha256:mno");

  auto* range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(0);
  range->set_max_value(max_value);
  range->set_epistemic(summary::v1::EPISTEMIC_STATE_MUST);

  return summary;
}

// Build a synthetic summary with a single top-level provenance ref, whose
// display text can be varied. Semantic content (range fact) is fixed.
summary::v1::FunctionSummary MakeProvenanceSummary(const std::string& display) {
  summary::v1::FunctionSummary summary = MakeRangeSummary(1024);

  auto* prov = summary.add_provenance_refs();
  prov->set_id("prov:sha256:001");
  prov->set_display_text(display);

  return summary;
}

// Assert the delta reports exactly one changed component, of the given kind.
void ExpectChangedOnly(const SummaryDelta& delta, ComponentKind kind) {
  ASSERT_EQ(delta.changed_components.size(), 1u);
  EXPECT_EQ(delta.changed_components[0].component_kind, kind);
  EXPECT_TRUE(delta.changed_components[0].SemanticChanged());
}

TEST(SummaryDeltaTest, DetectsRangeOnlySemanticChange) {
  auto delta = DiffSummaries(MakeRangeSummary(10), MakeRangeSummary(20));
  ASSERT_TRUE(delta.ok()) << delta.status().message();

  ExpectChangedOnly(*delta, ComponentKind::COMPONENT_KIND_RANGE_FACTS);
  EXPECT_TRUE(delta->semantic_changed);
}

TEST(SummaryDeltaTest, IdenticalSummariesProduceNoDelta) {
  auto delta = DiffSummaries(MakeRangeSummary(1024), MakeRangeSummary(1024));
  ASSERT_TRUE(delta.ok()) << delta.status().message();

  EXPECT_TRUE(delta->changed_components.empty());
  EXPECT_FALSE(delta->semantic_changed);
  EXPECT_FALSE(delta->evidence_changed);
}

TEST(SummaryDeltaTest, DetectsEvidenceOnlyChange) {
  auto delta = DiffSummaries(MakeProvenanceSummary("step a"),
                             MakeProvenanceSummary("step b"));
  ASSERT_TRUE(delta.ok()) << delta.status().message();

  // Provenance-only change: semantic content is unchanged, evidence changed.
  EXPECT_FALSE(delta->semantic_changed);
  EXPECT_TRUE(delta->evidence_changed);
  EXPECT_TRUE(delta->HasChanged(ComponentKind::COMPONENT_KIND_PROVENANCE));
  EXPECT_FALSE(delta->HasChanged(ComponentKind::COMPONENT_KIND_RANGE_FACTS));
}

TEST(SummaryDeltaTest, RangeOnlyChangeDoesNotTouchCallGraphComponent) {
  auto delta = DiffSummaries(MakeRangeSummary(10), MakeRangeSummary(20));
  ASSERT_TRUE(delta.ok()) << delta.status().message();

  // The change is confined to RANGE_FACTS; CALLS and VALUE_FLOW are untouched.
  EXPECT_FALSE(delta->HasChanged(ComponentKind::COMPONENT_KIND_CALLS));
  EXPECT_FALSE(delta->HasChanged(ComponentKind::COMPONENT_KIND_VALUE_FLOW));
}

}  // namespace
}  // namespace veritas::summarydb
