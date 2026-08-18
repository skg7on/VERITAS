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

#include "veritas/summary/FunctionSummary.h"

#include <cstddef>
#include <span>
#include <string>

#include "veritas/core/Hash.h"

namespace veritas::summary {

veritas::StatusOr<core::StableId> ComputeFunctionSummaryId(
    const v1::FunctionSummary& summary) {
  // Serialize the entire summary to canonical bytes
  std::string serialized;
  if (!summary.SerializeToString(&serialized)) {
    return veritas::Status::Internal("Failed to serialize FunctionSummary");
  }

  // Compute the StableId from the serialized bytes
  auto bytes_span = std::as_bytes(std::span(serialized));
  return core::MakeStableId(core::IdKind::kFunctionSummary, bytes_span);
}

}  // namespace veritas::summary
