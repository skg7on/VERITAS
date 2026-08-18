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

#ifndef VERITAS_SUMMARY_FUNCTION_SUMMARY_H_
#define VERITAS_SUMMARY_FUNCTION_SUMMARY_H_

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::summary {

// Compute the FunctionSummaryID from canonical summary bytes.
// The ID is derived from the entire summary's serialized form.
veritas::StatusOr<core::StableId> ComputeFunctionSummaryId(
    const v1::FunctionSummary& summary);

}  // namespace veritas::summary

#endif  // VERITAS_SUMMARY_FUNCTION_SUMMARY_H_
