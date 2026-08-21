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

#include "veritas/summary/ComponentHash.h"

#include <gtest/gtest.h>

#include "veritas/summary/FunctionSummary.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::summary {
namespace {

// Helper to create a synthetic summary with a specific range fact.
v1::FunctionSummary MakeSyntheticSummaryWithRange(int64_t min, int64_t max) {
  v1::FunctionSummary summary;

  auto *header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(1234567890);

  auto *identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id("rev:sha256:def");
  identity->set_build_variant_id("variant:sha256:ghi");
  identity->set_function_variant_id("funcvar:sha256:jkl");
  identity->set_function_body_id("funcbody:sha256:mno");

  auto *range = summary.add_range_facts();
  range->set_variable("buffer_size");
  range->set_min_value(min);
  range->set_max_value(max);
  range->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  range->set_provenance_ref("prov:1");

  return summary;
}

// Helper to create a synthetic summary with a call.
v1::FunctionSummary MakeSyntheticSummaryWithCall(const std::string &callee) {
  v1::FunctionSummary summary;

  auto *header = summary.mutable_header();
  header->set_schema_version("summary.v1");
  header->set_creation_epoch_ms(1234567890);

  auto *identity = summary.mutable_identity();
  identity->set_repository_id("repo:sha256:abc");
  identity->set_revision_id("rev:sha256:def");
  identity->set_build_variant_id("variant:sha256:ghi");
  identity->set_function_variant_id("funcvar:sha256:jkl");
  identity->set_function_body_id("funcbody:sha256:mno");

  auto *call = summary.add_calls();
  call->set_callee_symbol(callee);
  call->set_call_site_anchor_id("anchor:1");
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("prov:1");

  return summary;
}

TEST(ComponentHashTest, RangeOnlyChangeChangesOnlyRangeDigest) {
  auto before = MakeSyntheticSummaryWithRange(0, 1024);
  auto after = MakeSyntheticSummaryWithRange(0, 2048);

  auto digests_before = ComputeComponentDigests(before);
  auto digests_after = ComputeComponentDigests(after);

  ASSERT_EQ(digests_before.size(), digests_after.size());

  for (size_t i = 0; i < digests_before.size(); ++i) {
    if (digests_before[i].kind == v1::COMPONENT_KIND_RANGE_FACTS) {
      // Range component should have changed
      EXPECT_NE(digests_before[i].semantic_hash, digests_after[i].semantic_hash)
          << "Range semantic hash should change";
    } else {
      // All other components should be identical
      EXPECT_EQ(digests_before[i].semantic_hash, digests_after[i].semantic_hash)
          << "Component " << static_cast<int>(digests_before[i].kind)
          << " should not change";
    }
  }
}

TEST(ComponentHashTest, ProvenanceChangeChangesOnlyEvidenceHash) {
  auto before = MakeSyntheticSummaryWithRange(0, 1024);
  auto after = MakeSyntheticSummaryWithRange(0, 1024);

  // Change only the provenance ref
  after.mutable_range_facts(0)->set_provenance_ref("prov:2");

  auto digests_before = ComputeComponentDigests(before);
  auto digests_after = ComputeComponentDigests(after);

  for (size_t i = 0; i < digests_before.size(); ++i) {
    if (digests_before[i].kind == v1::COMPONENT_KIND_RANGE_FACTS) {
      // Semantic hash should not change
      EXPECT_EQ(digests_before[i].semantic_hash, digests_after[i].semantic_hash)
          << "Range semantic hash should not change when only provenance "
             "changes";

      // Evidence hash should change
      EXPECT_NE(digests_before[i].evidence_hash, digests_after[i].evidence_hash)
          << "Range evidence hash should change when provenance changes";
    }
  }
}

TEST(ComponentHashTest, CallChangeChangesOnlyCallDigest) {
  auto before = MakeSyntheticSummaryWithCall("foo");
  auto after = MakeSyntheticSummaryWithCall("bar");

  auto digests_before = ComputeComponentDigests(before);
  auto digests_after = ComputeComponentDigests(after);

  for (size_t i = 0; i < digests_before.size(); ++i) {
    if (digests_before[i].kind == v1::COMPONENT_KIND_CALLS) {
      EXPECT_NE(digests_before[i].semantic_hash, digests_after[i].semantic_hash)
          << "Call semantic hash should change";
    } else {
      EXPECT_EQ(digests_before[i].semantic_hash, digests_after[i].semantic_hash)
          << "Component " << static_cast<int>(digests_before[i].kind)
          << " should not change";
    }
  }
}

TEST(ComponentHashTest, ResolvedCallTargetChangesCallSemanticDigest) {
  auto before = MakeSyntheticSummaryWithCall("indirect");
  auto after = before;
  before.mutable_calls(0)->set_resolved_callee_function_variant_id(
      "funcvar:sha256:"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  after.mutable_calls(0)->set_resolved_callee_function_variant_id(
      "funcvar:sha256:"
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

  const auto before_digest =
      ComputeComponentDigest(v1::COMPONENT_KIND_CALLS, before);
  const auto after_digest =
      ComputeComponentDigest(v1::COMPONENT_KIND_CALLS, after);

  EXPECT_NE(before_digest.semantic_hash, after_digest.semantic_hash);
}

TEST(ComponentHashTest, IdenticalSummariesProduceIdenticalDigests) {
  auto summary1 = MakeSyntheticSummaryWithRange(0, 1024);
  auto summary2 = MakeSyntheticSummaryWithRange(0, 1024);

  auto digests1 = ComputeComponentDigests(summary1);
  auto digests2 = ComputeComponentDigests(summary2);

  ASSERT_EQ(digests1.size(), digests2.size());

  for (size_t i = 0; i < digests1.size(); ++i) {
    EXPECT_EQ(digests1[i].semantic_hash, digests2[i].semantic_hash);
    EXPECT_EQ(digests1[i].evidence_hash, digests2[i].evidence_hash);
    EXPECT_EQ(digests1[i].kind, digests2[i].kind);
  }
}

TEST(ComponentHashTest, ItemCountReflectsComponentSize) {
  auto summary = MakeSyntheticSummaryWithRange(0, 1024);
  summary.add_range_facts()->set_variable("other_var");

  auto digests = ComputeComponentDigests(summary);

  for (const auto &digest : digests) {
    if (digest.kind == v1::COMPONENT_KIND_RANGE_FACTS) {
      EXPECT_EQ(digest.item_count, 2) << "Should have 2 range facts";
    }
  }
}

TEST(FunctionSummaryTest, IdenticalSummariesProduceSameId) {
  auto summary1 = MakeSyntheticSummaryWithRange(0, 1024);
  auto summary2 = MakeSyntheticSummaryWithRange(0, 1024);

  auto id1 = ComputeFunctionSummaryId(summary1);
  auto id2 = ComputeFunctionSummaryId(summary2);

  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(id2.ok());
  EXPECT_EQ(*id1, *id2);
}

TEST(FunctionSummaryTest, DifferentSummariesProduceDifferentIds) {
  auto summary1 = MakeSyntheticSummaryWithRange(0, 1024);
  auto summary2 = MakeSyntheticSummaryWithRange(0, 2048);

  auto id1 = ComputeFunctionSummaryId(summary1);
  auto id2 = ComputeFunctionSummaryId(summary2);

  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(id2.ok());
  EXPECT_NE(*id1, *id2);
}

} // namespace
} // namespace veritas::summary
