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

// AnalysisManifest.h — the typed handoff produced by M1 project ingestion.
//
// A `ProgramContext` carries every domain-separated identifier and hash the
// downstream milestones cite; a `TranslationUnitCommand` carries one file's
// normalized compilation entry. `AnalysisManifest` bundles them and is the
// only artifact passed to later stages. `ToCanonicalBytes` produces a
// deterministic, length-prefixed byte string suitable for hashing and cache
// keys; `ToDiagnosticJson` produces a human-inspectable JSON document with
// identical semantic content. Both must be byte-identical for equivalent
// input regardless of translation-unit ordering, output root, or checkout
// path.

#ifndef VERITAS_BUILD_ANALYSISMANIFEST_H_
#define VERITAS_BUILD_ANALYSISMANIFEST_H_

#include <filesystem>
#include <string>
#include <vector>

namespace veritas::build {

enum class PathRootKind {
  kRepository,
  kGenerated,
  kExternal,
  kToolchain,
};

struct TaggedPath {
  PathRootKind root_kind = PathRootKind::kRepository;
  std::string root_id;
  std::filesystem::path relative_path;
};

struct ProgramContext {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::filesystem::path project_root;
  std::string vcs_kind;
  std::string vcs_revision;
  std::string source_tree_hash;
  std::string compilation_database_hash;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
  std::string compile_options_hash;
  std::string macro_set_hash;
  std::string include_closure_hash;
  std::string type_layout_hash;
};

struct TranslationUnitCommand {
  std::string translation_unit_id;
  std::string revision_id;
  std::string build_variant_id;
  TaggedPath source_path;
  TaggedPath working_directory;
  std::vector<std::string> arguments;
  std::string command_hash;
  std::string preprocessor_hash;
};

struct AnalysisManifest {
  ProgramContext context;
  std::vector<TranslationUnitCommand> translation_units;
};

// Byte-oriented canonical encoding: length-prefixed key/value records with
// integers in network byte order. Suitable for hashing and equality checks.
// Callers must not inspect the structure; treat the return value as opaque.
std::string ToCanonicalBytes(const AnalysisManifest& manifest);
std::string ToCanonicalBytes(const TranslationUnitCommand& unit);

// Human-inspectable JSON with sorted keys, sorted translation units, and no
// checkout- or output-specific paths. Byte-identical for equivalent input.
std::string ToDiagnosticJson(const AnalysisManifest& manifest);

}  // namespace veritas::build

#endif  // VERITAS_BUILD_ANALYSISMANIFEST_H_
