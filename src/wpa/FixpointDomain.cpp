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

#include "veritas/wpa/FixpointDomain.h"

#include <algorithm>
#include <utility>

namespace veritas::wpa {
namespace {

bool CandidateProofPrecedes(const facts::FactTuple& candidate,
                            const facts::FactTuple& current) {
  const bool candidate_is_direct =
      candidate.rule_id.find(".direct.") != std::string::npos;
  const bool current_is_direct =
      current.rule_id.find(".direct.") != std::string::npos;
  if (candidate_is_direct != current_is_direct) return candidate_is_direct;
  if (candidate.input_tuple_ids != current.input_tuple_ids) {
    return std::lexicographical_compare(
        candidate.input_tuple_ids.begin(), candidate.input_tuple_ids.end(),
        current.input_tuple_ids.begin(), current.input_tuple_ids.end());
  }
  if (candidate.rule_id != current.rule_id) {
    return candidate.rule_id < current.rule_id;
  }
  return candidate.tuple_id < current.tuple_id;
}

}  // namespace

std::vector<std::string> FactSemanticKey(const facts::FactTuple& tuple) {
  std::vector<std::string> key;
  key.reserve(tuple.columns.size() + 1u);
  key.push_back(std::to_string(static_cast<int>(tuple.relation)));
  key.insert(key.end(), tuple.columns.begin(), tuple.columns.end());
  return key;
}

StatusOr<bool> JoinFact(facts::FactTuple candidate, FactDomain* domain) {
  if (domain == nullptr) {
    return Status::InvalidArgument("fact domain must not be null");
  }
  auto validation = facts::ValidateFactTuple(candidate);
  if (!validation.ok()) return validation;

  auto key = FactSemanticKey(candidate);
  auto existing = domain->find(key);
  if (existing == domain->end()) {
    domain->emplace(std::move(key),
                    DomainFact{candidate.epistemic, std::move(candidate)});
    return true;
  }

  auto weakened = facts::WeakenPositiveEpistemic(
      existing->second.epistemic, candidate.epistemic);
  if (!weakened.ok()) return weakened.status();
  if (*weakened != existing->second.epistemic) {
    existing->second = DomainFact{candidate.epistemic, std::move(candidate)};
    return true;
  }
  if (candidate.epistemic != existing->second.epistemic) return false;
  if (!CandidateProofPrecedes(candidate, existing->second.tuple)) return false;
  existing->second = DomainFact{candidate.epistemic, std::move(candidate)};
  return true;
}

}  // namespace veritas::wpa
