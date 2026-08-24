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

#include "veritas/summary/SummaryArtifact.h"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include "veritas/summary/FunctionSummary.h"

namespace veritas::summary {

std::string_view SchemaVersion(const SummaryArtifact& artifact) {
  return std::visit(
      [](const auto& summary) -> std::string_view {
        return summary.header().schema_version();
      },
      artifact);
}

const v1::FunctionIdentity& Identity(const SummaryArtifact& artifact) {
  return std::visit(
      [](const auto& summary) -> const v1::FunctionIdentity& {
        return summary.identity();
      },
      artifact);
}

StatusOr<std::vector<std::byte>> SerializeSummaryArtifact(
    const SummaryArtifact& artifact) {
  return std::visit(
      [](const auto& summary) -> StatusOr<std::vector<std::byte>> {
        std::string serialized;
        if (!summary.SerializeToString(&serialized)) {
          return Status::Internal("Failed to serialize summary artifact");
        }
        auto bytes_span = std::as_bytes(std::span(serialized));
        return std::vector<std::byte>(bytes_span.begin(), bytes_span.end());
      },
      artifact);
}

StatusOr<SummaryArtifact> ParseSummaryArtifact(
    std::string_view schema_version, std::span<const std::byte> bytes) {
  if (schema_version == "summary.v1") {
    v1::FunctionSummary summary;
    if (!summary.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
      return Status::Internal("Failed to parse summary.v1 artifact");
    }
    return SummaryArtifact{std::move(summary)};
  }
  if (schema_version == "summary.v2") {
    v2::FunctionSummary summary;
    if (!summary.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
      return Status::Internal("Failed to parse summary.v2 artifact");
    }
    return SummaryArtifact{std::move(summary)};
  }
  return Status::FailedPrecondition("unknown summary schema version: " +
                                    std::string(schema_version));
}

StatusOr<core::StableId> ComputeFunctionSummaryId(
    const SummaryArtifact& artifact) {
  return std::visit(
      [](const auto& summary) -> StatusOr<core::StableId> {
        return ComputeFunctionSummaryId(summary);
      },
      artifact);
}

std::vector<ComponentDigestInfo> ComputeComponentDigests(
    const SummaryArtifact& artifact) {
  return std::visit(
      [](const auto& summary) -> std::vector<ComponentDigestInfo> {
        return ComputeComponentDigests(summary);
      },
      artifact);
}

ComponentDigestInfo ComputeComponentDigest(v1::ComponentKind kind,
                                           const SummaryArtifact& artifact) {
  return std::visit(
      [kind](const auto& summary) -> ComponentDigestInfo {
        return ComputeComponentDigest(kind, summary);
      },
      artifact);
}

}  // namespace veritas::summary
