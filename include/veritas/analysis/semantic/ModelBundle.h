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

// ModelBundle.h — versioned external function models.
//
// A ModelBundle is the immutable, content-addressed set of external-function
// models a whole-program analysis run applies. It is loaded from a
// version-annotated TSV of five tab-separated fields per row:
//
//   <model ID seed>\t<exact symbol>\t<effect kind>\t<subject>\t<epistemic>
//
// The model ID is a stable kModel identity derived from the seed, so a model
// row is content-addressed independently of its file position. Rows must be
// sorted lexicographically by (symbol, seed); duplicate model IDs, unknown
// enum tokens, control characters, and unsorted rows are rejected so the
// bundle's content hash is a faithful fingerprint of exactly one model set.

#ifndef VERITAS_ANALYSIS_SEMANTIC_MODEL_BUNDLE_H_
#define VERITAS_ANALYSIS_SEMANTIC_MODEL_BUNDLE_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::analysis::semantic {

// The kind of effect a modeled external function has on its subject. The
// subject is an opaque, human-authored symbolic name (e.g. "return", "arg0",
// "source", "destination") that the WPA materializer resolves to a stable
// subject identity; it is deliberately not typed here.
enum class ModelEffectKind : std::uint8_t {
  kRead,        // the modeled function reads the subject
  kWrite,       // the modeled function writes the subject
  kAllocate,    // the modeled function allocates memory named by the subject
  kDeallocate,  // the modeled function deallocates memory named by the subject
  kUnknown,     // opaque/unspecified effect
};

// One external-function model row.
struct FunctionModel {
  core::StableId model_id;  // kModel identity derived from the model ID seed
  std::string symbol;       // exact function symbol the model applies to
  ModelEffectKind effect;   // what the model asserts the function does
  std::string subject;      // symbolic subject of the effect
  EpistemicState epistemic; // certainty of the modeled effect
};

// A loaded, validated model bundle. Immutable after construction; Lookup
// returns spans into the internal (sorted) model table.
class ModelBundle {
 public:
  // Load the bundle from `rows` (the TSV) and `manifest` (one line,
  // `model_bundle_version=<version>`). Returns InvalidArgument on malformed
  // input (wrong field count, duplicate model IDs, unknown enum tokens,
  // control characters, unsorted rows, or a manifest/version mismatch).
  static StatusOr<ModelBundle> Load(const std::filesystem::path& rows,
                                    const std::filesystem::path& manifest);

  // The bundle version, e.g. "models.v1".
  std::string_view version() const { return version_; }

  // The 64-hex-character SHA-256 content hash: version + canonical TSV bytes.
  std::string_view hash() const { return hash_; }

  // Every model whose exact symbol matches `symbol`, in canonical row order.
  // Returns an empty span when no model matches.
  std::span<const FunctionModel> Lookup(std::string_view symbol) const;

 private:
  std::string version_;
  std::string hash_;
  std::vector<FunctionModel> models_;
  // symbol -> [begin, end) into models_.
  std::map<std::string, std::pair<std::size_t, std::size_t>> index_;
};

}  // namespace veritas::analysis::semantic

#endif  // VERITAS_ANALYSIS_SEMANTIC_MODEL_BUNDLE_H_
