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

#include "veritas/summary/SummaryV2Builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string_view>

#include "veritas/core/Ids.h"
#include "veritas/summary/FunctionSummary.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::summary {
namespace {

namespace semantic = veritas::analysis::semantic;

// Build a synthetic memory location with a kMemoryRef stable ID derived from
// the given name. The embedded abstract object is a stack object whose ID is a
// kAbstractObject stable ID derived from the same bytes.
semantic::MemoryLocation Memory(std::string_view name) {
  auto bytes = std::as_bytes(std::span<const char>(name.data(), name.size()));
  semantic::MemoryLocation location;
  location.id = core::MakeStableId(core::IdKind::kMemoryRef, bytes);
  location.object.id = core::MakeStableId(core::IdKind::kAbstractObject, bytes);
  location.object.kind = semantic::AbstractObjectKind::kStack;
  return location;
}

// Build a minimal, valid ProgramContext.
build::ProgramContext Context() {
  build::ProgramContext context;
  context.repository_id = "repo:sha256:abc";
  context.revision_id = "rev:sha256:def";
  context.build_variant_id = "bv:sha256:ghi";
  return context;
}

// Build a FunctionLocalFactsV2 with identity populated and no semantic records.
FunctionLocalFactsV2 MinimalFacts() {
  FunctionLocalFactsV2 facts;
  facts.function_symbol_id = "funcsym:sha256:0000000000000000000000000000000000000000000000000000000000000000";
  facts.function_variant_id = "funcvar:sha256:1111111111111111111111111111111111111111111111111111111111111111";
  return facts;
}

// The same three alias facts inserted in forward and reverse order. Equivalent
// input must produce byte-identical summaries after deterministic sorting.
FunctionLocalFactsV2 FactsInForwardOrder() {
  auto facts = MinimalFacts();
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("a"),
      .right = Memory("b"),
      .kind = semantic::AliasKind::kNoAlias,
      .epistemic = semantic::EpistemicState::kMust,
      .provenance_ref = "svf:alias:1",
  });
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("c"),
      .right = Memory("d"),
      .kind = semantic::AliasKind::kMustAlias,
      .epistemic = semantic::EpistemicState::kMay,
      .provenance_ref = "svf:alias:2",
  });
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("e"),
      .right = Memory("f"),
      .kind = semantic::AliasKind::kMayAlias,
      .epistemic = semantic::EpistemicState::kInferred,
      .provenance_ref = "svf:alias:3",
  });
  return facts;
}

FunctionLocalFactsV2 FactsInReverseOrder() {
  auto facts = MinimalFacts();
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("e"),
      .right = Memory("f"),
      .kind = semantic::AliasKind::kMayAlias,
      .epistemic = semantic::EpistemicState::kInferred,
      .provenance_ref = "svf:alias:3",
  });
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("c"),
      .right = Memory("d"),
      .kind = semantic::AliasKind::kMustAlias,
      .epistemic = semantic::EpistemicState::kMay,
      .provenance_ref = "svf:alias:2",
  });
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("a"),
      .right = Memory("b"),
      .kind = semantic::AliasKind::kNoAlias,
      .epistemic = semantic::EpistemicState::kMust,
      .provenance_ref = "svf:alias:1",
  });
  return facts;
}

TEST(SummaryV2BuilderTest, SeparatesAliasKindFromEpistemicState) {
  FunctionLocalFactsV2 facts = MinimalFacts();
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("left"),
      .right = Memory("right"),
      .kind = semantic::AliasKind::kNoAlias,
      .epistemic = semantic::EpistemicState::kMust,
      .provenance_ref = "svf:alias",
  });
  auto summary = BuildLocalSummaryV2(facts, Context());
  ASSERT_TRUE(summary.ok());
  EXPECT_EQ(summary->alias_facts(0).kind(), v2::ALIAS_KIND_NO_ALIAS);
  EXPECT_EQ(summary->alias_facts(0).epistemic(), v1::EPISTEMIC_STATE_MUST);
}

TEST(SummaryV2BuilderTest, EquivalentInputOrderHasOneSummaryId) {
  auto left = BuildLocalSummaryV2(FactsInForwardOrder(), Context());
  auto right = BuildLocalSummaryV2(FactsInReverseOrder(), Context());
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());
  EXPECT_EQ(*ComputeFunctionSummaryId(*left), *ComputeFunctionSummaryId(*right));
}

}  // namespace
}  // namespace veritas::summary
