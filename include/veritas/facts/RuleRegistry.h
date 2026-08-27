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

// RuleRegistry.h — the versioned rule bundle contract.
//
// Rule IDs are part of the durable witness protocol: a published proof names
// the rule that produced it. The registry is the single authority for which
// IDs exist, which relation each derives, and their tie-break priority. The
// historical M8 engine hard-coded rule-ID string literals in two files; a
// witness naming an unregistered rule is now a validation failure.

#ifndef VERITAS_FACTS_RULE_REGISTRY_H_
#define VERITAS_FACTS_RULE_REGISTRY_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "veritas/facts/RelationSchema.h"

namespace veritas::facts {

struct RuleSpec {
  std::string id;
  // Lower sorts first when two proofs have the same derived-edge count. A rule
  // reaching a result by a more direct route outranks a transitive one.
  std::uint32_t priority;
  RelationId result;
};

class RuleRegistry {
 public:
  // Returns nullptr when the id is not registered.
  const RuleSpec* Find(std::string_view rule_id) const;
  std::span<const RuleSpec> Rules() const;
};

// The rules.v2 bundle version. Participates in run identity.
std::string_view RuleBundleVersionV2();

// Returns the immutable rules.v2 registry.
const RuleRegistry& RulesV2();

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_RULE_REGISTRY_H_
