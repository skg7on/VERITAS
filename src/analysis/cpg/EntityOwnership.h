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

#ifndef VERITAS_ANALYSIS_CPG_ENTITYOWNERSHIP_H_
#define VERITAS_ANALYSIS_CPG_ENTITYOWNERSHIP_H_

#include <vector>

#include "analysis/pipeline/ProgramIr.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::analysis::cpg {

struct EntityOwner {
  core::StableId entity_id;
  core::StableId function_variant_id;
  auto operator<=>(const EntityOwner &) const = default;
};

// Reads only live compiler identities. Missing origins remain unowned; invalid
// or conflicting local identities fail rather than borrowing a summary owner.
StatusOr<std::vector<EntityOwner>>
CollectEntityOwners(const pipeline::ProgramIr &ir);

} // namespace veritas::analysis::cpg
#endif // VERITAS_ANALYSIS_CPG_ENTITYOWNERSHIP_H_
