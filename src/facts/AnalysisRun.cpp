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

#include "veritas/facts/AnalysisRun.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace veritas::facts {

namespace {

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendLenPrefixed(std::vector<std::byte>& out, std::string_view text) {
  const std::uint64_t size = text.size();
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<std::byte>((size >> (i * 8)) & 0xFF));
  }
  for (const char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
  }
}

bool IsLowercaseHex64(std::string_view text) {
  if (text.size() != 64) {
    return false;
  }
  for (const char c : text) {
    const bool is_digit = c >= '0' && c <= '9';
    const bool is_lower = c >= 'a' && c <= 'f';
    if (!is_digit && !is_lower) {
      return false;
    }
  }
  return true;
}

bool IsRecognizedEngine(EngineIdentity engine) {
  switch (engine) {
  case EngineIdentity::kSouffle:
  case EngineIdentity::kCppConformance:
  case EngineIdentity::kCppEmergency:
    return true;
  }
  return false;
}

Status ValidateDescriptor(const AnalysisRunDescriptor& d) {
  if (d.revision_id.kind != core::IdKind::kRevision) {
    return Status::InvalidArgument("revision id has wrong kind");
  }
  if (d.build_variant_id.kind != core::IdKind::kBuildVariant) {
    return Status::InvalidArgument("build variant id has wrong kind");
  }
  if (d.summary_schema_version.empty() || d.relation_schema_version.empty() ||
      d.rule_bundle_version.empty() || d.model_bundle_version.empty()) {
    return Status::InvalidArgument("schema/bundle version must be non-empty");
  }
  if (!IsLowercaseHex64(d.svf_configuration_hash) ||
      !IsLowercaseHex64(d.wpa_configuration_hash)) {
    return Status::InvalidArgument(
        "configuration hash must be lowercase 64-hex");
  }
  if (!IsRecognizedEngine(d.engine)) {
    return Status::InvalidArgument("unrecognized engine identity");
  }
  if (d.engine_toolchain_identity.empty()) {
    return Status::InvalidArgument("engine toolchain identity must be non-empty");
  }
  return Status::Ok();
}

std::vector<std::byte> Canonicalize(const AnalysisRunDescriptor& d) {
  std::vector<std::byte> bytes;
  AppendLenPrefixed(bytes, "veritas.wpa-run.v1");
  AppendLenPrefixed(bytes, core::ToString(d.revision_id));
  AppendLenPrefixed(bytes, core::ToString(d.build_variant_id));
  AppendLenPrefixed(bytes, d.summary_schema_version);
  AppendLenPrefixed(bytes, d.relation_schema_version);
  AppendLenPrefixed(bytes, d.rule_bundle_version);
  AppendLenPrefixed(bytes, d.model_bundle_version);
  AppendLenPrefixed(bytes, d.svf_configuration_hash);
  AppendLenPrefixed(bytes, d.wpa_configuration_hash);
  AppendU8(bytes, static_cast<std::uint8_t>(d.engine));
  AppendLenPrefixed(bytes, d.engine_toolchain_identity);
  return bytes;
}

}  // namespace

StatusOr<AnalysisRunManifest> MakeAnalysisRun(AnalysisRunDescriptor descriptor) {
  if (Status status = ValidateDescriptor(descriptor); !status.ok()) {
    return status;
  }
  AnalysisRunManifest manifest;
  static_cast<AnalysisRunDescriptor&>(manifest) = descriptor;
  manifest.run_id =
      core::MakeStableId(core::IdKind::kAnalysisRun, Canonicalize(descriptor));
  return manifest;
}

}  // namespace veritas::facts
