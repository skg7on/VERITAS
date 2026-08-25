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

#include "veritas/facts/RuleRegistry.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace veritas::facts {

namespace {

// This table is the authority for the rules.v2 bundle. logic/common/
// rules.v2.manifest mirrors it for the Datalog side and is checked against
// this table by RuleRegistryManifestTest.
//
// Priorities are grouped per derived relation: a rule that reaches a result
// directly outranks one that reaches it through a successor's support, which
// outranks a locally transitive derivation. Only the ordering matters.
constexpr std::size_t kRuleCount = 6;

using RuleTable = std::array<RuleSpec, kRuleCount>;

const RuleTable& Table() {
  static const RuleTable table = {
      RuleSpec{"wpa.reachability.direct.v2", 10, RelationId::kReachableCall},
      RuleSpec{"wpa.reachability.support.v2", 20, RelationId::kReachableCall},
      RuleSpec{"wpa.reachability.transitive.v2", 30,
               RelationId::kReachableCall},
      RuleSpec{"wpa.memory.may_write.direct.v2", 10, RelationId::kMayWrite},
      RuleSpec{"wpa.memory.may_write.support.v2", 20, RelationId::kMayWrite},
      RuleSpec{"wpa.memory.may_write.transitive.v2", 30,
               RelationId::kMayWrite},
  };
  return table;
}

}  // namespace

const RuleSpec* RuleRegistry::Find(std::string_view rule_id) const {
  const auto it = std::ranges::find_if(
      Table(), [&](const RuleSpec& rule) { return rule.id == rule_id; });
  return it == Table().end() ? nullptr : &*it;
}

std::span<const RuleSpec> RuleRegistry::Rules() const { return Table(); }

std::string_view RuleBundleVersionV2() { return "rules.v2"; }

const RuleRegistry& RulesV2() {
  static const RuleRegistry registry;
  return registry;
}

}  // namespace veritas::facts
