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

#include <utility>

namespace veritas::wpa {

std::vector<std::string> FactSemanticKey(const facts::FactTuple &tuple) {
  std::vector<std::string> key;
  key.reserve(tuple.columns.size() + 1u);
  key.push_back(std::to_string(static_cast<int>(tuple.relation)));
  key.insert(key.end(), tuple.columns.begin(), tuple.columns.end());
  return key;
}

StatusOr<bool> JoinFact(facts::FactTuple candidate, FactDomain *domain) {
  if (domain == nullptr) {
    return Status::InvalidArgument("fact domain must not be null");
  }
  auto validation = facts::ValidateFactTuple(candidate);
  if (!validation.ok())
    return validation;

  auto key = FactSemanticKey(candidate);
  auto existing = domain->find(key);
  if (existing == domain->end()) {
    domain->emplace(std::move(key),
                    DomainFact{candidate.epistemic, std::move(candidate)});
    return true;
  }

  auto weakened = facts::WeakenPositiveEpistemic(existing->second.epistemic,
                                                 candidate.epistemic);
  if (!weakened.ok())
    return weakened.status();
  if (*weakened != existing->second.epistemic) {
    existing->second = DomainFact{candidate.epistemic, std::move(candidate)};
    return true;
  }
  return false;
}

} // namespace veritas::wpa
