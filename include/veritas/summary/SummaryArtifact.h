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

// SummaryArtifact.h — version-neutral boundary over the FunctionSummary IR.
//
// A SummaryArtifact is either a summary.v1 or summary.v2 FunctionSummary.
// Storage and retrieval code operates on SummaryArtifact so the two schema
// versions coexist without rewriting existing V1 objects: every accessor
// dispatches on the stored schema version rather than hard-coding one proto.

#ifndef VERITAS_SUMMARY_SUMMARY_ARTIFACT_H_
#define VERITAS_SUMMARY_SUMMARY_ARTIFACT_H_

#include <cstddef>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/ComponentHash.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::summary {

// A version-neutral handle over a FunctionSummary: either the V1 message or
// the V2 message. The variant alternative is the source of truth for the
// concrete schema; SchemaVersion mirrors it for the storage layer.
using SummaryArtifact = std::variant<v1::FunctionSummary, v2::FunctionSummary>;

// The schema version recorded in the summary's header ("summary.v1" or
// "summary.v2").
std::string_view SchemaVersion(const SummaryArtifact& artifact);

// The function identity shared by both schema versions.
const v1::FunctionIdentity& Identity(const SummaryArtifact& artifact);

// Serialize an artifact with the protobuf matching its concrete schema.
StatusOr<std::vector<std::byte>> SerializeSummaryArtifact(
    const SummaryArtifact& artifact);

// Parse serialized bytes into the artifact variant whose schema matches
// schema_version. Unknown schema versions return FailedPrecondition.
StatusOr<SummaryArtifact> ParseSummaryArtifact(
    std::string_view schema_version, std::span<const std::byte> bytes);

// Version-neutral FunctionSummaryID computation, dispatching to the V1 or V2
// overload.
StatusOr<core::StableId> ComputeFunctionSummaryId(const SummaryArtifact& artifact);

// Version-neutral component-digest accessors, dispatching to the V1 or V2
// overloads in ComponentHash.h.
std::vector<ComponentDigestInfo> ComputeComponentDigests(
    const SummaryArtifact& artifact);

ComponentDigestInfo ComputeComponentDigest(v1::ComponentKind kind,
                                           const SummaryArtifact& artifact);

}  // namespace veritas::summary

#endif  // VERITAS_SUMMARY_SUMMARY_ARTIFACT_H_
