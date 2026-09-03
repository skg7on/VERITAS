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

#include "WpaFixtureHarness.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"

namespace veritas::testing {
namespace {

namespace sem = analysis::semantic;

bool HasAlias(const sem::NormalizedAnalysisFacts& facts, sem::AliasKind kind,
              sem::EpistemicState epistemic) {
  for (const auto& alias : facts.aliases) {
    if (alias.kind == kind && alias.epistemic == epistemic) {
      return true;
    }
  }
  return false;
}

bool HasScopedBudgetUnknown(const sem::NormalizedAnalysisFacts& facts,
                            std::string_view reason) {
  for (const auto& unknown : facts.unknowns) {
    if (unknown.scope == "analysis_truncated" && unknown.reason == reason &&
        unknown.epistemic == sem::EpistemicState::kUnknown) {
      return true;
    }
  }
  return false;
}

bool HasAliasObjectKind(const sem::NormalizedAnalysisFacts& facts,
                        sem::AbstractObjectKind kind) {
  for (const auto& alias : facts.aliases) {
    if (alias.left.object.kind == kind || alias.right.object.kind == kind) {
      return true;
    }
  }
  return false;
}

std::string AliasObservations(const sem::NormalizedAnalysisFacts& facts) {
  std::string text;
  for (const auto& alias : facts.aliases) {
    if (!text.empty()) {
      text += ", ";
    }
    text += std::to_string(static_cast<unsigned>(alias.kind));
    text += "/";
    text += std::to_string(static_cast<unsigned>(alias.epistemic));
  }
  return text;
}

TEST(SemanticMemorySvfTest, SourceCoversMustMayAndNoAlias) {
  auto snapshot = MapFixtureWithSvf("pointer_alias");
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, sem::AliasKind::kMustAlias,
                       sem::EpistemicState::kMust))
      << AliasObservations(snapshot->mapping.facts);
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, sem::AliasKind::kMayAlias,
                       sem::EpistemicState::kMay))
      << AliasObservations(snapshot->mapping.facts);
  EXPECT_TRUE(HasAlias(snapshot->mapping.facts, sem::AliasKind::kNoAlias,
                       sem::EpistemicState::kMust));
  EXPECT_FALSE(HasAlias(snapshot->mapping.facts, sem::AliasKind::kUnknownAlias,
                        sem::EpistemicState::kMust));
  EXPECT_TRUE(HasAliasObjectKind(snapshot->mapping.facts,
                                 sem::AbstractObjectKind::kArgument));
}

TEST(SemanticMemorySvfTest, FactBudgetCreatesScopedExplicitUnknown) {
  auto config = analysis::svf::SvfConfig::Default();
  config.max_emitted_facts = 1;
  auto snapshot = MapFixtureWithSvf("pointer_alias", config);
  ASSERT_TRUE(snapshot.ok()) << snapshot.status().message();
  EXPECT_EQ(snapshot->mapping.completion,
            analysis::svf::SvfMappingCompletion::kCompleteWithUnknowns);
  EXPECT_TRUE(HasScopedBudgetUnknown(snapshot->mapping.facts, "fact_limit"));
}

}  // namespace
}  // namespace veritas::testing
