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

#ifndef VERITAS_FACTS_SUMMARYFACTBUILDER_H_
#define VERITAS_FACTS_SUMMARYFACTBUILDER_H_

#include <span>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/facts/FactSchema.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::facts {

// Converts current summaries into the canonical five base relations. Positive
// facts with unresolved or unavailable call targets, or unsupported states,
// are intentionally omitted because they remain scoped uncertainty at the
// call-graph boundary. Returns InvalidArgument for malformed summary/callee
// identities or base columns.
StatusOr<std::vector<FactTuple>>
BuildBaseFacts(std::span<const summary::v1::FunctionSummary> summaries);

} // namespace veritas::facts

#endif // VERITAS_FACTS_SUMMARYFACTBUILDER_H_
