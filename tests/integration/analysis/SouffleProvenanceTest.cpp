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

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/core/Hash.h"

namespace veritas::analysis {
namespace {

std::string Sha256Hex(std::string_view bytes) {
  return core::DigestToHex(core::ComputeSHA256(
      std::as_bytes(std::span(bytes.data(), bytes.size()))));
}

std::filesystem::path TempDir() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-provenance-XXXXXX")
          .string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// Builds a valid provenance manifest whose canonical digest matches its
// fields, in the exact field order WriteSouffleProvenance.cmake emits.
std::string MakeManifest(std::string_view source_revision,
                         std::string_view executable_sha256) {
  const char* fields[] = {
      "source_revision",           "souffle_executable_sha256",
      "runner_library_sha256",     "functor_library_sha256",
      "reachability_bundle_sha256", "memory_effects_bundle_sha256",
      "flow_bundle_sha256",        "effects_bundle_sha256",
      "compiler_id",
      "compiler_version",
      "compiler_path",             "system_name",
      "system_processor",          "cmake_generator",
      "build_type",                "cxx_standard",
      "cxx_flags",                 "executable_linker_flags",
  };
  const std::string dummy = "deadbeef";
  std::string canonical;
  for (const char* field : fields) {
    canonical += field;
    canonical += '=';
    if (std::string_view(field) == "source_revision") {
      canonical += source_revision;
    } else if (std::string_view(field) == "souffle_executable_sha256") {
      canonical += executable_sha256;
    } else {
      canonical += dummy;
    }
    canonical += '\n';
  }
  const std::string canonical_digest = Sha256Hex(canonical);

  std::string json = "{";
  json += "\"source_revision\":\"" + std::string(source_revision) + "\",";
  json += "\"executable_sha256\":\"" + std::string(executable_sha256) + "\",";
  for (const char* field : fields) {
    json += "\"" + std::string(field) + "\":\"";
    if (std::string_view(field) == "source_revision") {
      json += source_revision;
    } else if (std::string_view(field) == "souffle_executable_sha256") {
      json += executable_sha256;
    } else {
      json += dummy;
    }
    json += "\",";
  }
  json += "\"canonical_provenance_sha256\":\"" + canonical_digest + "\"";
  json += "}";
  return json;
}

TEST(SouffleProvenanceTest, LoadsValidManifestAndDerivesIdentity) {
  const auto dir = TempDir();
  const auto manifest = dir / "souffle-provenance.json";
  const auto executable = dir / "souffle";
  const std::string executable_bytes = "souffle-binary-bytes";
  WriteFile(executable, executable_bytes);
  WriteFile(manifest,
            MakeManifest(kSoufflePinnedRevision, Sha256Hex(executable_bytes)));

  auto provenance = SouffleProvenance::Load(manifest.string(), executable.string());
  ASSERT_TRUE(provenance.ok()) << provenance.status().message();
  EXPECT_EQ(provenance->source_revision(), kSoufflePinnedRevision);
  EXPECT_EQ(provenance->toolchain_identity(),
            "souffle-" + provenance->canonical_digest());

  std::filesystem::remove_all(dir);
}

TEST(SouffleProvenanceTest, RejectsUnsupportedRevision) {
  const auto dir = TempDir();
  const auto manifest = dir / "souffle-provenance.json";
  const auto executable = dir / "souffle";
  const std::string executable_bytes = "souffle-binary-bytes";
  WriteFile(executable, executable_bytes);
  WriteFile(manifest, MakeManifest("deadbeef", Sha256Hex(executable_bytes)));

  auto provenance = SouffleProvenance::Load(manifest.string(), executable.string());
  ASSERT_FALSE(provenance.ok());

  std::filesystem::remove_all(dir);
}

TEST(SouffleProvenanceTest, RejectsTamperedExecutable) {
  const auto dir = TempDir();
  const auto manifest = dir / "souffle-provenance.json";
  const auto executable = dir / "souffle";
  WriteFile(executable, "tampered-bytes");
  WriteFile(manifest, MakeManifest(kSoufflePinnedRevision, Sha256Hex("original")));

  auto provenance = SouffleProvenance::Load(manifest.string(), executable.string());
  ASSERT_FALSE(provenance.ok());

  std::filesystem::remove_all(dir);
}

TEST(SouffleProvenanceTest, RejectsMissingManifest) {
  const auto dir = TempDir();
  const auto executable = dir / "souffle";
  WriteFile(executable, "souffle-binary-bytes");

  auto provenance =
      SouffleProvenance::Load((dir / "missing.json").string(),
                              executable.string());
  ASSERT_FALSE(provenance.ok());

  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace veritas::analysis
