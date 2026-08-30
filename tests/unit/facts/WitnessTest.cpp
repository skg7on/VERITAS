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

#include "veritas/facts/Witness.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/facts/RuleRegistry.h"

namespace veritas::facts {
namespace {

namespace sem = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

SemanticRow Unsupported(std::string node, std::string feature,
                        std::string policy) {
  return SemanticRow{RelationId::kUnsupportedFeature,
                     {std::move(node), std::move(feature), std::move(policy)}};
}

// Keys are concatenated from cells, so a cell boundary must not be forgeable by
// choosing adversarial cell contents. Without length prefixing, {"a","bc"} and
// {"ab","c"} would collide.
TEST(WitnessTest, AdjacentCellsCannotForgeAKey) {
  EXPECT_NE(EncodeSemanticKey(Unsupported("a", "bc", "p")),
            EncodeSemanticKey(Unsupported("ab", "c", "p")));
}

// Cells containing the encoding's own delimiters must not be able to imitate a
// field boundary.
TEST(WitnessTest, DelimiterLikeCellsRemainDistinct) {
  EXPECT_NE(EncodeSemanticKey(Unsupported("", ":1:|", "p")),
            EncodeSemanticKey(Unsupported(":1:", "|", "p")));
}

TEST(WitnessTest, EmptyAndDigitPrefixedCellsRemainDistinct) {
  EXPECT_NE(EncodeSemanticKey(Unsupported("", "01", "p")),
            EncodeSemanticKey(Unsupported("0", "1", "p")));
}

TEST(WitnessTest, UnicodeCellsSurviveEncoding) {
  EXPECT_NE(EncodeSemanticKey(Unsupported("\xCE\xBB", "x", "p")),
            EncodeSemanticKey(Unsupported("x", "\xCE\xBB", "p")));
}

// The relation is part of the key: two relations sharing a column shape are
// different facts.
TEST(WitnessTest, RelationParticipatesInTheKey) {
  const SemanticRow reachable{
      RelationId::kReachableCall,
      {FunctionId("f"), FunctionId("g"), sem::EpistemicState::kMay}};
  const SemanticRow support{
      RelationId::kSupportReachableCall,
      {FunctionId("f"), FunctionId("g"), sem::EpistemicState::kMay}};
  EXPECT_NE(EncodeSemanticKey(reachable), EncodeSemanticKey(support));
}

// Epistemic state is independent semantic content, not a formatting detail.
TEST(WitnessTest, EpistemicStateParticipatesInTheKey) {
  const SemanticRow may{
      RelationId::kReachableCall,
      {FunctionId("f"), FunctionId("g"), sem::EpistemicState::kMay}};
  const SemanticRow must{
      RelationId::kReachableCall,
      {FunctionId("f"), FunctionId("g"), sem::EpistemicState::kMust}};
  EXPECT_NE(EncodeSemanticKey(may), EncodeSemanticKey(must));
}

TEST(RuleRegistryTest, KnownRulesResolveAndUnknownRulesDoNot) {
  EXPECT_NE(RulesV2().Find("wpa.reachability.direct.v2"), nullptr);
  EXPECT_NE(RulesV2().Find("wpa.reachability.transitive.v2"), nullptr);
  EXPECT_EQ(RulesV2().Find("wpa.not.a.rule"), nullptr);
}

// Proof selection breaks ties on rule priority, so a rule that derives the
// same relation by a more direct route must sort ahead of the transitive one.
TEST(RuleRegistryTest, DirectRulesOutrankTransitiveRules) {
  const auto* direct = RulesV2().Find("wpa.reachability.direct.v2");
  const auto* transitive = RulesV2().Find("wpa.reachability.transitive.v2");
  ASSERT_NE(direct, nullptr);
  ASSERT_NE(transitive, nullptr);
  EXPECT_LT(direct->priority, transitive->priority);
  EXPECT_EQ(direct->result, RelationId::kReachableCall);
}

// The Datalog bundles in logic/ cite rule ids by hand, so the manifest and the
// compiled registry must agree. Checking it here keeps the manifest from
// becoming an unenforced copy that silently drifts.
TEST(RuleRegistryManifestTest, ManifestMatchesCompiledRegistry) {
  const std::filesystem::path manifest_path =
      std::filesystem::path(VERITAS_LOGIC_DIR) / "common" / "rules.v2.manifest";
  std::ifstream stream(manifest_path);
  ASSERT_TRUE(stream.is_open()) << "missing " << manifest_path;

  std::string version;
  std::vector<std::tuple<std::string, std::uint32_t, std::string>> rows;
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.starts_with("rule_bundle_version=")) {
      version = line.substr(std::string("rule_bundle_version=").size());
      continue;
    }
    const auto first = line.find('\t');
    ASSERT_NE(first, std::string::npos) << line;
    const auto second = line.find('\t', first + 1);
    ASSERT_NE(second, std::string::npos) << line;
    const std::string id = line.substr(0, first);
    const std::string priority_text =
        line.substr(first + 1, second - first - 1);
    const std::string relation = line.substr(second + 1);

    std::uint32_t priority = 0;
    ASSERT_FALSE(priority_text.empty()) << line;
    for (const char digit : priority_text) {
      ASSERT_TRUE(digit >= '0' && digit <= '9') << line;
      priority = priority * 10 + static_cast<std::uint32_t>(digit - '0');
    }
    rows.emplace_back(id, priority, relation);
  }

  EXPECT_EQ(version, RuleBundleVersionV2());
  const auto rules = RulesV2().Rules();
  ASSERT_EQ(rows.size(), rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i) {
    EXPECT_EQ(std::get<0>(rows[i]), rules[i].id);
    EXPECT_EQ(std::get<1>(rows[i]), rules[i].priority);
    EXPECT_EQ(std::get<2>(rows[i]),
              RelationsV2().Get(rules[i].result).name);
  }
}

}  // namespace
}  // namespace veritas::facts
