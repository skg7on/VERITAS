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

// AnalysisRun.h — reproducible WPA analysis-run identity.
//
// Every WPA run is described by an engine-neutral semantic descriptor (what is
// analyzed) and an execution envelope (revision, engine, and exact
// engine/toolchain identity). MakeAnalysisRun validates both and derives a
// content-addressed run ID over the canonical, versioned, length-prefixed
// concatenation of every field.

#ifndef VERITAS_FACTS_ANALYSIS_RUN_H_
#define VERITAS_FACTS_ANALYSIS_RUN_H_

#include <cstdint>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::facts {

enum class EngineIdentity : std::uint8_t {
  kSouffle,
  kCppConformance,
  kCppEmergency,
};

// Engine-neutral subset of the run descriptor: everything that determines the
// semantic content of an analysis, excluding the execution envelope.
struct AnalysisRunSemanticDescriptor {
  core::StableId build_variant_id;
  std::string summary_schema_version;
  std::string relation_schema_version;
  std::string rule_bundle_version;
  std::string model_bundle_version;
  std::string svf_configuration_hash;
  std::string wpa_configuration_hash;
};

// Full execution-envelope descriptor: the semantic descriptor plus revision,
// engine, and exact engine/toolchain identity.
struct AnalysisRunDescriptor : AnalysisRunSemanticDescriptor {
  core::StableId revision_id;
  EngineIdentity engine;
  std::string engine_toolchain_identity;
};

// Immutable manifest: the descriptor plus its derived content-addressed ID.
struct AnalysisRunManifest : AnalysisRunDescriptor {
  core::StableId run_id;
};

// Validates the descriptor (ID kinds, non-empty versions, lowercase 64-hex
// configuration hashes, and a recognized engine) and derives the canonical
// run ID. Returns InvalidArgument on any validation failure.
StatusOr<AnalysisRunManifest> MakeAnalysisRun(AnalysisRunDescriptor descriptor);

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_ANALYSIS_RUN_H_
