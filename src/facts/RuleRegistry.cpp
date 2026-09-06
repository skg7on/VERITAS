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
constexpr std::size_t kRuleCount = 14;

using RuleTable = std::array<RuleSpec, kRuleCount>;

const RuleTable& Table() {
  static const RuleTable table = {
      RuleSpec{"wpa.reachability.direct.v2", 10, RelationId::kReachableCall,
               1},
      RuleSpec{"wpa.reachability.support.v2", 20, RelationId::kReachableCall,
               2},
      RuleSpec{"wpa.reachability.transitive.v2", 30,
               RelationId::kReachableCall, 2},
      RuleSpec{"wpa.memory.may_write.direct.v2", 10, RelationId::kMayWrite, 1},
      RuleSpec{"wpa.memory.may_write.support.v2", 20, RelationId::kMayWrite, 2},
      RuleSpec{"wpa.memory.may_write.transitive.v2", 30, RelationId::kMayWrite,
               2},
      RuleSpec{"wpa.memory.may_read.direct.v2", 10, RelationId::kMayRead, 1},
      RuleSpec{"wpa.memory.may_read.support.v2", 20, RelationId::kMayRead, 2},
      RuleSpec{"wpa.memory.may_read.transitive.v2", 30, RelationId::kMayRead,
               2},
      RuleSpec{"wpa.flow.global.local.v2", 10, RelationId::kGlobalFlow, 1},
      RuleSpec{"wpa.flow.global.parameter.v2", 10, RelationId::kGlobalFlow, 1},
      RuleSpec{"wpa.flow.global.return.v2", 10, RelationId::kGlobalFlow, 1},
      RuleSpec{"wpa.flow.global.support.v2", 20, RelationId::kGlobalFlow, 2},
      RuleSpec{"wpa.flow.global.transitive.v2", 30, RelationId::kGlobalFlow, 2},
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
