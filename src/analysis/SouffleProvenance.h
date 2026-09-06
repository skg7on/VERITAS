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

// SouffleProvenance.h — runtime verification of the generated Souffle
// provenance manifest.
//
// The build derives a canonical provenance digest over the compiled Souffle
// toolchain and writes souffle-provenance.json. Load parses that manifest,
// requires the pinned source revision, re-verifies the built executable's
// digest, and recomputes the canonical provenance digest. Any missing,
// malformed, or mismatched artifact fails before a WPA run begins.

#ifndef VERITAS_ANALYSIS_SOUFFLE_PROVENANCE_H_
#define VERITAS_ANALYSIS_SOUFFLE_PROVENANCE_H_

#include <string>

#include "veritas/core/Status.h"

namespace veritas::analysis {

// The vendored Souffle tree is committed at this revision (tag 2.5); the
// runtime requires the generated manifest to record exactly it.
inline constexpr const char* kSoufflePinnedRevision =
    "5682a9f12e2668ecdd26348fe63cc508bc0fcf47";

// Verified identity material for the compiled-Souffle production engine.
class SouffleProvenance {
 public:
  // Parses `manifest_path` (the generated souffle-provenance.json), requires
  // the pinned source revision, recomputes the SHA-256 of `souffle_executable`
  // and checks it against the recorded executable digest, and recomputes the
  // canonical provenance digest over the manifest's recorded fields. Returns
  // InvalidArgument for a malformed manifest, FailedPrecondition for a
  // revision or digest mismatch, or NotFound for a missing file.
  static StatusOr<SouffleProvenance> Load(
      const std::string& manifest_path,
      const std::string& souffle_executable_path);

  // The versioned toolchain identity derived from the verified canonical
  // digest. Replaces the former label-only "souffle-2.5-pinned" string.
  std::string toolchain_identity() const;

  const std::string& canonical_digest() const { return canonical_digest_; }
  const std::string& source_revision() const { return source_revision_; }

 private:
  SouffleProvenance() = default;

  std::string source_revision_;
  std::string canonical_digest_;
};

}  // namespace veritas::analysis

#endif  // VERITAS_ANALYSIS_SOUFFLE_PROVENANCE_H_
