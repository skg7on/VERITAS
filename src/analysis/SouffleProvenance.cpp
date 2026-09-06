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

#include "analysis/SouffleProvenance.h"

#include <fstream>
#include <iterator>
#include <span>
#include <string>

#include "llvm/Support/JSON.h"
#include "veritas/core/Hash.h"

namespace veritas::analysis {
namespace {

StatusOr<std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Status::NotFound("cannot open file: " + path);
  }
  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (in.bad()) {
    return Status::Internal("failed reading file: " + path);
  }
  return contents;
}

std::string Sha256Hex(std::string_view bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

StatusOr<std::string> GetStringField(const llvm::json::Object& obj,
                                     llvm::StringRef key) {
  const llvm::json::Value* value = obj.get(key);
  if (value == nullptr) {
    return Status::InvalidArgument("souffle provenance is missing field " +
                                   key.str());
  }
  auto text = value->getAsString();
  if (!text) {
    return Status::InvalidArgument("souffle provenance field " + key.str() +
                                   " is not a string");
  }
  return std::string(*text);
}

}  // namespace

StatusOr<SouffleProvenance> SouffleProvenance::Load(
    const std::string& manifest_path,
    const std::string& souffle_executable_path) {
  auto manifest_bytes = ReadFile(manifest_path);
  if (!manifest_bytes.ok()) {
    return manifest_bytes.status();
  }

  auto parsed = llvm::json::parse(*manifest_bytes);
  if (!parsed) {
    return Status::InvalidArgument("souffle provenance is malformed JSON");
  }
  const llvm::json::Object* obj = parsed->getAsObject();
  if (obj == nullptr) {
    return Status::InvalidArgument("souffle provenance is not a JSON object");
  }

  auto source_revision = GetStringField(*obj, "source_revision");
  if (!source_revision.ok()) {
    return source_revision.status();
  }
  if (*source_revision != kSoufflePinnedRevision) {
    return Status::FailedPrecondition(
        "souffle provenance records an unsupported source revision");
  }

  auto recorded_executable = GetStringField(*obj, "executable_sha256");
  if (!recorded_executable.ok()) {
    return recorded_executable.status();
  }

  auto executable_bytes = ReadFile(souffle_executable_path);
  if (!executable_bytes.ok()) {
    return executable_bytes.status();
  }
  if (Sha256Hex(*executable_bytes) != *recorded_executable) {
    return Status::FailedPrecondition(
        "souffle executable digest does not match the provenance manifest");
  }

  // Recompute the canonical provenance digest over the recorded fields, in the
  // exact order WriteSouffleProvenance.cmake emits them.
  const char* kCanonicalFields[] = {
      "source_revision",
      "souffle_executable_sha256",
      "runner_library_sha256",
      "functor_library_sha256",
      "reachability_bundle_sha256",
      "memory_effects_bundle_sha256",
      "flow_bundle_sha256",
      "compiler_id",
      "compiler_version",
      "compiler_path",
      "system_name",
      "system_processor",
      "cmake_generator",
      "build_type",
      "cxx_standard",
      "cxx_flags",
      "executable_linker_flags",
  };
  std::string canonical;
  for (const char* field : kCanonicalFields) {
    auto value = GetStringField(*obj, field);
    if (!value.ok()) {
      return value.status();
    }
    canonical += field;
    canonical += '=';
    canonical += *value;
    canonical += '\n';
  }
  const std::string recomputed_digest = Sha256Hex(canonical);

  auto recorded_canonical =
      GetStringField(*obj, "canonical_provenance_sha256");
  if (!recorded_canonical.ok()) {
    return recorded_canonical.status();
  }
  if (recomputed_digest != *recorded_canonical) {
    return Status::FailedPrecondition(
        "souffle provenance canonical digest does not match its fields");
  }

  SouffleProvenance provenance;
  provenance.source_revision_ = *source_revision;
  provenance.canonical_digest_ = *recorded_canonical;
  return provenance;
}

std::string SouffleProvenance::toolchain_identity() const {
  return "souffle-" + canonical_digest_;
}

}  // namespace veritas::analysis
