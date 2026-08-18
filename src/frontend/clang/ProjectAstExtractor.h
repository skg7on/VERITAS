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

#ifndef VERITAS_FRONTEND_CLANG_PROJECTASTEXTRACTOR_H_
#define VERITAS_FRONTEND_CLANG_PROJECTASTEXTRACTOR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace veritas::frontend::clang {

struct SourceAnchor {
  std::string file_path;
  std::int32_t start_line = 0;
  std::int32_t start_column = 0;
  std::int32_t end_line = 0;
  std::int32_t end_column = 0;
  std::string spelling_location;
  std::string expansion_location;
};

struct ExtractedFunctionDecl {
  core::StableId function_symbol_id;
  core::StableId translation_unit_id;
  std::string qualified_name;
  std::string mangled_name;
  std::string canonical_signature;
  std::string linkage_kind;
  std::string template_identity;
  SourceAnchor source_anchor;
};

struct ProjectAstIndex {
  std::vector<ExtractedFunctionDecl> declarations;
  std::size_t processed_translation_units = 0;

  const ExtractedFunctionDecl* FindByMangledName(
      std::string_view mangled_name) const;
};

class ProjectAstExtractor {
 public:
  veritas::StatusOr<ProjectAstIndex> ExtractProject(
      const build::AnalysisManifest& manifest);
};

}  // namespace veritas::frontend::clang

#endif  // VERITAS_FRONTEND_CLANG_PROJECTASTEXTRACTOR_H_
