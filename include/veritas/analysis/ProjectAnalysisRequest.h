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

// ProjectAnalysisRequest.h — the sole public source-input abstraction.
//
// A project directory is the only public entry point into VERITAS analysis.
// `output_root` is diagnostic only; it never participates in semantic
// identity. When empty, callers default it to `project_root / ".veritas"`
// during resolution.

#ifndef VERITAS_ANALYSIS_PROJECTANALYSISREQUEST_H_
#define VERITAS_ANALYSIS_PROJECTANALYSISREQUEST_H_

#include <filesystem>

namespace veritas::analysis {

struct ProjectAnalysisRequest {
  std::filesystem::path project_root;
  std::filesystem::path output_root;
};

}  // namespace veritas::analysis

#endif  // VERITAS_ANALYSIS_PROJECTANALYSISREQUEST_H_
