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

#include "SourceAnchorBuilder.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"

#include "ProjectAstExtractor.h"

namespace veritas::frontend::clang {

SourceAnchor SourceAnchorBuilder::Build(
    const ::clang::SourceRange& range,
    const ::clang::SourceManager& source_manager) {
  SourceAnchor anchor;

  auto start_loc = range.getBegin();
  auto end_loc = range.getEnd();

  if (start_loc.isValid()) {
    auto spelling_loc = source_manager.getSpellingLoc(start_loc);
    auto expansion_loc = source_manager.getExpansionLoc(start_loc);

    anchor.start_line = source_manager.getSpellingLineNumber(spelling_loc);
    anchor.start_column = source_manager.getSpellingColumnNumber(spelling_loc);

    if (auto file_id = source_manager.getFileID(spelling_loc); file_id.isValid()) {
      if (const auto* file_entry = source_manager.getFileEntryForID(file_id)) {
        anchor.file_path = file_entry->getName().str();
      }
    }

    anchor.spelling_location = spelling_loc.printToString(source_manager);
    anchor.expansion_location = expansion_loc.printToString(source_manager);
  }

  if (end_loc.isValid()) {
    auto spelling_loc = source_manager.getSpellingLoc(end_loc);
    anchor.end_line = source_manager.getSpellingLineNumber(spelling_loc);
    anchor.end_column = source_manager.getSpellingColumnNumber(spelling_loc);
  }

  return anchor;
}

}  // namespace veritas::frontend::clang
