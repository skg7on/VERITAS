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

#ifndef VERITAS_FACTS_FACT_SCHEMA_H_
#define VERITAS_FACTS_FACT_SCHEMA_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::facts {

enum class FactRelation {
  kDirectCall,
  kDirectRead,
  kDirectWrite,
  kLocalFlow,
  kMayAlias,
  kReachableCall,
  kMayWrite,
  kGlobalFlow,
};

struct BaseFactOrigin {
  core::StableId function_summary_id;
  std::string anchor;
  std::string provenance_ref;
};

struct FactTuple {
  core::StableId tuple_id;
  FactRelation relation;
  std::vector<std::string> columns;
  summary::v1::EpistemicState epistemic;
  std::string rule_id;
  std::vector<core::StableId> input_tuple_ids;
};

StatusOr<std::string_view> FactRelationName(FactRelation relation);
StatusOr<FactRelation> ParseFactRelation(std::string_view name);
StatusOr<std::size_t> FactRelationArity(FactRelation relation);

StatusOr<summary::v1::EpistemicState> WeakenPositiveEpistemic(
    summary::v1::EpistemicState left,
    summary::v1::EpistemicState right);

StatusOr<FactTuple> MakeBaseFact(
    FactRelation relation, std::vector<std::string> columns,
    summary::v1::EpistemicState epistemic, BaseFactOrigin origin);

StatusOr<FactTuple> MakeDerivedFact(
    FactRelation relation, std::vector<std::string> columns,
    summary::v1::EpistemicState epistemic, std::string rule_id,
    std::vector<core::StableId> input_tuple_ids);

Status ValidateFactTuple(const FactTuple& tuple);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_FACT_SCHEMA_H_
