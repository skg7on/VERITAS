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

#ifndef VERITAS_SUMMARY_COMPONENT_HASH_H_
#define VERITAS_SUMMARY_COMPONENT_HASH_H_

#include <cstdint>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::summary {

// ComponentDigestInfo extends the proto ComponentDigest with computed hashes.
struct ComponentDigestInfo {
  v1::ComponentKind kind;
  core::SHA256Digest semantic_hash;
  core::SHA256Digest evidence_hash;
  int32_t item_count;
  int64_t payload_offset;
  int64_t payload_length;

  bool operator==(const ComponentDigestInfo&) const = default;
};

// Compute component digests for all components in a FunctionSummary.
// Semantic hashes cover the semantic content only; evidence hashes include
// provenance and display fields.
std::vector<ComponentDigestInfo> ComputeComponentDigests(
    const v1::FunctionSummary& summary);

// Compute digest for a single component kind.
ComponentDigestInfo ComputeComponentDigest(
    v1::ComponentKind kind,
    const v1::FunctionSummary& summary);

// V2 overloads preserve the same semantic/evidence split for summary.v2.
std::vector<ComponentDigestInfo> ComputeComponentDigests(
    const v2::FunctionSummary& summary);

ComponentDigestInfo ComputeComponentDigest(
    v1::ComponentKind kind,
    const v2::FunctionSummary& summary);

}  // namespace veritas::summary

#endif  // VERITAS_SUMMARY_COMPONENT_HASH_H_
