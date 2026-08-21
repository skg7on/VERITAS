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

#ifndef VERITAS_WPA_FIXPOINT_DOMAIN_H_
#define VERITAS_WPA_FIXPOINT_DOMAIN_H_

#include <map>
#include <string>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/FactSchema.h"

namespace veritas::wpa {

struct DomainFact {
  summary::v1::EpistemicState epistemic;
  facts::FactTuple tuple;
};

using FactDomain = std::map<std::vector<std::string>, DomainFact>;

std::vector<std::string> FactSemanticKey(const facts::FactTuple &tuple);
StatusOr<bool> JoinFact(facts::FactTuple candidate, FactDomain *domain);

} // namespace veritas::wpa

#endif // VERITAS_WPA_FIXPOINT_DOMAIN_H_
